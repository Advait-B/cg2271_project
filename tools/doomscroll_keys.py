#!/usr/bin/env python3
"""Doomscroll Breaker - laptop-side key presser (Option B).

Reads the ESP32's serial output and, whenever it prints "DBG key 217" / "DBG key 218",
presses Down / Up arrow (and L for a clap/like) on this Mac so the frontmost window (TikTok) scrolls.
It also follows "DBG vol up/down/mute" from the EC11 volume knob and changes the Mac's volume.

Browser lock: the ESP32 prints "$STATE,<state>,<left>" every second. This script serves the latest
one on http://127.0.0.1:47271/state for the Chrome extension (Project/extension). If this script
isn't running, or the ESP32 stops talking (cable pulled), TikTok / Instagram stay locked.

Network: the ESP32's Wi-Fi is off (USE_WIFI = false in the sketch), so this script also does its
networking. "$ASK,<watched>" -> Telegram Approve/Deny message to the friend, answer goes back as
"$REPLY,APPROVED|DENIED". "$SESSION,<json>" / "$BUDGET,<date>,<left>,<watched>" -> Firebase.
It sends "$DATE,YYYY-MM-DD" to the ESP32 so the daily budget resets at midnight.
Bot token, chat id, Firebase URL/secret are read from the ESP32 sketch, so they live in one place.

Set PRESS_KEYS = False below if the ESP32's own USB keyboard cable (GPIO19/20) is plugged in,
otherwise every scroll would happen twice.

Run:   python3 ~/Desktop/Y2S1/CG2271/Project/tools/doomscroll_keys.py
Stop:  Ctrl-C
"""
import glob, json, os, select, subprocess, sys, termios, threading, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PRESS_KEYS = True       # False if the ESP32's own USB keyboard cable is connected
LOCK_PORT = 47271       # must match extension/background.js

device = {"state": None, "left": 0, "seen": 0.0}   # latest $STATE line from the ESP32
serial_fd = None                                    # current ESP32 port, for writing replies

SKETCH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "doomscroll_esp32", "doomscroll_esp32.ino")
FRIEND_TIMEOUT_S = 120


SECRETS = os.path.join(os.path.dirname(SKETCH), "secrets.h")


def sketch_constant(name):
    import re
    for path in (SECRETS, SKETCH):
        try:
            m = re.search(name + r'\s*=\s*"([^"]*)"', open(path).read())
            if m:
                return m.group(1)
        except OSError:
            pass
    return ""


BOT_TOKEN = sketch_constant("BOT_TOKEN")
FRIEND_CHAT_ID = sketch_constant("FRIEND_CHAT_ID")
USER_NAME = sketch_constant("USER_NAME")
FIREBASE_URL = sketch_constant("FIREBASE_URL")
FIREBASE_SECRET = sketch_constant("FIREBASE_SECRET")

KEYS = {217: ("key code 125", "DOWN  (next video)"),      # ESP32 KEY_DOWN_ARROW
        218: ("key code 126", "UP    (previous video)"),  # ESP32 KEY_UP_ARROW
        108: ('keystroke "l"', "L     (like - clap)")}     # ESP32 'l' = TikTok like shortcut

VOLUME_STEP = 6  # % per knob click (macOS volume keys move about 6 %)
VOLUME = {"up": f"set volume output volume ((output volume of (get volume settings)) + {VOLUME_STEP})",
          "down": f"set volume output volume ((output volume of (get volume settings)) - {VOLUME_STEP})",
          "mute": "set volume output muted (not (output muted of (get volume settings)))"}


class LockHandler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path.split("?")[0] != "/state":
            self.send_error(404)
            return
        age_ms = int((time.time() - device["seen"]) * 1000) if device["seen"] else None
        body = json.dumps({"state": device["state"], "left": device["left"], "ageMs": age_ms}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass  # keep the terminal quiet


def start_lock_server():
    server = ThreadingHTTPServer(("127.0.0.1", LOCK_PORT), LockHandler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    print(f"Browser lock server on http://127.0.0.1:{LOCK_PORT}/state")


def find_port():
    for pat in ("/dev/cu.SLAB_USBtoUART", "/dev/cu.usbserial-*", "/dev/cu.wchusbserial*"):
        hits = glob.glob(pat)
        if hits:
            return hits[0]
    return None


def open_port(port):
    fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    a = termios.tcgetattr(fd)
    a[0] = 0; a[1] = 0; a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL; a[3] = 0
    a[4] = a[5] = termios.B115200
    termios.tcsetattr(fd, termios.TCSANOW, a)
    return fd


def press(action):
    subprocess.Popen(["osascript", "-e", f'tell application "System Events" to {action}'],
                     stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)


# ---------------------------------------------------------------- network (curl = macOS's own TLS)
def curl(args, timeout=20):
    try:
        out = subprocess.run(["curl", "-sS", "--max-time", str(timeout)] + args,
                             capture_output=True, text=True, timeout=timeout + 5)
        return out.stdout
    except Exception as e:
        return json.dumps({"ok": False, "description": str(e)})


def telegram(method, **params):
    args = [f"https://api.telegram.org/bot{BOT_TOKEN}/{method}"]
    for k, v in params.items():
        args += ["--data-urlencode", f"{k}={v}"]
    try:
        return json.loads(curl(args, timeout=params.get("timeout", 0) + 15) or "{}")
    except ValueError:
        return {"ok": False}


def send_to_esp32(text):
    if serial_fd is not None:
        try:
            os.write(serial_fd, (text + "\n").encode())
        except OSError as e:
            print(f"   !!! couldn't write to the ESP32: {e}", flush=True)


asking = threading.Lock()


def ask_friend(watched):
    if not asking.acquire(blocking=False):
        print("   >>> already waiting for the friend", flush=True)
        return
    try:
        # skip old button taps so a stale "approve" can't unlock this request
        old = telegram("getUpdates", offset=-1, limit=1, timeout=0)
        last = max([u["update_id"] for u in old.get("result", [])] or [0])
        sent = telegram("sendMessage", chat_id=FRIEND_CHAT_ID,
                        text=f"{USER_NAME} has watched {watched} videos today and wants 10 more.",
                        reply_markup=json.dumps({"inline_keyboard": [[
                            {"text": "Approve", "callback_data": "approve"},
                            {"text": "Deny", "callback_data": "deny"}]]}))
        if not sent.get("ok"):
            print(f"   !!! Telegram send failed: {sent.get('description')}", flush=True)
            send_to_esp32("$REPLY,DENIED")
            return
        print("   >>> asked the friend on Telegram, waiting for Approve/Deny...", flush=True)
        end = time.time() + FRIEND_TIMEOUT_S
        while time.time() < end:
            upd = telegram("getUpdates", offset=last + 1, timeout=10,
                           allowed_updates=json.dumps(["callback_query"]))
            for u in upd.get("result", []):
                last = u["update_id"]
                cb = u.get("callback_query")
                if not cb or str(cb.get("from", {}).get("id")) != FRIEND_CHAT_ID:
                    continue
                approved = cb.get("data") == "approve"
                telegram("answerCallbackQuery", callback_query_id=cb["id"],
                         text="Approved" if approved else "Denied")
                send_to_esp32("$REPLY,APPROVED" if approved else "$REPLY,DENIED")
                telegram("sendMessage", chat_id=FRIEND_CHAT_ID,
                         text="Approved: +10 videos." if approved else "Denied.")
                print(f"   >>> friend {'APPROVED' if approved else 'DENIED'}", flush=True)
                return
        send_to_esp32("$REPLY,DENIED")
        telegram("sendMessage", chat_id=FRIEND_CHAT_ID, text="Request expired.")
        print("   >>> no reply in time: denied", flush=True)
    finally:
        asking.release()


def firebase(method, path, body):
    out = curl(["-X", method, "-H", "Content-Type: application/json", "-d", body,
                f"{FIREBASE_URL}{path}.json?auth={FIREBASE_SECRET}"])
    ok = out and '"error"' not in out
    print(f"   >>> Firebase {method} {path}: {'ok' if ok else out.strip()[:120]}", flush=True)


def in_background(fn, *args):
    threading.Thread(target=fn, args=args, daemon=True).start()


def handle(line):
    if line.startswith("$STATE,"):
        parts = line.split(",")
        if len(parts) == 3 and parts[2].isdigit():
            changed = (parts[1], int(parts[2])) != (device["state"], device["left"])
            device.update(state=parts[1], left=int(parts[2]), seen=time.time())
            if changed:
                print(f"   >>> device {parts[1]}, {parts[2]} videos left", flush=True)
        return
    print(line, flush=True)
    if line.startswith("$ASK,"):
        in_background(ask_friend, line.split(",", 1)[1])
        return
    if line.startswith("$SESSION,"):
        in_background(firebase, "POST", "/sessions", line.split(",", 1)[1])
        return
    if line.startswith("$BUDGET,"):
        parts = line.split(",")
        if len(parts) == 4:
            body = json.dumps({"left": int(parts[2]), "watched": int(parts[3])})
            in_background(firebase, "PUT", f"/budget/{USER_NAME}/{parts[1]}", body)
        return
    if line.startswith("DBG vol "):
        parts = line.split()
        what = parts[2] if len(parts) > 2 else ""
        if what in VOLUME:
            subprocess.Popen(["osascript", "-e", VOLUME[what]],
                             stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
            print(f"   >>> volume {what}", flush=True)
        return
    if line.startswith("DBG key "):
        try:
            k = int(line.split()[2])
        except (IndexError, ValueError):
            return
        if k in KEYS and PRESS_KEYS:
            code, name = KEYS[k]
            press(code)
            print(f"   >>> pressed {name}", flush=True)


def main():
    print("Doomscroll helper (keys + browser lock + Telegram/Firebase) - Ctrl-C to stop.")
    if not BOT_TOKEN:
        print(f"!!! couldn't read the bot token from {SKETCH}", flush=True)
    start_lock_server()
    print("Click on the TikTok video once so the browser has focus.\n")
    while True:
        port = find_port()
        if not port:
            print("ESP32 not found (is the micro-USB plugged in?) - retrying...", flush=True)
            time.sleep(2)
            continue
        try:
            fd = open_port(port)
        except OSError as e:
            print(f"Can't open {port}: {e} (close any other serial monitor) - retrying...", flush=True)
            time.sleep(2)
            continue
        print(f"Listening on {port}\n", flush=True)
        global serial_fd
        serial_fd = fd
        buf = b""
        last_date = 0.0
        try:
            while True:
                if time.time() - last_date > 30:  # the ESP32 has no clock of its own without Wi-Fi
                    last_date = time.time()
                    send_to_esp32("$DATE," + time.strftime("%Y-%m-%d"))
                r, _, _ = select.select([fd], [], [], 0.5)
                if not r:
                    continue
                try:
                    data = os.read(fd, 4096)
                except BlockingIOError:
                    continue
                if not data:
                    raise OSError("port closed")
                buf += data
                while b"\n" in buf:
                    raw, buf = buf.split(b"\n", 1)
                    line = raw.decode("utf-8", "replace").strip()
                    if line:
                        handle(line)
        except OSError as e:
            print(f"\nLost the ESP32 ({e}) - reconnecting...", flush=True)
            try:
                os.close(fd)
            except OSError:
                pass
            time.sleep(2)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nStopped.")
        sys.exit(0)
