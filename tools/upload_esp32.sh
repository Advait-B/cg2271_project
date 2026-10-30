#!/bin/bash
# Uploads the Doomscroll Breaker sketch to the ESP32-S2.
# Before running: hold BOOT, tap RST, release BOOT (download mode).
SKETCH=~/Desktop/Y2S1/CG2271/Project/doomscroll_esp32
FQBN="esp32:esp32:esp32s2:CDCOnBoot=default,UploadMode=default,PartitionScheme=huge_app"
CLI=~/bin/arduino-cli
echo "Ports now:"; ls /dev/cu.* 2>/dev/null
PORT=$(ls /dev/cu.SLAB_USBtoUART /dev/cu.usbserial-* /dev/cu.wchusbserial* 2>/dev/null | head -1)
if [ -z "$PORT" ]; then
  echo "No ESP32 serial port found. Check the USB cable."
  echo "Ports seen:"; ls /dev/cu.* ; exit 1
fi
echo "Uploading to $PORT"
$CLI compile --fqbn "$FQBN" "$SKETCH" && $CLI upload --fqbn "$FQBN" -p "$PORT" "$SKETCH" \
  && echo "=== UPLOAD OK: now press RST on the board ===" \
  || echo "=== UPLOAD FAILED (see above) ==="
echo
echo "Serial monitor (Ctrl+C to quit): press RST first, wait 3 s, then run:"
echo "  ~/bin/arduino-cli monitor -p \$(ls /dev/cu.usbmodem* | head -1) -c baudrate=115200"
