# Doomscroll Breaker

A physical TikTok remote with a daily video budget. The FRDM-MCXC444 runs the controls and the
budget (FreeRTOS); an ESP32-S2 turns its packets into USB keyboard presses on the laptop; a laptop
helper does Telegram (ask a friend for more videos), Firebase stats and a Chrome lock for
TikTok/Instagram.

```
FRDM-MCXC444 ──UART 9600──▶ ESP32-S2 ──USB HID (GPIO19/20)──▶ laptop: arrow keys, volume
  button, joystick,           │  EC11 volume knob
  clap, LED, buzzer, LCD      └──USB serial (CP2102)──▶ tools/doomscroll_keys.py
                                                         ├─ Telegram, Firebase (curl)
                                                         └─ http://127.0.0.1:47271/state ─▶ extension/
```

| Folder | What |
|---|---|
| `mcxc444/MCXC444_Project/` | MCUXpresso project; the app is `source/MCXC444_Project.c` |
| `doomscroll_esp32/` | Arduino sketch + `secrets.h` (keys, read by the sketch and the helper) |
| `tools/` | laptop helper and build/monitor scripts |
| `extension/` | Chrome extension that locks TikTok/Instagram |
| `hardware/` | perfboard layout + case generator (`doomscroll_hw.py`), printable STLs in `stl_rev2/` |
| `schematics/` | wiring diagram |

## Demo

Start the ESP32 first, then the FRDM, then `tools/Doomscroll_helper.command`.

| Action | Result |
|---|---|
| Short press | Open (green LED, LCD shows videos left) / close (`OFF`) |
| Joystick | Next / previous video; only new videos use up the budget |
| Clap | Like |
| 3 or fewer left | Amber LED, warning beep |
| 0 left | `LOC`, red |
| Long press (1 s) while `LOC` | Asks the friend on Telegram (`SEnt`, red blinking) |
| Short press while `SEnt` | Gives up and returns to `LOC` |
| Friend approves / denies | +10 videos, tune / `dEny` for 3 s; no reply in 2 min = denied |
| EC11 knob | Laptop volume; press = mute |

## Wiring

Power every kit module from **3.3 V**. Join GND between the boards; keep the two 3V3 rails separate.

**FRDM-MCXC444**

| Part | Module pin | Board pin | Header |
|---|---|---|---|
| Joystick (one axis) | VRy | PTB0 | A0 |
| Sound sensor | DO | PTA12 | D3 |
| Button | S | PTA13 | D8 |
| RGB LED, each via 220 Ω (R1–R3) | R / G / B | PTA5 / PTE30 / PTB1 | D5 / D7 / A1 |
| Passive buzzer | S | PTB2 | A2 |
| ESP32 link | TX → ESP32 RX / RX ← ESP32 TX | PTE22 / PTE23 | - |
| Segment LCD | on board (leave JP7 shorted) | - | - |

**ESP32-S2-DevKitM-1U**

| Part | Pin |
|---|---|
| FRDM link RX / TX | GPIO 2 / GPIO 1 |
| EC11 A / B / SW, common → GND | GPIO 33 / 35 / 34 |
| USB keyboard cable to the laptop | white → GPIO19 (D−), green → GPIO20 (D+), black → GND, red unconnected |
| Micro-USB (CP2102) | laptop: power, upload, helper link |

## Building and flashing

**FRDM.** In MCUXpresso IDE (SDK_2.x_FRDM-MCXC444 installed): **File → Import → Existing Projects into
Workspace**, select `mcxc444/MCXC444_Project`, and leave **Copy projects into workspace unticked** so
the IDE builds the repo's files directly. Build, then **Run → Debug As → MCXC444_Project LinkServer
Debug** and press Resume (F8) when it stops at `main`. FreeRTOS preemption + time slicing are forced
on in `freertos/freertos-kernel/template/FreeRTOSConfig.h`. `PRINTF` goes to the MCU-Link USB port
at 115200 (prints `Doomscroll Breaker`; the LCD shows `OFF`).

**ESP32.** `bash tools/setup_esp32.sh` once (installs arduino-cli + the esp32 core, compiles), then
`bash tools/ESP32_upload.command` (compiles and uploads over the CP2102, no BOOT/RST needed). Board
options: `esp32:esp32:esp32s2:CDCOnBoot=default,UploadMode=default,PartitionScheme=huge_app` (CDC off,
so `Serial` is the CP2102 the helper reads). `bash tools/ESP32_monitor.command 30` resets the board and
logs 30 s of output to `logs/esp32_serial.log`.

Encoder: if one click changes the volume twice, set `ENC_STEPS_PER_DETENT` to 2; if it turns the
wrong way, set `ENC_REVERSED`. The video count is kept in NVS across ESP32 reboots and resets at the
helper's midnight.

## Laptop helper and browser lock

`tools/Doomscroll_helper.command` runs `doomscroll_keys.py`; keep its window open. It reads the
ESP32's serial lines, sends Telegram/Firebase requests with `curl`, sends the date, and serves the
device state to the extension. If the ESP32's own USB keyboard cable is **not** plugged in, it also
presses the keys and changes the volume on the Mac; set `FORWARD_INPUT = False` when the cable is in,
or everything happens twice.

Extension: Chrome → `chrome://extensions` → Developer mode → **Load unpacked** → `extension/`. Pages
start covered and unlock only if the state is `open`, `left > 0`, and the last `$STATE` line is under
4 s old, so unplugging the device or quitting the helper locks them again. Badge: videos left (green
= unlocked, red = locked, `OFF` = device or helper not running).

Telegram: create the bot with @BotFather; the friend must message it once; their chat id is printed
by `bash tools/Telegram_test.command`.

Known gaps: the helper can be quit (fails locked), the extension only covers Chrome and can be
removed, and phone apps are out of scope.

## Protocols

**FRDM ↔ ESP32 (UART2/UART1, 9600 8N1).** Every packet is 5 bytes: `0xAA, type, a, b, type ^ a ^ b`.
The FRDM owns the live count; the ESP32 saves it, resets it each day and sends it back with SYNC.

| Direction | Type | Meaning |
|---|---|---|
| FRDM → ESP32 | `0x01` HELLO | FRDM started; ESP32 ends any open session and replies SYNC |
| | `0x02` OPEN / `0x03` CLOSE | App opened / closed (close posts session stats) |
| | `0x04` NEXT | `a` = new video, `b` = videos left |
| | `0x05` PREV / `0x06` LIKE | Previous video / like |
| | `0x07` STATE | `a` = state (0 idle, 1 open, 2 locked, 3 waiting), `b` = videos left |
| | `0x08` ASK | `b` = videos asked for |
| ESP32 → FRDM | `0x10` SYNC | `a:b` = videos left today |
| | `0x11` APPROVED / `0x12` DENIED | Friend's reply |

**ESP32 ↔ laptop (CP2102, 115200, text lines).** Lines starting `DBG` or without `$` are just log.

| Direction | Line |
|---|---|
| ESP32 → laptop | `$STATE,<idle\|open\|locked\|waiting>,<left>` (1 Hz + on change) |
| | `$KEY,<down\|up\|like>`, `$VOL,<up\|down\|mute>` |
| | `$ASK,<watched today>,<bonus>` |
| | `$SESSION,<json>`, `$BUDGET,<date>,<left>,<watched>` (Firebase) |
| laptop → ESP32 | `$DATE,YYYY-MM-DD` (every 30 s), `$REPLY,APPROVED\|DENIED` |

## Troubleshooting

| Symptom | Fix |
|---|---|
| Joystick does nothing | Only the axis on A0 is read; 3V3 + common GND; thresholds `JOY_LOW`/`JOY_HIGH` (12-bit) |
| Joystick or claps do nothing | No common GND, or modules on 5 V |
| Keys do nothing on the laptop | Click the TikTok page so it has focus; check the GPIO19/20 cable or that the helper is running |
| Volume knob does nothing | EC11 A → 33, B → 35, SW → 34, common → GND |
| Stuck on `SEnt` | Helper not running; short press to cancel (it also gives up after 150 s) |
| ESP32 crash | `bash tools/ESP32_monitor.command`, then `bash tools/decode_crash.command` |
