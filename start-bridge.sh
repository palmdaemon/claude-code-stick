#!/bin/bash
# Start the StickS3 bridge daemon.
# Keep this terminal open; Ctrl-C to stop.
#
# LAN ASR port pinned to 8766 to avoid colliding with other local ESP
# buddy projects that default to 8765. Override via STICKS3_LAN_PORT env var.
cd "$(dirname "$0")"
export STICKS3_LAN_PORT="${STICKS3_LAN_PORT:-8766}"
exec bridge-venv/bin/python3 bridge/bridge.py "$@"
