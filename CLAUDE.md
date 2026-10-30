# Doomscroll Breaker - project status (21 Oct 2026)

Folder: `~/Desktop/Y2S1/CG2271/Project/` is the ONLY copy and the single source of truth (24 Oct: old `Doomscroll Breaker/` copy, `.bak*` sketches and `DoomscrollBreaker_Code.zip` deleted). Layout: `doomscroll_esp32/doomscroll_esp32.ino`, `mcxc444/doomscroll_mcxc444.c` (+ `MCXC444_Project.zip`, the MCUXpresso project export whose `source/MCXC444_Project.c` must stay identical to it), `tools/` (scripts), `logs/`, `schematics/`.
Mac user: advait. Boards both plugged into this MacBook.

## Hardware
- **FRDM-MCXC444** - USB on the MCU-Link port. Appears as `/dev/cu.usbmodem31DMQZ5XVSOFS3` (NOT the ESP32!).
- **ESP32-S2-DevKitM-1U** (module ESP32-S2-MINI-1U, chip ESP32-S2FH4, 4 MB flash, no PSRAM). ONE micro-USB, which
  goes to a CP2102 serial chip -> `/dev/cu.SLAB_USBtoUART`. The chip's own USB (needed for the USB-keyboard
  feature) is only on header pins GPIO19 (D-) / GPIO20 (D+). A second USB cable must be wired
  white->GPIO19, green->GPIO20, black->GND (leave red unconnected) and plugged into the laptop running TikTok.
  "1U" = external antenna: a U.FL antenna must be attached for Wi-Fi.
- Pins (ESP32): link RX GPIO2 <- FRDM PTE22 (TX); link TX GPIO1 -> FRDM PTE23 (RX); EC11 rotary encoder A 33, B 35, SW 34, common -> GND (volume knob, replaced the MPU-6050 on 30 Oct). Common GND.
- Pins (FRDM): button PTA13, sound sensor PTA12, joystick PTB0/A0 (ADC, ONE axis only), LEDs PTA5/PTE30/PTB1,
  buzzer PTB2, LCD on-board. UART2 9600 baud to the ESP32.

## Status
- FRDM: `mcxc444/MCXC444_Project.zip` imported into MCUXpresso IDE (workspace already has it). FreeRTOS fix is inside
  (`freertos/freertos-kernel/template/FreeRTOSConfig.h` has #undef/#define overrides for configUSE_TIME_SLICING=1 etc.
  because the real settings live in `FreeRTOSConfig_Gen.h`). Builds 0 errors. SDK_2.x_FRDM-MCXC444 26.06 is installed.
  Debug launch config was fixed (C/C++ Application path = `Debug/MCXC444_Project.axf`, the imported one had a `\`).
  Flashed and running. To flash again: select project, Run > Debug History > "MCXC444_Project LinkServer Debug",
  cancel the "Install LinkServer" update dialog if it appears, press Resume (F8) when it stops at main.
  Debug/semihost console prints go to MCUXpresso's Console; PRINTF goes to LPUART0 = the usbmodem31DMQ... port at 115200.
- ESP32: arduino-cli installed in `~/bin`, esp32 core 3.3.12, ArduinoJson 7.4.3, UniversalTelegramBot 1.3.0.
  Sketch `Project/doomscroll_esp32/doomscroll_esp32.ino` has Wi-Fi/bot/Firebase constants filled in,
  FRIEND_CHAT_ID = 6130634739 (advait's own Telegram id), plus two debug lines (`DBG packet ...`, `DBG key ...`).
  Build: FQBN `esp32:esp32:esp32s2:CDCOnBoot=default,UploadMode=default,PartitionScheme=huge_app`
  (CDC off so Serial debug comes out of the CP2102; huge_app because the sketch is 1.09 MB). Uploaded OK.
  Serial at boot shows: starting / USB keyboard + volume keys ready / Volume knob ready / Connected (hotspot "advait") / Date / Videos left 10 / Ready.
- Link FRDM->ESP32 confirmed: packets 0x03 CLOSE and 0x07 STATE arrive.

## Open issues
1. Joystick "does nothing": only the axis wired to A0 is read. Try both axes while the LCD shows a number (app open).
   If neither works: joystick VCC must be 3V3, GND shared, signal on A0. Thresholds in FRDM code: >3300 = next, <800 = prev (12-bit).
2. (30 Oct) MPU-6050 removed; EC11 volume knob + Chrome browser lock added, budget 10. Both boards built, flashed, booting. Sketch folder renamed esp32/ -> doomscroll_esp32/ (arduino-cli needs folder = .ino name).
   Wi-Fi now hotspot "dropout" (keys in doomscroll_esp32/secrets.h, git-ignored); RSSI is ~-95 dBm (check the U.FL antenna). HTTPS (Telegram/Firebase) fails with mbedTLS BIGNUM alloc errors at ~50 KB free heap; one boot also hit lwIP assert 'Required to lock TCPIP core' during getMe. Telegram unlock is therefore unreliable.
   FRDM: DAILY_BUDGET 10, AMBER_LEFT 3 changed in mcxc444/doomscroll_mcxc444.c AND the zip; the MCUXpresso workspace copy needs the same 2 lines (or re-import).
3. USB keyboard untested - needs the GPIO19/20 cable above. Then open tiktok.com in a browser, click a video for focus.
4. Clap (sound sensor) untested while open.
5. Video count persists across ESP32 reboots (NVS). To reset: add `prefs.clear();` after `prefs.begin(...)` once, or
   erase flash: `~/bin/arduino-cli upload ... ` after `~/Library/Arduino15/packages/esp32/tools/esptool_py/*/esptool --chip esp32s2 -p /dev/cu.SLAB_USBtoUART erase-flash`.

## Commands (run in Terminal on the Mac)
```
# install tools + compile (idempotent)
bash ~/Desktop/Y2S1/CG2271/Project/tools/setup_esp32.sh
# compile + upload over the CP2102 (no BOOT/RST needed)
bash ~/Desktop/Y2S1/CG2271/Project/tools/upload_esp32.sh
# both of the above, then 25 s of output (the output part is flaky - use the monitor instead)
bash ~/Desktop/Y2S1/CG2271/Project/tools/ESP32_setup_and_upload.command
# reset the ESP32 and record 40 s of serial output to ~/Desktop/Y2S1/CG2271/Project/logs/esp32_serial.log
bash ~/Desktop/Y2S1/CG2271/Project/tools/ESP32_monitor.command        # optional arg = seconds
# manual compile / upload
~/bin/arduino-cli compile --fqbn esp32:esp32:esp32s2:CDCOnBoot=default,UploadMode=default,PartitionScheme=huge_app ~/Desktop/Y2S1/CG2271/Project/doomscroll_esp32
~/bin/arduino-cli upload  --fqbn esp32:esp32:esp32s2:CDCOnBoot=default,UploadMode=default,PartitionScheme=huge_app -p /dev/cu.SLAB_USBtoUART ~/Desktop/Y2S1/CG2271/Project/doomscroll_esp32
# Option B (no 2nd USB cable): Mac reads 'DBG key 217/218' from the ESP32 serial and presses Down/Up itself
python3 ~/Desktop/Y2S1/CG2271/Project/tools/doomscroll_keys.py
# FRDM serial (PRINTF) monitor
~/bin/arduino-cli monitor -p /dev/cu.usbmodem31DMQZ5XVSOFS3 -c baudrate=115200
```
Logs: `setup_esp32.log`, `esp32_run.log`, `esp32_serial.log`, `esp32_compile.log` in `~/Desktop/Y2S1/CG2271/Project/logs/`.
Note for Claude: `.command` files lose their +x bit when rewritten through the mounted folder - tell the user to run them with `bash`.
Claude (cloud) can read/write `~/Desktop/Y2S1/CG2271` via the mount but cannot type into Terminal/IDEs (click-only); MCUXpresso and Finder are full-control.

## Demo flow
ESP32 first (wait for Ready), FRDM second. Short press = open (LCD shows count, green). Joystick = next/prev (count drops on new videos).
Clap = like. 0 left -> LOC red. Long press (1 s) while LOC -> Telegram Approve/Deny (SEnt). No break timer any more (it needed the MPU). EC11 knob = laptop volume, press = mute.
TikTok/Instagram in Chrome are covered by `extension/` unless the helper (tools/Doomscroll_helper.command -> doomscroll_keys.py, serves http://127.0.0.1:47271/state from the ESP32's `$STATE,<state>,<left>` serial lines) says open, left>0, fresh (<4 s). Firebase was dropped for this on 30 Oct: TLS ran out of heap (BIGNUM alloc fail, ~50 KB free).
Short press closes -> stats to Firebase. DAILY_BUDGET 10 (ESP32 value is authoritative).

## Telegram fix (21 Oct 2026, 16:40)
UniversalTelegramBot removed. The sketch now calls api.telegram.org directly with HTTPClient (new TLS client per call,
10 s timeouts, retries, logs HTTP code + Telegram's error text + heap/biggest block). WiFi.setSleep(false) after connect.
If the send fails the FRDM gets DENIED instead of waiting 2 min. (Old `.bak*` versions deleted 24 Oct.)
Compile+upload by running `bash ~/Desktop/Y2S1/CG2271/Project/tools/ESP32_compile_upload.command` (log: Project/logs/esp32_compile.log).

Update 17:15: Telegram now copies the 2028Assignment STM32 format (Core/Src/telegram.c): plain HTTPS
GET /bot<TOKEN>/<method>?<url-encoded params> HTTP/1.1, Host api.telegram.org, Connection: close, success = " 200".
Boot prints $TLS,CHECK_OK / $TLS,CHECK_FAILED (getMe). Sends print $TELEGRAM,SENT|FAILED,ASK and
$TELEGRAM,ERROR,<method>,<step>,<detail>. (Previous `.bak3` deleted 24 Oct.)

## 30 Oct 12:55 - networking moved to the laptop
ESP32 `USE_WIFI = false`: Wi-Fi off (antenna RSSI -85..-96, TLS out of heap, lwIP assert in WiFi.hostByName vs SNTP).
Serial protocol on the CP2102 cable (115200): ESP32 -> laptop `$STATE,<st>,<left>` (1 Hz), `$ASK,<watched>`, `$SESSION,<json>`,
`$BUDGET,<date>,<left>,<watched>`; laptop -> ESP32 `$DATE,YYYY-MM-DD` (every 30 s), `$REPLY,APPROVED|DENIED`.
tools/doomscroll_keys.py (start with tools/Doomscroll_helper.command) does Telegram (curl, reads token/chat id from the sketch),
Firebase writes, the browser-lock server, key/volume forwarding. tools/ESP32_check.command = reset + 60 s log + restart helper.
tools/decode_crash.command decodes the last Backtrace with the Arduino toolchain's addr2line.
