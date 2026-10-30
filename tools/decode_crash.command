#!/bin/bash
# Decodes the last ESP32 crash backtrace in logs/esp32_serial.log -> logs/crash_decoded.txt
P="$(cd "$(dirname "$0")/.." && pwd)/logs"
OUT=$P/crash_decoded.txt
ELF=$(ls -t ~/Library/Caches/arduino/sketches/*/doomscroll_esp32.ino.elf 2>/dev/null | head -1)
A2L=$(find ~/Library/Arduino15/packages/esp32/tools -name 'xtensa*addr2line' 2>/dev/null | grep -i s2 | head -1)
[ -z "$A2L" ] && A2L=$(find ~/Library/Arduino15/packages/esp32/tools -name 'xtensa*addr2line' 2>/dev/null | head -1)
ADDRS=$(grep -m1 '^Backtrace:' $P/esp32_serial.log | sed 's/^Backtrace://' | tr ' ' '\n' | cut -d: -f1 | grep 0x)
{
  echo "ELF: $ELF"; echo "A2L: $A2L"; echo "ADDRS: $ADDRS"; echo
  "$A2L" -pfiaC -e "$ELF" $ADDRS
} > "$OUT" 2>&1
echo "Saved to $OUT"
