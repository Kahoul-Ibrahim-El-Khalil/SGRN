#!/bin/bash

# Configuration
PORT="${1:-/dev/ttyUSB0}"
BAUD=115200

echo "--- Opening Console on $PORT ($BAUD baud) ---"
echo "--- Press Ctrl+C to exit ---"
echo ""

# Use arduino-cli to open the monitor
arduino-cli monitor -p "$PORT" --config baudrate=$BAUD
