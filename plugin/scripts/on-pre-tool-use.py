#!/usr/bin/env python3
"""
PreToolUse hook: updates StickS3 display with current session activity.
Non-blocking — if bridge is unavailable, silently exits.
"""
import json
import os
import socket
import sys

SOCKET_PATH = os.environ.get(
    "STICKS3_SOCKET",
    os.path.expanduser("~/.config/sticks3-buddy/bridge.sock"),
)


def main():
    if not os.path.exists(SOCKET_PATH):
        sys.exit(0)

    try:
        hook_input = json.loads(sys.stdin.read())
    except Exception:
        sys.exit(0)

    # Count running tools from session info if available.
    session = hook_input.get("session", {})
    running = len(session.get("active_tasks", [])) + 1  # +1 for this tool

    # Tool name → buddy character state. Bridge does the actual name→state
    # translation; we just forward the raw tool name.
    tool_name = hook_input.get("tool_name", "")

    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(1.0)
        s.connect(SOCKET_PATH)
        s.sendall(json.dumps({
            "action": "state_update",
            "total": session.get("total_tasks", 1),
            "running": running,
            "waiting": 0,
            "completed": False,
            "tokens": session.get("output_tokens", 0),
            "tool_name": tool_name,
            "cwd": hook_input.get("cwd", ""),
        }).encode())
        s.shutdown(socket.SHUT_WR)
        s.recv(256)
        s.close()
    except Exception:
        pass

    sys.exit(0)


if __name__ == "__main__":
    main()
