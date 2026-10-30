#!/bin/bash
# Compile + upload the Doomscroll ESP32 sketch. Output also saved to ~/Desktop/Y2S1/CG2271/Project/logs/esp32_compile.log
LOG=~/Desktop/Y2S1/CG2271/Project/logs/esp32_compile.log
FQBN=esp32:esp32:esp32s2:CDCOnBoot=default,UploadMode=default,PartitionScheme=huge_app
pkill -f doomscroll_keys.py 2>/dev/null   # frees the serial port
{
  echo "=== $(date) ==="
  ~/bin/arduino-cli compile --upload --fqbn $FQBN -p /dev/cu.SLAB_USBtoUART ~/Desktop/Y2S1/CG2271/Project/doomscroll_esp32 2>&1 | grep -v "Writing at"
  echo "EXIT ${PIPESTATUS[0]}"
} 2>&1 | tee "$LOG"
