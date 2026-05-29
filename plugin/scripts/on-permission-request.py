#!/usr/bin/env python3
"""
PermissionRequest hook: routes permission prompts to StickS3 via bridge daemon.
Returns allow/deny decision from physical button press.
Falls back to normal Claude Code dialog if bridge is not running.
"""
import json
import os
import socket
import sys

SOCKET_PATH = os.environ.get(
    "STICKS3_SOCKET",
    os.path.expanduser("~/.config/sticks3-buddy/bridge.sock"),
)
TIMEOUT = 95  # slightly longer than bridge's PERMISSION_TIMEOUT


def send_to_bridge(tool: str, detail: str) -> dict:
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(TIMEOUT)
    s.connect(SOCKET_PATH)
    s.sendall(json.dumps({"action": "permission_request", "tool": tool, "detail": detail}).encode())
    s.shutdown(socket.SHUT_WR)
    resp = b""
    while True:
        chunk = s.recv(4096)
        if not chunk:
            break
        resp += chunk
    s.close()
    return json.loads(resp.decode())


def extract_detail(tool_name: str, tool_input: dict) -> str:
    if tool_name == "Bash":
        desc = tool_input.get("description", "")
        return desc[:40] if desc else tool_input.get("command", "")[:40]
    if tool_name in ("Edit", "Write", "Read"):
        path = tool_input.get("file_path", "")
        return os.path.basename(path)[:40] if path else ""
    if tool_name in ("WebFetch", "WebSearch"):
        val = tool_input.get("url") or tool_input.get("query", "")
        for p in ("https://", "http://"):
            if val.startswith(p):
                val = val[len(p):]
        return val[:40]
    if tool_name == "Agent":
        return tool_input.get("description", "")[:40]
    if "__" in tool_name:
        return tool_name.split("__")[-1][:40]
    return ""


def main():
    if not os.path.exists(SOCKET_PATH):
        sys.exit(1)  # bridge not running → normal dialog

    try:
        hook_input = json.loads(sys.stdin.read())
    except Exception:
        sys.exit(1)

    tool_name = hook_input.get("tool_name", "Unknown")
    tool_input = hook_input.get("tool_input", {})

    # Shorten MCP tool names for display: mcp__server__action → server:action
    display_name = tool_name
    if "__" in tool_name:
        parts = tool_name.split("__")
        if len(parts) >= 3:
            display_name = f"{parts[1]}:{parts[2]}"[:20]

    detail = extract_detail(tool_name, tool_input)

    try:
        result = send_to_bridge(display_name, detail)
    except Exception:
        sys.exit(1)  # bridge error → normal dialog

    status = result.get("status")
    if status not in ("ok",):
        sys.exit(1)  # timeout/no_device → normal dialog

    allowed = result.get("allowed", False)
    always = result.get("always", False)

    if allowed:
        decision = {"behavior": "allow"}
        if always:
            suggestions = hook_input.get("permission_suggestions", [])
            if suggestions:
                decision["updatedPermissions"] = suggestions
    else:
        decision = {"behavior": "deny", "message": "Denied on StickS3"}

    print(json.dumps({
        "hookSpecificOutput": {
            "hookEventName": "PermissionRequest",
            "decision": decision,
        }
    }))
    sys.exit(0)


if __name__ == "__main__":
    main()
