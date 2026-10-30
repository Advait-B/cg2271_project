#!/bin/bash
# Compiles and uploads the ESP32 sketch over the CP2102 cable (no BOOT/RST needed). Log: logs/esp32_compile.log
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$ROOT/logs"
FQBN="esp32:esp32:esp32s2:CDCOnBoot=default,UploadMode=default,PartitionScheme=huge_app"
PORT=$(ls /dev/cu.SLAB_USBtoUART /dev/cu.usbserial-* /dev/cu.wchusbserial* 2>/dev/null | head -1)
if [ -z "$PORT" ]; then
  echo "No ESP32 serial port found (is the micro-USB plugged in?). Ports seen:"; ls /dev/cu.*; exit 1
fi
pkill -f doomscroll_keys.py 2>/dev/null   # frees the serial port
{
  echo "=== $(date) port=$PORT ==="
  ~/bin/arduino-cli compile --upload --fqbn "$FQBN" -p "$PORT" "$ROOT/doomscroll_esp32" 2>&1 | grep -v "Writing at"
  echo "EXIT ${PIPESTATUS[0]}"
} 2>&1 | tee "$ROOT/logs/esp32_compile.log"
echo
echo "Watch it boot:  bash tools/ESP32_monitor.command 30"
