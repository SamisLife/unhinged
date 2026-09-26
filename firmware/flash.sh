#!/bin/sh
# Compile and upload a sketch to the XIAO ESP32-S3.
#   firmware/flash.sh [sketch-dir]   (default: firmware/servo_bringup)
set -e
cd "$(dirname "$0")/.."
SKETCH=${1:-firmware/servo_bringup}
PORT=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)
[ -n "$PORT" ] || { echo "No XIAO found. Plug it in (data cable, not charge-only)."; exit 1; }
arduino-cli compile --upload -p "$PORT" \
  --fqbn esp32:esp32:XIAO_ESP32S3 \
  --build-property tools.ctags.path="$PWD/tools/ctags-stub" \
  "$SKETCH"
