#!/bin/bash
# Tests the Doomscroll Breaker Telegram bot from the Mac, using the token + chat id in the ESP32 sketch.
INO=~/Desktop/Y2S1/CG2271/Project/doomscroll_esp32/secrets.h
TOKEN=$(grep -o 'BOT_TOKEN = "[^"]*"' "$INO" | cut -d'"' -f2)
CHAT=$(grep -o 'FRIEND_CHAT_ID = "[^"]*"' "$INO" | cut -d'"' -f2)
API="https://api.telegram.org/bot$TOKEN"
show() { python3 -c 'import sys,json; print(json.dumps(json.load(sys.stdin), indent=2, ensure_ascii=False))' 2>/dev/null || cat; }

echo "Chat id in the code: $CHAT"
echo
echo "=== 1. Which bot is this token? ==="
curl -s "$API/getMe" | show
echo
echo "=== 2. Is a webhook set? (url should be empty) ==="
curl -s "$API/getWebhookInfo" | show
echo
echo "=== 3. Sending a test message to $CHAT ==="
curl -s "$API/sendMessage" -d chat_id="$CHAT" \
  --data-urlencode text="Test from the Mac: if you can see this, the bot works." \
  --data-urlencode 'reply_markup={"inline_keyboard":[[{"text":"Approve","callback_data":"approve"},{"text":"Deny","callback_data":"deny"}]]}' | show
echo
echo "=== 4. Recent messages to the bot (shows who has chatted with it + their chat id) ==="
curl -s "$API/getUpdates?limit=10" | python3 -c '
import sys,json
d=json.load(sys.stdin)
if not d.get("ok"): print(d); sys.exit()
if not d["result"]: print("(none waiting)")
for u in d["result"]:
    m=u.get("message") or u.get("callback_query",{})
    f=m.get("from",{})
    print("from", f.get("first_name"), "id", f.get("id"), "->", m.get("text") or m.get("data"))
' 2>/dev/null
