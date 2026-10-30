#!/bin/bash
# Doomscroll Breaker - ESP32-S2 one-time setup (macOS)
# Run with:  bash ~/Desktop/Y2S1/CG2271/Project/tools/setup_esp32.sh
set -u
LOG=~/Desktop/Y2S1/CG2271/Project/logs/setup_esp32.log
exec > >(tee "$LOG") 2>&1
echo "=== Doomscroll Breaker ESP32 setup: $(date) ==="

SKETCH=~/Desktop/Y2S1/CG2271/Project/doomscroll_esp32
FQBN="esp32:esp32:esp32s2:CDCOnBoot=default,UploadMode=default,PartitionScheme=huge_app"

# 1. arduino-cli (installed into ~/bin, no admin password needed)
mkdir -p ~/bin
if ! command -v ~/bin/arduino-cli >/dev/null; then
  echo "--- Installing arduino-cli"
  curl -fsSL https://raw.githubusercontent.com/arduino/arduino-cli/master/install.sh | BINDIR=~/bin sh
fi
CLI=~/bin/arduino-cli
$CLI version || { echo "arduino-cli install failed"; exit 1; }

# 2. ESP32 boards package (same folder Arduino IDE uses, so the IDE sees it too)
echo "--- Installing ESP32 boards package (large download, several minutes)"
$CLI config init --overwrite >/dev/null
$CLI config set board_manager.additional_urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
$CLI core update-index
$CLI core install esp32:esp32 || { echo "ESP32 core install failed"; exit 1; }

# 3. Libraries
echo "--- Installing libraries"
$CLI lib update-index
$CLI lib install "ArduinoJson@6.21.5"
$CLI lib install "UniversalTelegramBot"

# 4. Compile the sketch
echo "--- Compiling $SKETCH (first build takes a few minutes)"
$CLI compile --fqbn "$FQBN" "$SKETCH" && echo "=== BUILD OK ===" || { echo "=== BUILD FAILED ==="; exit 1; }

echo
echo "Done. To upload: hold BOOT, tap RST, release BOOT, then run:"
echo "  bash ~/Desktop/Y2S1/CG2271/Project/tools/upload_esp32.sh"
