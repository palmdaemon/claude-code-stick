#!/usr/bin/env python3
"""
StickS3 Claude Code bridge daemon.

Connects to the StickS3 via BLE NUS, exposes a Unix socket for Claude Code
hooks, and routes permission requests to the device for physical button approval.

Usage:
    ./bridge-venv/bin/python bridge/bridge.py [--debug]

The daemon runs until interrupted. Claude Code hooks connect to the Unix socket
at SOCKET_PATH to request permission decisions.
"""

import asyncio
import base64
import io
import ipaddress
import json
import logging
import os
import signal
import struct
import subprocess
import sys
import threading
import urllib.error
import urllib.request
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# ── NUS UUIDs (Nordic UART Service — same as claude-desktop-buddy firmware) ──
NUS_SERVICE_UUID = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
NUS_RX_UUID      = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"  # host → device
NUS_TX_UUID      = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"  # device → host (notify)

DEVICE_NAME_PREFIX = "Echo"        # StickS3 advertises "Echo-XXXX"
BT_SCAN_TIMEOUT    = 15.0          # seconds
# Socket in ~/.config (not /tmp) so macOS periodic cleanup of /tmp doesn't
# silently unlink it while bridge holds the FD. Hooks rely on os.path.exists
# to gate the connect; a missing path means every hook call short-circuits.
SOCKET_PATH        = os.environ.get(
    "STICKS3_SOCKET",
    os.path.expanduser("~/.config/sticks3-buddy/bridge.sock"),
)
PERMISSION_TIMEOUT = 90.0          # seconds to wait for user to press button
HEARTBEAT_INTERVAL = 8.0           # seconds between keepalive JSON to device
RECONNECT_INTERVAL = 5.0           # seconds between BLE reconnect attempts

# ── LAN ASR proxy (StickS3 streams raw PCM here; we call Dashscope) ──────────
LAN_PORT           = int(os.environ.get("STICKS3_LAN_PORT", 8765))
DASHSCOPE_URL      = "https://dashscope.aliyuncs.com/api/v1/services/aigc/multimodal-generation/generation"

# Context biasing: per Dashscope docs, the system message's text field
# carries background info / proper-noun hints to bias recognition. We pass
# project-specific jargon so "Claude Code" stops getting transcribed as
# "Crawl Code" and similar homophones.
ASR_CONTEXT = (
    "Claude Code 是 Anthropic 官方 CLI 工具，简称 claude。"
    "Echo 是基于 ESP32-S3 的 Claude Code 物理硬件配件。"
    "常用术语：Claude Code、claude、Echo、M5Stack、StickS3、ESP32、"
    "BLE、WiFi、dashscope、buddy、clawd、bridge、hook、daemon、"
    "Anthropic、prompt、asr、emotion。"
)
DASHSCOPE_API_KEY  = os.environ.get("DASHSCOPE_API_KEY", "").strip()
DASHSCOPE_KEYFILE  = os.path.expanduser("~/.config/sticks3-buddy/dashscope_key")
if not DASHSCOPE_API_KEY and os.path.exists(DASHSCOPE_KEYFILE):
    with open(DASHSCOPE_KEYFILE) as _f:
        DASHSCOPE_API_KEY = _f.read().strip()

log = logging.getLogger("sticks3-bridge")

# Tool-name → clawd buddy state. Mapping follows clawd-tank's READme
# convention so individual Claude tools get distinct buddy animations.
# Unknown tools fall back to working_thinking.
TOOL_TO_STATE: dict[str, str] = {
    "Edit":           "working_typing",
    "Write":          "working_typing",
    "NotebookEdit":   "working_typing",
    "Read":           "working_debugger",
    "Grep":           "working_debugger",
    "Glob":           "working_debugger",
    "Bash":           "working_building",
    "Task":           "working_conducting",   # subagent
    "Agent":          "working_conducting",
    "WebSearch":      "working_wizard",
    "WebFetch":       "working_wizard",
    "LSP":            "working_beacon",
}

def state_for_tool(name: str) -> str:
    if not name:
        return "working_thinking"
    # MCP/LSP all start with "mcp__"; treat as beacon.
    if name.startswith("mcp__"):
        return "working_beacon"
    return TOOL_TO_STATE.get(name, "working_thinking")

# After this idle window, buddy returns to "idle" GIF.
IDLE_AFTER_S = 6.0

# Dashscope qwen3-asr-flash returns one of 7 emotion labels alongside the
# transcript. We map each to a distinct clawd GIF state so the buddy
# reacts in character to what was said. neutral = don't change state.
EMOTION_TO_STATE: dict[str, str] = {
    "happy":     "happy",
    "sad":       "dizzy",
    "angry":     "working_overheated",
    "surprised": "working_confused",
    "fearful":   "going_away",
    "disgusted": "working_sweeping",
    # "neutral" intentionally omitted — leave whatever state we're in.
}

# How long an ASR-emotion state lingers before reverting to idle.
EMOTION_PULSE_S = 4.0


# ─────────────────────────────────────────────────────────────────────────────
# BLE NUS transport
# ─────────────────────────────────────────────────────────────────────────────

class NusTransport:
    def __init__(self):
        self._client = None
        self._rx_buf = bytearray()
        self._rx_event = asyncio.Event()
        self._closed = True
        self._address = None

    async def connect(self) -> bool:
        try:
            from bleak import BleakScanner, BleakClient
        except ImportError:
            log.error("bleak not installed; run:  bridge-venv/bin/pip install bleak")
            return False

        # Tear down any previous client before a fresh attempt.
        if self._client is not None:
            try:
                await self._client.disconnect()
            except Exception:
                pass
            self._client = None
        self._closed = True
        self._rx_buf.clear()
        self._rx_event.clear()

        def _is_sticks3(device, adv_data) -> bool:
            if NUS_SERVICE_UUID.lower() in [u.lower() for u in adv_data.service_uuids]:
                return True
            return bool(device.name and device.name.startswith(DEVICE_NAME_PREFIX))

        log.info("BLE: scanning for %s* (timeout %.0fs)…", DEVICE_NAME_PREFIX, BT_SCAN_TIMEOUT)
        try:
            device = await BleakScanner.find_device_by_filter(_is_sticks3, timeout=BT_SCAN_TIMEOUT)
        except Exception as e:
            log.error("BLE: scan failed: %s", e)
            return False
        if device is None:
            log.warning("BLE: StickS3 not found — is Bluetooth on and device advertising?")
            return False

        log.info("BLE: found %s (%s)", device.name, device.address)
        self._address = device.address
        self._client = BleakClient(device, disconnected_callback=self._on_disconnect)
        try:
            await self._client.connect()
            await self._client.start_notify(NUS_TX_UUID, self._on_notify)
        except Exception as e:
            log.error("BLE: connect/start_notify failed: %s", e)
            try:
                await self._client.disconnect()
            except Exception:
                pass
            self._client = None
            return False

        self._closed = False
        log.info("BLE: connected to %s", device.name)
        return True

    async def readline(self) -> bytes:
        while True:
            if b"\n" in self._rx_buf:
                idx = self._rx_buf.index(b"\n")
                line = bytes(self._rx_buf[: idx + 1])
                del self._rx_buf[: idx + 1]
                return line
            if self._closed:
                return b""
            self._rx_event.clear()
            if b"\n" in self._rx_buf or self._closed:
                continue
            await self._rx_event.wait()

    async def write_line(self, obj: dict) -> None:
        if self._closed or self._client is None:
            return
        data = (json.dumps(obj, separators=(",", ":")) + "\n").encode()
        mtu = getattr(self._client, "mtu_size", 23)
        chunk = max(1, min(mtu - 3, 180))
        for i in range(0, len(data), chunk):
            await self._client.write_gatt_char(NUS_RX_UUID, data[i:i+chunk], response=False)

    async def aclose(self) -> None:
        self._closed = True
        self._rx_event.set()
        client = self._client
        self._client = None
        if client:
            try:
                await client.disconnect()
            except Exception:
                pass

    @property
    def is_connected(self) -> bool:
        return not self._closed and self._client is not None and self._client.is_connected

    def _on_notify(self, _handle, data: bytearray) -> None:
        self._rx_buf.extend(data)
        self._rx_event.set()

    def _on_disconnect(self, _client) -> None:
        log.warning("BLE: disconnected")
        self._closed = True
        self._rx_event.set()


# ─────────────────────────────────────────────────────────────────────────────
# Bridge core
# ─────────────────────────────────────────────────────────────────────────────

def _enumerate_local_ipv4() -> list[str]:
    """Return all non-loopback IPv4 addresses on this host. macOS-friendly
    via `ifconfig`; falls back to a single hostname lookup if that fails."""
    try:
        out = subprocess.check_output(["ifconfig"], text=True, timeout=2)
    except Exception:
        try:
            import socket
            return [socket.gethostbyname(socket.gethostname())]
        except Exception:
            return []
    ips = []
    for line in out.splitlines():
        line = line.strip()
        if line.startswith("inet ") and not line.startswith("inet 127."):
            parts = line.split()
            if len(parts) >= 2:
                ips.append(parts[1])
    return ips


def _pick_mac_ip_for(esp_ip: str) -> str | None:
    """Pick the local IPv4 in the same /24 subnet as the ESP32. advisor
    explicitly warned against the 8.8.8.8/default-route trick — that picks
    Ethernet/VPN when ESP32 is on Wi-Fi. Same-subnet match is the right cue."""
    try:
        esp_net = ipaddress.IPv4Network(f"{esp_ip}/24", strict=False)
    except ValueError:
        return None
    for ip in _enumerate_local_ipv4():
        try:
            if ipaddress.IPv4Address(ip) in esp_net:
                return ip
        except ValueError:
            continue
    return None


def _pcm_to_wav(pcm_bytes: bytes, sample_rate: int) -> bytes:
    n = len(pcm_bytes)
    buf = io.BytesIO()
    buf.write(b"RIFF")
    buf.write(struct.pack("<I", 36 + n))
    buf.write(b"WAVEfmt ")
    # PCM format (1), 1 channel, sample_rate, byte_rate, block_align, bits
    buf.write(struct.pack("<IHHIIHH", 16, 1, 1, sample_rate, sample_rate * 2, 2, 16))
    buf.write(b"data")
    buf.write(struct.pack("<I", n))
    buf.write(pcm_bytes)
    return buf.getvalue()


def _dashscope_asr(pcm_bytes: bytes, sample_rate: int) -> tuple[str, str]:
    """Synchronous call to qwen3-asr-flash. Returns (text, emotion).
    Raises on transport/HTTP errors so the HTTP handler can surface 502."""
    if not DASHSCOPE_API_KEY:
        raise RuntimeError("DASHSCOPE_API_KEY not set (env or ~/.config/sticks3-buddy/dashscope_key)")
    wav = _pcm_to_wav(pcm_bytes, sample_rate)
    b64 = base64.b64encode(wav).decode()
    body = {
        "model": "qwen3-asr-flash",
        "input": {
            "messages": [
                {"role": "system", "content": [{"text": ASR_CONTEXT}]},
                {"role": "user", "content": [{"audio": f"data:audio/wav;base64,{b64}"}]},
            ],
        },
        "parameters": {"asr_options": {"enable_itn": False, "language": "zh"}},
    }
    req = urllib.request.Request(
        DASHSCOPE_URL,
        data=json.dumps(body).encode(),
        headers={
            "Authorization": f"Bearer {DASHSCOPE_API_KEY}",
            "Content-Type": "application/json",
        },
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=15) as resp:
        data = json.loads(resp.read().decode())
    out = data["output"]["choices"][0]["message"]
    contents = out.get("content") or [{}]
    text = contents[0].get("text", "") if contents else ""
    emotion = ""
    anns = out.get("annotations") or []
    if anns:
        emotion = anns[0].get("emotion", "")
    return text, emotion


def _make_audio_handler(bridge):
    """Closure binds the live Bridge so the threaded HTTP handler can
    schedule clipboard injection back on the asyncio loop."""
    class AudioHandler(BaseHTTPRequestHandler):
        def do_POST(self):
            if self.path != "/audio":
                self.send_response(404); self.end_headers(); return
            try:
                length = int(self.headers.get("Content-Length", "0"))
                sr = int(self.headers.get("X-Sample-Rate", "16000"))
            except ValueError:
                self.send_response(400); self.end_headers(); return
            pcm = self.rfile.read(length)
            log.info("Audio: %d bytes @ %d Hz from %s", length, sr, self.client_address[0])
            try:
                text, emotion = _dashscope_asr(pcm, sr)
            except urllib.error.HTTPError as e:
                detail = e.read().decode("utf-8", "replace")[:200]
                log.error("Audio: dashscope HTTP %d: %s", e.code, detail)
                resp = json.dumps({"error": f"dashscope {e.code}", "detail": detail}).encode()
                self.send_response(502); self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(resp))); self.end_headers()
                self.wfile.write(resp); return
            except Exception as e:
                log.error("Audio: asr error: %s", e)
                resp = json.dumps({"error": str(e)}).encode()
                self.send_response(502); self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(resp))); self.end_headers()
                self.wfile.write(resp); return

            log.info("Audio: ASR text=%r emotion=%s", text, emotion)
            # Schedule the paste on the asyncio loop (cross-thread). Don't
            # block the response on it — ESP32 doesn't care if paste is done.
            if text and bridge._loop:
                asyncio.run_coroutine_threadsafe(
                    bridge._inject_text(text), bridge._loop
                )
            # Pulse a matching emotion-state to the buddy. Runs in
            # parallel with the paste; ESP32 picks up the BLE notify
            # right after recorder.cpp returns control to main loop.
            if emotion and bridge._loop:
                asyncio.run_coroutine_threadsafe(
                    bridge._emotion_pulse(emotion), bridge._loop
                )
            resp = json.dumps({"text": text, "emotion": emotion},
                              ensure_ascii=False).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(resp)))
            self.end_headers()
            self.wfile.write(resp)

        def log_message(self, fmt, *args):
            # Silence default access logs — we log relevant events ourselves.
            pass

    return AudioHandler


class Bridge:
    def __init__(self):
        self._nus = NusTransport()
        self._server = None
        self._shutdown = False
        self._pending: dict[str, asyncio.Future] = {}  # id → Future[decision]
        self._session_stats = {
            "total": 0, "running": 0, "waiting": 0,
            "completed": False, "tokens": 0,
        }
        # Buddy character state tracking
        self._char_state: str = "idle"
        self._last_tool_at: float = 0.0
        # Active project — last cwd basename a hook fired from. Shows up
        # in Echo's HUD line so user sees which project just triggered.
        self._project: str = ""
        # Active permission prompt — set in _handle_permission, cleared on
        # decision/timeout. Heartbeat & state_update must preserve this so
        # they don't accidentally clear an in-flight prompt on the device.
        self._active_prompt: dict | None = None
        # LAN ASR proxy
        self._http_server: ThreadingHTTPServer | None = None
        self._http_thread: threading.Thread | None = None
        self._loop: asyncio.AbstractEventLoop | None = None

    # ── lifecycle ──────────────────────────────────────────────────

    async def run(self) -> None:
        # Cache the asyncio loop so the threaded HTTP handler can dispatch
        # back to it (clipboard injection runs on the asyncio side).
        self._loop = asyncio.get_running_loop()
        # Unix socket starts once and survives reconnects so hooks can always
        # reach the bridge (they'll just get no_device while BLE is down).
        await self._start_socket()
        self._start_http_server()

        while not self._shutdown:
            connected = await self._nus.connect()
            if not connected:
                if self._shutdown:
                    break
                log.info("BLE: reconnect in %.0fs…", RECONNECT_INTERVAL)
                await asyncio.sleep(RECONNECT_INTERVAL)
                continue

            await self._run_session()

            # Session ended (disconnect or shutdown). Fail-fast any in-flight
            # permission prompts so hooks don't wait for the 90s timeout.
            self._fail_pending_permissions()

            if self._shutdown:
                break
            log.info("BLE: reconnect in %.0fs…", RECONNECT_INTERVAL)
            await asyncio.sleep(RECONNECT_INTERVAL)

    async def _run_session(self) -> None:
        """Run reader + heartbeat until BLE drops; cancel both on exit."""
        reader_task = asyncio.create_task(self._device_reader())
        hb_task = asyncio.create_task(self._heartbeat_loop())
        try:
            done, pending = await asyncio.wait(
                {reader_task, hb_task},
                return_when=asyncio.FIRST_COMPLETED,
            )
            for t in pending:
                t.cancel()
                try:
                    await t
                except (asyncio.CancelledError, Exception):
                    pass
            # Surface any unexpected error from the completed task.
            for t in done:
                if t.exception():
                    log.error("BLE: session task error: %s", t.exception())
        except Exception as e:
            log.error("BLE: session error: %s", e)

    def _fail_pending_permissions(self) -> None:
        if not self._pending:
            return
        log.warning(
            "BLE: failing %d pending permission(s) due to disconnect",
            len(self._pending),
        )
        for fut in list(self._pending.values()):
            if not fut.done():
                fut.set_result("__disconnect__")

    async def _start_socket(self) -> None:
        os.makedirs(os.path.dirname(SOCKET_PATH), exist_ok=True)
        if os.path.exists(SOCKET_PATH):
            os.unlink(SOCKET_PATH)
        self._server = await asyncio.start_unix_server(
            self._handle_hook_client, path=SOCKET_PATH
        )
        os.chmod(SOCKET_PATH, 0o666)
        log.info("IPC: listening on %s", SOCKET_PATH)

    def _start_http_server(self) -> None:
        if not DASHSCOPE_API_KEY:
            log.warning("LAN ASR: DASHSCOPE_API_KEY not set; /audio will 502. "
                        "Set env var or write to %s", DASHSCOPE_KEYFILE)
        try:
            self._http_server = ThreadingHTTPServer(
                ("0.0.0.0", LAN_PORT), _make_audio_handler(self)
            )
        except OSError as e:
            log.error("LAN ASR: bind :%d failed (%s) — port in use?", LAN_PORT, e)
            return
        self._http_thread = threading.Thread(
            target=self._http_server.serve_forever, daemon=True
        )
        self._http_thread.start()
        log.info("LAN ASR: listening on 0.0.0.0:%d/audio", LAN_PORT)

    async def stop(self) -> None:
        self._shutdown = True
        if self._server:
            self._server.close()
        if self._http_server:
            # shutdown() blocks until serve_forever returns; do it on the
            # http thread itself via call_soon-equivalent (it's a stdlib
            # method that just sets a flag — safe from any thread).
            self._http_server.shutdown()
            self._http_server.server_close()
        await self._nus.aclose()
        # Don't unlink the socket file on stop — if a new bridge has
        # already started and recreated it during a restart race, deleting
        # the file would orphan the new bridge's listener. Next bridge's
        # _start_socket() unlinks any stale file before listening.
        # Cancel all pending permission futures.
        for fut in self._pending.values():
            if not fut.done():
                fut.cancel()

    # ── device reader: parse lines from StickS3 ───────────────────

    async def _device_reader(self) -> None:
        while self._nus.is_connected:
            line = await self._nus.readline()
            if not line:
                break
            try:
                msg = json.loads(line.decode().strip())
            except Exception:
                continue
            await self._handle_device_msg(msg)
        log.info("Device reader stopped.")

    async def _handle_device_msg(self, msg: dict) -> None:
        cmd = msg.get("cmd")
        if cmd == "permission":
            pid = msg.get("id", "")
            decision = msg.get("decision", "deny")
            fut = self._pending.get(pid)
            if fut and not fut.done():
                fut.set_result(decision)
                log.info("Device decision: id=%s decision=%s", pid, decision)
        elif cmd == "hello":
            # ESP32 boot/reconnect handshake. Reply with the LAN IP that
            # shares its subnet so its asr_client can POST audio to us.
            esp_ip = msg.get("my_ip", "")
            mac_ip = _pick_mac_ip_for(esp_ip) if esp_ip else None
            if mac_ip:
                await self._nus.write_line({
                    "cmd": "init", "mac_ip": mac_ip, "mac_port": LAN_PORT
                })
                log.info("Hello from %s → init mac_ip=%s port=%d", esp_ip, mac_ip, LAN_PORT)
            else:
                log.warning("Hello from %s but no matching LAN IP found (local IPs: %s)",
                            esp_ip, _enumerate_local_ipv4())
        elif cmd == "asr":
            text = msg.get("text", "")
            emotion = msg.get("emotion", "")
            log.info("ASR text: %r emotion=%s", text, emotion)
            if text:
                await self._inject_text(text)
        elif cmd == "send_enter":
            log.info("Send Enter")
            await self._press_enter()
        else:
            log.debug("Device msg: %s", msg)

    # ── macOS text injection helpers ──────────────────────────────

    async def _inject_text(self, text: str) -> None:
        """Paste `text` to the focused window via clipboard + ⌘V.

        Backs up the current clipboard, swaps in the ASR text, sends
        Cmd-V via osascript, waits for osascript to exit AND for the
        target app to consume the paste, then restores the original
        clipboard. The osascript-exit + sleep pair is critical: the
        first cold invocation of System Events on macOS can take
        500-800ms (permission prompt, helper boot), and restoring the
        clipboard before the target app actually reads it leaves the
        wrong content pasted.
        """
        import shutil
        if not shutil.which("pbcopy") or not shutil.which("osascript"):
            log.warning("Not on macOS (pbcopy/osascript missing); skipping paste")
            return
        try:
            # Backup current clipboard.
            backup_proc = await asyncio.create_subprocess_exec(
                "pbpaste",
                stdout=asyncio.subprocess.PIPE,
            )
            backup, _ = await backup_proc.communicate()
            # Write new text to clipboard.
            copy_proc = await asyncio.create_subprocess_exec(
                "pbcopy",
                stdin=asyncio.subprocess.PIPE,
            )
            await copy_proc.communicate(input=text.encode("utf-8"))
            # Send Cmd-V to focused app. Await osascript to actually exit
            # (subprocess_exec alone returns once forked, before the script
            # runs) — otherwise on a cold System Events boot the keystroke
            # may not have fired by the time we restore the clipboard.
            keystroke = await asyncio.create_subprocess_exec(
                "osascript", "-e",
                'tell application "System Events" to keystroke "v" using command down',
            )
            await keystroke.wait()
            # The keystroke is now queued in macOS; give the target app
            # time to consume it (paste delivery + clipboard read).
            await asyncio.sleep(0.5)
            # Restore original clipboard.
            restore_proc = await asyncio.create_subprocess_exec(
                "pbcopy",
                stdin=asyncio.subprocess.PIPE,
            )
            await restore_proc.communicate(input=backup)
        except Exception as e:
            log.error("inject_text failed: %s", e)

    async def _emotion_pulse(self, emotion: str) -> None:
        """Briefly show an emotion-mapped buddy state, then revert to idle.
        If a Claude tool fires mid-pulse and changes self._char_state to
        something else, we leave it alone (don't stomp on real activity)."""
        state = EMOTION_TO_STATE.get(emotion)
        if not state:
            return  # neutral / unknown — keep current state
        import time
        self._char_state = state
        # Push tool-decay clock forward so the 6s auto-decay in _send_state
        # doesn't immediately undo us on the next heartbeat.
        self._last_tool_at = time.time()
        try:
            await self._send_state()
        except Exception as e:
            log.warning("Emotion pulse send failed: %s", e)
            return
        await asyncio.sleep(EMOTION_PULSE_S)
        # Revert only if we're still showing the emotion (no tool overrode us).
        if self._char_state == state:
            self._char_state = "idle"
            try:
                await self._send_state()
            except Exception as e:
                log.warning("Emotion revert send failed: %s", e)

    async def _press_enter(self) -> None:
        try:
            await asyncio.create_subprocess_exec(
                "osascript", "-e",
                'tell application "System Events" to key code 36',  # 36 = Return
            )
        except Exception as e:
            log.error("press_enter failed: %s", e)

    # ── heartbeat: keep device display alive ──────────────────────

    async def _heartbeat_loop(self) -> None:
        while self._nus.is_connected:
            try:
                await self._send_state()
            except Exception as e:
                log.warning("Heartbeat write failed: %s", e)
                return
            await asyncio.sleep(HEARTBEAT_INTERVAL)

    async def _send_state(self, prompt: dict | None = None) -> None:
        payload = dict(self._session_stats)
        # Three-way semantics:
        #   prompt={"id":...} → set/replace active prompt
        #   prompt={}         → explicitly clear active prompt
        #   prompt=None       → heartbeat / unrelated update — preserve active
        # Without this, the 8s heartbeat would push prompt={} and wipe a
        # live permission UI on the device before the user gets to press.
        if prompt is not None:
            self._active_prompt = prompt or None
        payload["prompt"] = self._active_prompt or {}
        # HUD msg stays fixed: "project name" UX was inconsistent (BLE notify
        # stale + multi-session race kept overwriting it). _project still
        # tracked for debug logs but not pushed to Echo screen.
        payload["msg"] = "Echo"

        # Auto-decay to idle: if no tool fired in the last IDLE_AFTER_S
        # seconds (and not currently in a notification overlay), set state
        # back to idle so the buddy stops "working" forever.
        import time
        now = time.time()
        if (self._char_state != "idle"
                and self._char_state != "notification"
                and self._last_tool_at
                and (now - self._last_tool_at) > IDLE_AFTER_S):
            self._char_state = "idle"
        payload["char_state"] = self._char_state
        await self._nus.write_line(payload)

    # ── Unix socket: handle incoming hook requests ────────────────

    async def _handle_hook_client(
        self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter
    ) -> None:
        try:
            data = await asyncio.wait_for(reader.read(65536), timeout=10.0)
            request = json.loads(data.decode().strip())
            response = await self._dispatch(request)
            writer.write(json.dumps(response).encode() + b"\n")
            await writer.drain()
        except asyncio.TimeoutError:
            log.warning("IPC: read timeout")
        except Exception as e:
            log.error("IPC: error: %s", e)
        finally:
            writer.close()
            try:
                await writer.wait_closed()
            except Exception:
                pass

    async def _dispatch(self, request: dict) -> dict:
        action = request.get("action")
        if action == "permission_request":
            return await self._handle_permission(request)
        if action == "state_update":
            self._session_stats.update({
                k: request[k] for k in
                ("total", "running", "waiting", "completed", "tokens")
                if k in request
            })
            # Map tool name → character state for buddy GIF.
            tool = request.get("tool_name", "")
            if tool:
                import time
                self._char_state = state_for_tool(tool)
                self._last_tool_at = time.time()
                log.info("Tool %s → state %s", tool, self._char_state)
            # Record the project this hook fired from (cwd basename)
            cwd = request.get("cwd", "")
            if cwd:
                proj = os.path.basename(cwd.rstrip("/")) or cwd
                # ASCII-only fallback: Echo's default font can't render CJK.
                # If the basename has non-ASCII chars, keep just the ASCII
                # prefix so HUD shows something sensible instead of "????".
                ascii_proj = "".join(c for c in proj if ord(c) < 128)
                new_project = ascii_proj or "(non-ascii)"
                if new_project != self._project:
                    log.info("Project: %s (cwd=%s)", new_project, cwd)
                    self._project = new_project
            await self._send_state()
            return {"status": "ok"}
        if action == "wifi_add":
            # Push a WiFi credential pair through to Echo over BLE NUS.
            # ESP32 stores it in NVS and re-runs the connect cycle. Requires
            # BLE to already be linked — first-time provisioning still needs
            # USB serial (or AP captive portal once we build it).
            ssid = request.get("ssid", "")
            pwd = request.get("pass", "")
            if not ssid:
                return {"status": "error", "msg": "ssid required"}
            if not self._nus.is_connected:
                return {"status": "no_device", "msg": "Echo not connected over BLE"}
            await self._nus.write_line(
                {"cmd": "wifi_add", "ssid": ssid, "pass": pwd}
            )
            log.info("WiFi add forwarded: ssid=%s", ssid)
            return {"status": "ok", "ssid": ssid}
        return {"status": "unknown_action"}

    async def _handle_permission(self, request: dict) -> dict:
        if not self._nus.is_connected:
            return {"status": "no_device"}

        pid = uuid.uuid4().hex[:8]
        tool = request.get("tool", "Unknown")[:20]
        detail = request.get("detail", "")[:40]

        fut: asyncio.Future = asyncio.get_event_loop().create_future()
        self._pending[pid] = fut

        # Buddy switches to "notification" while awaiting decision.
        self._char_state = "notification"

        # Send prompt to device.
        prompt = {"id": pid, "tool": tool, "hint": detail}
        await self._send_state(prompt=prompt)
        log.info("Permission sent to device: tool=%s id=%s", tool, pid)

        try:
            decision = await asyncio.wait_for(fut, timeout=PERMISSION_TIMEOUT)
        except asyncio.TimeoutError:
            log.warning("Permission timed out: id=%s", pid)
            decision = None
        finally:
            self._pending.pop(pid, None)
            # On allow → happy briefly; on deny/timeout → back to idle.
            if decision in ("once", "always"):
                self._char_state = "happy"
                import time
                self._last_tool_at = time.time()  # decay handles return
            else:
                self._char_state = "idle"
            # Clear the prompt from device display. Must pass prompt={}
            # explicitly — _send_state() with no arg now means "preserve",
            # which would leave the just-resolved prompt stuck on screen.
            await self._send_state(prompt={})

        if decision is None:
            return {"status": "timeout"}
        if decision == "__disconnect__":
            return {"status": "no_device"}
        if decision == "once":
            return {"status": "ok", "allowed": True, "always": False}
        if decision == "always":
            return {"status": "ok", "allowed": True, "always": True}
        # "deny" or anything else
        return {"status": "ok", "allowed": False, "always": False}


# ─────────────────────────────────────────────────────────────────────────────
# Entry point
# ─────────────────────────────────────────────────────────────────────────────

async def _main() -> None:
    level = logging.DEBUG if "--debug" in sys.argv else logging.INFO
    logging.basicConfig(
        level=level,
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
        datefmt="%H:%M:%S",
    )

    bridge = Bridge()

    loop = asyncio.get_event_loop()
    for sig in (signal.SIGINT, signal.SIGTERM):
        loop.add_signal_handler(sig, lambda: asyncio.ensure_future(bridge.stop()))

    try:
        await bridge.run()
    finally:
        await bridge.stop()


if __name__ == "__main__":
    asyncio.run(_main())
