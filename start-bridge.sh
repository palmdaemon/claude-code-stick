#!/bin/bash
# Start the StickS3 bridge daemon.
# Keep this terminal open; Ctrl-C to stop.
cd "$(dirname "$0")"
exec bridge-venv/bin/python3 bridge/bridge.py "$@"
