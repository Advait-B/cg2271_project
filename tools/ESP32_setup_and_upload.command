#!/bin/bash
# Double-click me. Installs tools (first time only), compiles, uploads to the ESP32-S2, then shows 25 s of serial output.
cd ~/Desktop/Y2S1/CG2271/Project
LOG=~/Desktop/Y2S1/CG2271/Project/logs/esp32_run.log
{
echo "=== $(date) ==="
bash ~/Desktop/Y2S1/CG2271/Project/tools/setup_esp32.sh || { echo "SETUP FAILED"; exit 1; }
bash ~/Desktop/Y2S1/CG2271/Project/tools/upload_esp32.sh
echo "--- waiting for board to restart"
sleep 4
PORT=$(ls /dev/cu.SLAB_USBtoUART /dev/cu.usbserial-* /dev/cu.wchusbserial* 2>/dev/null | head -1)
echo "--- serial output from $PORT (25 s)"
if [ -n "$PORT" ]; then
  ~/bin/arduino-cli monitor -p "$PORT" -c baudrate=115200 & MPID=$!
  sleep 25; kill $MPID 2>/dev/null
fi
echo "=== finished $(date) ==="
} 2>&1 | tee "$LOG"
echo; echo "Log saved to $LOG. You can close this window."
