#!/bin/bash
# One-time setup (macOS): installs arduino-cli into ~/bin and the ESP32 core, then compiles the sketch.
# Run with:  bash tools/setup_esp32.sh
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$ROOT/logs"
exec > >(tee "$ROOT/logs/setup_esp32.log") 2>&1
echo "=== Doomscroll Breaker ESP32 setup: $(date) ==="

FQBN="esp32:esp32:esp32s2:CDCOnBoot=default,UploadMode=default,PartitionScheme=huge_app"
CLI=~/bin/arduino-cli

mkdir -p ~/bin
if ! command -v "$CLI" >/dev/null; then
  echo "--- Installing arduino-cli"
  curl -fsSL https://raw.githubusercontent.com/arduino/arduino-cli/master/install.sh | BINDIR=~/bin sh
fi
"$CLI" version || { echo "arduino-cli install failed"; exit 1; }

echo "--- Installing ESP32 boards package (large download, several minutes)"
"$CLI" config init --overwrite >/dev/null
"$CLI" config set board_manager.additional_urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
"$CLI" core update-index
"$CLI" core install esp32:esp32 || { echo "ESP32 core install failed"; exit 1; }

echo "--- Compiling (first build takes a few minutes)"
"$CLI" compile --fqbn "$FQBN" "$ROOT/doomscroll_esp32" && echo "=== BUILD OK ===" || { echo "=== BUILD FAILED ==="; exit 1; }
echo
echo "Done. To upload:  bash tools/ESP32_upload.command"
