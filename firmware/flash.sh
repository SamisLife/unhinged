#!/bin/sh
# Compile and upload a sketch to the rig board (Raspberry Pi Pico, Pico 2 or XIAO ESP32-S3).
#   firmware/flash.sh [sketch-dir]   (default: firmware/rig)
# The board is detected automatically; force it with BOARD=pico, BOARD=pico2 or BOARD=xiao.
set -e
cd "$(dirname "$0")/.."
SKETCH=${1:-firmware/rig}
CTAGS="--build-property tools.ctags.path=$PWD/tools/ctags-stub"
XIAO_FQBN=esp32:esp32:XIAO_ESP32S3
USB=$(ioreg -p IOUSB -l -w0)

if [ -z "$BOARD" ]; then
  if [ -d /Volumes/RP2350 ] || echo "$USB" | grep -qE '"USB Product Name" = "(RP2350|Pico 2)'; then
    BOARD=pico2
  elif [ -d /Volumes/RPI-RP2 ] || echo "$USB" | grep -q '"idVendor" = 11914'; then
    BOARD=pico
  else
    BOARD=xiao
  fi
fi

case "$BOARD" in
  pico2) PICO_FQBN=rp2040:rp2040:rpipico2; PICO_DRIVE=/Volumes/RP2350 ;;
  pico)  PICO_FQBN=rp2040:rp2040:rpipico;  PICO_DRIVE=/Volumes/RPI-RP2 ;;
esac

PORT=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)

if [ -n "$PICO_FQBN" ]; then
  if [ -d "$PICO_DRIVE" ]; then
    # BOOTSEL mode: the Pico is a USB drive, copy the firmware onto it.
    OUT=$(mktemp -d)
    arduino-cli compile --fqbn $PICO_FQBN $CTAGS --output-dir "$OUT" "$SKETCH"
    cp "$OUT"/*.uf2 "$PICO_DRIVE"/
    echo "Copied to $PICO_DRIVE. The Pico reboots into the new firmware."
  else
    [ -n "$PORT" ] || { echo "No Pico found. Hold BOOTSEL while plugging it in, then rerun."; exit 1; }
    arduino-cli compile --upload -p "$PORT" --fqbn $PICO_FQBN $CTAGS "$SKETCH"
  fi
else
  [ -n "$PORT" ] || { echo "No board found. Plug it in (data cable, not charge-only)."; exit 1; }
  arduino-cli compile --upload -p "$PORT" --fqbn $XIAO_FQBN $CTAGS "$SKETCH"
fi
