# claude-code-stick

Hardware companion for [Claude Code](https://claude.ai/code) — an M5Stack StickS3 (ESP32-S3) that watches your sessions, approves permission prompts with a physical button, and lets you talk to your terminal.

> **Status:** v0.1 alpha. Single-developer project. Documentation in progress.

## Trademark notice

This is an **independent fan project**, not affiliated with or endorsed by Anthropic, Alibaba Cloud, or Amazon.com.

- **Claude**, **Claude Code**, **Clawd** are trademarks of Anthropic, PBC.
- **DashScope** is a trademark of Alibaba Cloud.
- The default device name **"Echo"** is unrelated to Amazon Echo. The name is a placeholder borrowed from Greek mythology and is **renameable** — see Configuration.

## What it does

- **Permission approval over physical buttons** — when Claude Code prompts for permission, an animated Anthropic-style Clawd buddy lights up on the device and you press A (approve) or B (deny). No alt-tab.
- **Voice input over Wi-Fi** — hold A on the device, speak, release. Your transcript is pasted into Claude Code's input box via macOS Cmd-V. Uses Alibaba Cloud DashScope's Qwen3-ASR-Flash (you bring your own key).
- **Buddy state mirrors tool activity** — Edit/Write → typing, Bash → building, WebSearch → wizard, etc. Idle clawd has random mood bursts so it doesn't look frozen.
- **Boot status panel** — Wi-Fi → BLE → Bridge handshake progress on screen at startup.
- **Remote Wi-Fi provisioning over BLE** — `echo-wifi "Network" "password"` from the Mac.

## Architecture

```
[StickS3]
   ├── BLE NUS ─────────────────► [bridge.py on Mac]
   │   (control: permission /            │
   │    char_state / asr text /          │── Unix socket: hooks (Claude Code)
   │    send_enter / wifi_add)           │── HTTPS: DashScope ASR
   │                                     └── osascript Cmd-V: paste
   │
   └── Wi-Fi HTTP POST ─────────► bridge.py:8765/audio  (raw int16 PCM)
```

**Device side (firmware/)**: ESP32-S3 doesn't talk to the cloud. It records audio and ships raw PCM over Wi-Fi to the Mac, and exchanges JSON control messages over BLE.

**Mac side (bridge/bridge.py)**: A single Python daemon. Handles BLE, runs an HTTP server for audio, calls DashScope, pastes into the focused window, and exposes a Unix socket for Claude Code's PreToolUse / PermissionRequest hooks.

## Hardware required

- M5Stack StickS3 (ESP32-S3-PICO, ES8311 mic, 135×240 ST7789P3)
- macOS with Bluetooth Low Energy (any modern Mac)
- A 2.4 GHz Wi-Fi network the StickS3 can join
- An Alibaba Cloud DashScope account for ASR (~free tier for casual use)

## I'm not a developer — can I still use this?

**Yes.** Open Claude Code (or another coding-agent CLI) and paste this prompt:

> Set up claude-code-stick from https://github.com/s34g3r/claude-code-stick
> following its README. I'm on macOS with a M5Stack StickS3 plugged in via
> USB and I have an Alibaba Cloud DashScope account. Walk me through every
> step including installing Homebrew, PlatformIO, the bridge Python venv,
> flashing firmware, configuring Wi-Fi, registering Claude Code hooks, and
> the first round-trip test. Stop and ask me whenever you need a value
> from me (Wi-Fi name and password, DashScope key, etc.).

Your agent will handle the rest. Plan for ~30 minutes including the slow
PlatformIO download on first run.

If you don't have a DashScope account, you can skip the voice-input
feature — the physical-button permission approval still works without it.
The agent should adapt if you tell it "skip voice setup, button only".

## Quickstart

> Detailed setup is still being written. For now this is a sketch.

1. **Get a DashScope API key**: https://help.aliyun.com/zh/model-studio/qwen-asr-api-reference
2. Save it: `mkdir -p ~/.config/claude-code-stick && echo 'sk-...' > ~/.config/claude-code-stick/dashscope_key`
3. **Flash firmware**: `cd firmware && pio run --target upload`
4. **Configure Wi-Fi** (first time, via USB serial): `pio device monitor`, then `wifi add YourSSID YourPassword`
5. **Install bridge dependencies**: `cd bridge && python3 -m venv venv && venv/bin/pip install bleak`
6. **Configure Claude Code hooks**: add the entries in `plugin/settings.example.json` to your `~/.claude/settings.json`
7. **Start the bridge**: `./start-bridge.sh`
8. Press PWR on the StickS3 to power on. Watch the boot panel. When it says READY, you're set.

## Configuration

### Device name

The default BLE device name is `Echo-XXXX`. To rename:
- Edit `firmware/src/main.cpp:27` (`static char btName[16] = "Echo"`)
- Update `DEVICE_NAME_PREFIX` in `bridge/bridge.py`
- Reflash and restart bridge

### Wi-Fi networks

Once BLE is paired, you can add networks remotely:

```bash
./bridge/wifi_add.py "Network Name" "password"
```

Open networks: pass an empty string as password.

## Known limitations

- **No first-time Wi-Fi captive portal.** First network has to be entered via USB serial (or use a saved network).
- **BLE link can go stale** occasionally on macOS. Symptom: bridge thinks it's connected but notifies don't arrive. Workaround: physical PWR cycle (off / on) on the device.
- **HUD shows fixed "Echo" instead of project name.** A "follow current Claude Code session" feature was tried but raced under heavy hook traffic. Reserved for later.
- **No automatic Wi-Fi configuration over AP.** Coming.
- **macOS only** for the bridge. Linux / Windows would need a substitute for `osascript` paste injection.

## Security notes

- `bridge.py:8765/audio` is an **unauthenticated LAN endpoint**. Anyone on your local network can POST raw PCM and consume your DashScope quota. If you're on untrusted Wi-Fi, firewall it.
- DashScope API key lives in `~/.config/claude-code-stick/dashscope_key` (mode 600 recommended) or `DASHSCOPE_API_KEY` env var.
- BLE Nordic UART Service uses **encrypted, MITM-protected pairing** (you'll see a passkey on the device screen at first pair). Bonds are persisted in NVS.

## Acknowledgments

This project is a derivative of [anthropics/claude-desktop-buddy](https://github.com/anthropics/claude-desktop-buddy) — Anthropic's reference firmware for ESP32-based companion devices, whose `CONTRIBUTING.md` opens with *"The best contribution is a fork"*. We took them up on that.

- Clawd animations from [marciogranzotto/clawd-tank](https://github.com/marciogranzotto/clawd-tank) (MIT) — converted to GIF and tuned for ST7789P3.
- Architecture inspiration from [steveruizok/chat-stick](https://github.com/steveruizok/chat-stick) and [78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32), particularly the "device is a microphone, server does the talking" pattern.
- Hardware platform: M5Stack StickS3.

See `NOTICE` for full third-party license attribution.

## License

MIT — see `LICENSE`.
