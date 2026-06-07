#!/usr/bin/env python3
"""
Stop hook: fires a one-shot chime on Echo when Claude finishes a turn.
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
        sys.stdin.read()  # drain stdin so Claude Code doesn't see SIGPIPE
    except Exception:
        pass

    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(1.0)
        s.connect(SOCKET_PATH)
        s.sendall(json.dumps({"action": "chime", "agent": "claude"}).encode())
        s.shutdown(socket.SHUT_WR)
        s.recv(256)
        s.close()
    except Exception:
        pass

    sys.exit(0)


if __name__ == "__main__":
    main()
