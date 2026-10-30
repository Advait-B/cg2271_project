# Doomscroll Breaker

## Wiring

Power every kit module from **3.3 V**, not 5 V.

### MCXC444 (FRDM-MCXC444)

| Part | Module pin | Board pin | Header |
|---|---|---|---|
| Joystick | VRy | PTB0 | A0 |
| Big sound sensor | DO | PTA12 | D3 |
| Button | S | PTA13 | D8 |
| RGB LED (kit) | R | PTA5 | D5 |
| RGB LED (kit) | G | PTE30 | D7 |
| RGB LED (kit) | B | PTB1 | A1 |
| Passive buzzer | S | PTB2 | A2 |
| ESP32 link | TX → ESP32 RX | PTE22 | - |
| ESP32 link | RX ← ESP32 TX | PTE23 | - |
| Segment LCD | onboard | - | - |

Connect GND between the two boards.

### ESP32-S2 Mini

| Part | Pin |
|---|---|
| MCXC444 link RX | GPIO 2 |
| MCXC444 link TX | GPIO 1 |
| EC11 encoder A (CLK) | GPIO 33 |
| EC11 encoder B (DT) | GPIO 35 |
| EC11 push switch (SW) | GPIO 34 |
| EC11 common pin(s) | GND |
| Laptop | USB-C cable (acts as the keyboard) |

## MCXC444 setup

- Replace the `main` file in a new MCUXpresso FreeRTOS project with `doomscroll_mcxc444.c`.
- In `FreeRTOSConfig.h`:
  - `configUSE_PREEMPTION` = 1
  - `configUSE_TIME_SLICING` = 1
  - `configTOTAL_HEAP_SIZE` of at least 8 KB
- Leave jumper **JP7** shorted (default), so the LCD is powered from 3.3 V.

## ESP32 setup

- Board: **Tools → Board → esp32 → LOLIN S2 Mini**. Leave **USB CDC On Boot** enabled so the Serial Monitor still works.
- Install **UniversalTelegramBot** (also installs ArduinoJson). The USB keyboard library is built into the ESP32 board package.
- The ESP32-S2 has no Bluetooth, so the remote is a **USB keyboard**: plug the S2 Mini into the laptop running TikTok.
- If an upload fails, hold **0**, tap **RST**, release **0**, then upload again. Press **RST** after uploading.
- Copy `doomscroll_esp32/secrets.example.h` to `doomscroll_esp32/secrets.h` and fill in Wi-Fi, `BOT_TOKEN`, `FRIEND_CHAT_ID`, `USER_NAME`, `FIREBASE_URL` and `FIREBASE_SECRET`. `secrets.h` is git-ignored; the laptop helper reads the same file.
- Telegram: create a bot with @BotFather. Your friend must send the bot a message first. Their chat ID comes from `https://api.telegram.org/bot<TOKEN>/getUpdates`.
- `DAILY_BUDGET` is 10 videos (also set in the FRDM code; the ESP32 value wins at SYNC).
- EC11 volume knob: if one click changes the volume twice, set `ENC_STEPS_PER_DETENT` to `2`; if it turns the wrong way, set `ENC_REVERSED` to `true`. Pressing the knob mutes.
- Plug the S2 Mini into the laptop, open TikTok in a browser and click the video once so the page has focus.

## UART packets

Every packet is 5 bytes: `0xAA, type, a, b, type ^ a ^ b`.

| Direction | Type | Meaning |
|---|---|---|
| MCXC444 → ESP32 | `0x01` HELLO | Board started; ESP32 replies with SYNC |
| | `0x02` OPEN / `0x03` CLOSE | App opened or closed |
| | `0x04` NEXT | `a` = new video, `b` = videos left |
| | `0x05` PREV / `0x06` LIKE | Scroll back / like |
| | `0x07` STATE | `a` = state, `b` = videos left |
| | `0x08` ASK | Ask the friend for more videos |
| ESP32 → MCXC444 | `0x10` SYNC | `a:b` = videos left today |
| | `0x11` APPROVED / `0x12` DENIED | Friend's reply (no reply = denied) |
| | `0x13`–`0x16` BREAK_* | Unused since the MPU-6050 was removed (FRDM still accepts them) |

## Laptop helper (network)

The ESP32's Wi-Fi is off (`USE_WIFI = false`). The helper (`tools/Doomscroll_helper.command`) does all networking over the USB cable: Telegram Approve/Deny for extra videos, Firebase session/budget stats, and the date for the midnight budget reset. Keep its Terminal window open.

## Browser lock (Chrome extension)

TikTok and Instagram stay covered and paused unless the device is **plugged in, open, and has videos left**.

How it works: the ESP32 prints `$STATE,<state>,<left>` on its USB serial cable once a second (and straight away on every change). `tools/doomscroll_keys.py` (the laptop helper) reads it and serves it on `http://127.0.0.1:47271/state`. The extension in `extension/` checks that every 1.5 s and unlocks only if `state == "open"`, `left > 0` and the last line is under 4 s old. Unplugging the device, quitting the helper, or closing the app locks the sites again (fail closed). This needs no Wi-Fi: an earlier version went through Firebase, but each HTTPS request needs ~50 KB of RAM and the ESP32-S2 kept running out.

Setup:
1. Double-click `tools/Doomscroll_helper.command` and leave its Terminal window open. It also forwards keys and the volume knob; set `PRESS_KEYS = False` in `doomscroll_keys.py` if the ESP32's own USB keyboard cable (GPIO19/20) is plugged in, or every scroll happens twice.
2. Chrome → `chrome://extensions` → **Developer mode** → **Load unpacked** → pick `Project/extension` (after changing its files, press the reload icon on its card).

The toolbar badge shows videos left (green = unlocked, red = locked, `OFF` = device or helper not running).

Known gaps (say these in the report): the helper can be quit (that locks the sites, so it fails safe), the extension only covers Chrome (block other browsers with Screen Time, passcode held by the friend), it can be removed from `chrome://extensions`, and the phone app is out of scope.
