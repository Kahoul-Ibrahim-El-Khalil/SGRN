#!/bin/bash

# Get absolute path to the arduino directory
ARDUINO_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"

# Configuration
SKETCH_NAME="data"
SKETCH_DIR="$ARDUINO_DIR/$SKETCH_NAME"
FQBN="esp32:esp32:esp32" 
PORT="/dev/ttyUSB0"
DO_MONITOR=false

# Simple argument parsing
for arg in "$@"; do
  case $arg in
    --monitor) DO_MONITOR=true ;;
    /dev/*) PORT="$arg" ;;
  esac
done

# Temporary build directory
BUILD_TMP="/tmp/arduino_build_$(date +%s)"
mkdir -p "$BUILD_TMP"

echo "--- Preparing patched library ---"
cp -r "$ARDUINO_DIR/s7client" "$BUILD_TMP/"
sed -i 's/^#define M5STACK_LAN/\/\/#define M5STACK_LAN/' "$BUILD_TMP/s7client/Platform.h"
sed -i 's/^\/\/#define ESP32_WIFI/#define ESP32_WIFI/' "$BUILD_TMP/s7client/Platform.h"

echo "--- Compiling $SKETCH_NAME ---"
arduino-cli compile --fqbn "$FQBN" --libraries "$BUILD_TMP" "$SKETCH_DIR"
COMPILE_RES=$?

if [ $COMPILE_RES -eq 0 ]; then
    echo "--- Flashing to $PORT ---"
    arduino-cli upload -p "$PORT" --fqbn "$FQBN" "$SKETCH_DIR"
    
    if [ "$DO_MONITOR" = true ]; then
        echo "--- Entering Monitor Mode (Ctrl+C to exit) ---"
        arduino-cli monitor -p "$PORT" --config baudrate=115200
    fi
else
    echo "Compilation failed."
fi

rm -rf "$BUILD_TMP"
exit $COMPILE_RES
