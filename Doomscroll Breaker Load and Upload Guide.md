# Doomscroll Breaker - Load & Upload Guide

Use the files in `Project/` - the only copy.

| Board | File | Tool |
|---|---|---|
| FRDM-MCXC444 | `Project/mcxc444/doomscroll_mcxc444.c` | MCUXpresso IDE |
| LOLIN ESP32-S2 Mini | `Project/doomscroll_esp32/doomscroll_esp32.ino` | Arduino IDE 2.x |

Wire everything first, following `schematics/Doomscroll Breaker Schematic BW.pdf`.

## A. FRDM-MCXC444 (MCUXpresso IDE)

1. **Install the SDK (one time only).** Open MCUXpresso IDE and go to the **Installed SDKs** tab. Drag in the FRDM-MCXC444 SDK zip. Build the SDK on the NXP SDK Builder with **FreeRTOS** ticked.
2. **Create the project.** Go to **File → New → Create a new C/C++ Project**, pick **frdmmcxc444** and click Next.
   - Give it a name such as `doomscroll`.
   - Under **Operating Systems**, tick **FreeRTOS kernel**. Leave the default drivers (clock, common, port, gpio, lpuart, debug console) ticked.
   - Click **Finish**. The wizard generates `board`, `pin_mux`, `clock_config`, `peripherals` and a main file in `source/`.
3. **Load the code.** Open the generated `source/doomscroll.c`, select all, and paste in the contents of `doomscroll_mcxc444.c`. Alternatively, delete that file and copy `doomscroll_mcxc444.c` into `source/`. Keep only one file that defines `main`.
4. **Edit `source/FreeRTOSConfig.h`.** This step is required, or the build stops at the `#error` on line 14.
   - `configUSE_TIME_SLICING` from **0 → 1**
   - Check that `configUSE_PREEMPTION` is **1** (the default).
   - Leave `configTOTAL_HEAP_SIZE` at its default of 10240. Anything ≥ 8 KB is fine.
5. **Build.** Select the project and click the hammer (**Build**). You should get 0 errors.
6. **Connect.** Plug the USB cable into the FRDM's **OpenSDA** USB port. This port both powers the board and programs it. Leave jumper **JP7** shorted.
7. **Flash.**
   - Click **Debug** (the blue bug) in the Quickstart panel and pick the detected LinkServer / CMSIS-DAP probe.
   - When it stops at `main`, press **Resume (F8)**.
   - To flash without debugging, use **GUI Flash Tool**, then press the board's **RESET** button.
8. **Check.** Open a serial terminal on the OpenSDA COM port at **115200 baud**. It should print `Doomscroll Breaker`, and the LCD should show `OFF`.

## B. ESP32-S2 Mini (Arduino IDE)

1. **Board package (one time only).**
   - Go to **File → Preferences → Additional boards manager URLs** and add `https://espressif.github.io/arduino-esp32/package_esp32_index.json`.
   - Open **Boards Manager** and install **esp32 by Espressif Systems**.
2. **Libraries (one time only).** In **Library Manager**, install **UniversalTelegramBot** (Brian Lough). When asked, accept the **ArduinoJson** dependency. The USB keyboard, WiFi, HTTPClient, Wire and Preferences libraries come with the board package.
3. **Open the sketch.** Open `Project/doomscroll_esp32/doomscroll_esp32.ino`. The folder name must match the file name, and it already does.
4. **Fill in the constants** at the top of the sketch:
   - `WIFI_SSID` and `WIFI_PASSWORD`. The network must be **2.4 GHz**; the S2 has no 5 GHz radio.
   - `BOT_TOKEN`: create a bot with **@BotFather** in Telegram.
   - `FRIEND_CHAT_ID`: your friend messages the bot first. Then open `https://api.telegram.org/bot<TOKEN>/getUpdates` and copy `message.chat.id`.
   - `FIREBASE_URL` and `FIREBASE_SECRET`: the Realtime Database URL and its database secret.
   - `USER_NAME`.
   - For the demo, set `BREAK_MS = 30000` (a 30 s break instead of 5 min).
5. **Tools menu:**
   - Board: **esp32 → LOLIN S2 Mini**
   - **USB CDC On Boot: Enabled**, so the Serial Monitor works alongside the keyboard.
   - Port: the S2 Mini's port.
6. **Upload.**
   - If no port shows up, or the upload fails, put the board in download mode: **hold 0, tap RST, release 0**. Then pick the new port and click **Upload**.
   - After the upload, press **RST** once to run the sketch.
7. **Check.** Open the **Serial Monitor** at 115200. You should see `Doomscroll Breaker starting` → `USB keyboard + volume keys ready` → `Volume knob ready (EC11 A=33 B=35 SW=34)` → `Connected: <IP>` → `Date: ...` → `Videos left today: 10` → `Ready`.

## C. Bring-up order

1. Join **GND** between the two boards. Keep the two **3V3** rails separate.
2. Plug in the ESP32 first (its USB-C goes to the laptop running TikTok), then the FRDM.
   - If the FRDM starts first, its `HELLO` packet is answered with `SYNC` once the ESP32 is ready.
   - The FRDM can be powered from the same laptop or from a USB charger.
3. Open TikTok in a browser on the laptop and click the video once, so the page has keyboard focus.
4. Test the controls:
   - Short press: opens the app (LED green, LCD shows the videos left).
   - Joystick up/down: next / previous video.
   - Clap: like.
   - At 0 videos: locked (`LOC`, red).
   - Long press: asks your friend (`SEnt`).
   - Phone face-down & still: break timer (`m:ss`, blue).

## D. If something fails

| Symptom | Fix |
|---|---|
| Build stops at `#error "Enable configUSE_PREEMPTION and configUSE_TIME_SLICING..."` | Step A4: set `configUSE_TIME_SLICING` to 1 |
| `multiple definition of main` | Two source files define `main`: delete the generated one (A3) |
| Volume knob does nothing | Check EC11 A → GPIO33, B → GPIO35, SW → GPIO34, middle pin(s) → GND |
| `WiFi failed` | Wrong SSID or password, or a 5 GHz-only hotspot |
| Joystick or claps do nothing | No common GND, or the modules are on 5 V instead of 3V3 |
| Keys do nothing on the laptop | Click the TikTok page so it has focus; the S2 Mini USB-C must go to the laptop |
| No reply from the friend | They must message the bot first; with no reply after 2 min, the request counts as denied |
