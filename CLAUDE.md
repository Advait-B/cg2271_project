# Doomscroll Breaker - notes for Claude

The repo is github.com/Advait-B/cg2271_project. README.md has wiring, protocols and build steps; keep it current.
Mac user: advait; local copy at `~/Desktop/Y2S1/CG2271/Project/`. Both boards plug into that MacBook.

## Hardware facts
- FRDM-MCXC444 on the MCU-Link port = `/dev/cu.usbmodem31DMQZ5XVSOFS3` (NOT the ESP32). PRINTF is on it at 115200.
- ESP32-S2-DevKitM-1U (ESP32-S2FH4, 4 MB flash, no PSRAM, external U.FL antenna). Its micro-USB is a CP2102 =
  `/dev/cu.SLAB_USBtoUART` (Serial, used by the helper). Native USB (HID keyboard) is only on GPIO19/20 via a second cable.
- Wi-Fi on the ESP32 was dropped: RSSI -85..-96 dBm, mbedTLS ran out of heap (~50 KB free), lwIP asserts. All
  networking is in `tools/doomscroll_keys.py` over the CP2102. Don't bring the on-board Wi-Fi path back.

## Build
- FRDM: MCUXpresso project in `mcxc444/MCXC444_Project` (import without copying). SDK_2.x_FRDM-MCXC444 26.06.
  Real FreeRTOS settings live in `FreeRTOSConfig_Gen.h`; the template `FreeRTOSConfig.h` overrides them (#undef/#define).
  Cortex-M0+: NVIC priorities are levels 0-3, not shifted values.
- ESP32: `~/bin/arduino-cli`, esp32 core 3.3.12, FQBN `esp32:esp32:esp32s2:CDCOnBoot=default,UploadMode=default,PartitionScheme=huge_app`.
  The sketch folder name must equal the .ino name. No extra libraries needed.
- Scripts in `tools/` find the repo root from their own path. `.command` files lose +x through the mounted folder:
  tell the user to run them with `bash`.

## Open issues
1. Joystick: only the A0 axis is read; untested whether the wired axis is the scrolling one.
2. USB HID keyboard untested (needs the GPIO19/20 cable). Until then the helper forwards keys (`FORWARD_INPUT = True`).
3. Clap untested while open.
4. To reset the saved video count: erase the ESP32 flash with esptool (`--chip esp32s2 erase-flash`), then re-upload.
