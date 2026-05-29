#!/usr/bin/env python3
"""
wifi_add — push a WiFi credential pair to Echo over BLE NUS.

Requires bridge.py to be running and BLE to be linked to the Echo device.
For first-time provisioning (or when BLE isn't available), use USB serial:
    pio device monitor → wifi add <ssid> <password>

Usage:
    wifi_add <ssid> <password>
    wifi_add "Network With Spaces" "password with spaces"

Open networks: pass empty string as password.
"""
import json
import os
import socket
import sys


SOCKET_PATH = os.environ.get(
    "STICKS3_SOCKET",
    os.path.expanduser("~/.config/sticks3-buddy/bridge.sock"),
)


def main() -> int:
    if len(sys.argv) < 2 or len(sys.argv) > 3:
        print(__doc__.strip(), file=sys.stderr)
        return 1

    ssid = sys.argv[1]
    password = sys.argv[2] if len(sys.argv) == 3 else ""

    if not os.path.exists(SOCKET_PATH):
        print(f"bridge socket not found: {SOCKET_PATH}\n"
              "is bridge.py running?  cd <repo> && ./start-bridge.sh", file=sys.stderr)
        return 2

    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(5)
        s.connect(SOCKET_PATH)
        s.sendall(json.dumps({
            "action": "wifi_add",
            "ssid": ssid,
            "pass": password,
        }).encode())
        s.shutdown(socket.SHUT_WR)
        resp = s.recv(2048).decode()
        s.close()
    except Exception as e:
        print(f"bridge IPC failed: {e}", file=sys.stderr)
        return 3

    try:
        data = json.loads(resp)
    except Exception:
        print(resp)
        return 4

    if data.get("status") == "ok":
        print(f"✓ sent: ssid={data.get('ssid', ssid)}")
        print("  Echo should reconnect to this network within ~5s")
        return 0
    print(f"✗ {data.get('status')}: {data.get('msg', '(no detail)')}")
    return 5


if __name__ == "__main__":
    sys.exit(main())
