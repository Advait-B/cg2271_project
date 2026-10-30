#!/bin/bash
# Double-click: stops the helper, resets the ESP32 and logs 60 s of boot output to logs/esp32_serial.log,
# then starts the helper again in this window (keep it open).
pkill -f doomscroll_keys.py 2>/dev/null
sleep 1
bash ~/Desktop/Y2S1/CG2271/Project/tools/ESP32_monitor.command 60
exec python3 ~/Desktop/Y2S1/CG2271/Project/tools/doomscroll_keys.py
