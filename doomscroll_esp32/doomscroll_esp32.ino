#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <time.h>
#include <USB.h>
#include <USBHIDKeyboard.h>
#include <USBHIDConsumerControl.h>
#include <ArduinoJson.h>

#include "secrets.h"  // Wi-Fi, Telegram and Firebase keys (git-ignored; see secrets.example.h)

const int MCX_RX_PIN = 2;
const int MCX_TX_PIN = 1;
// EC11 rotary encoder = laptop volume knob (common pin -> GND, internal pull-ups used)
const int ENC_A_PIN = 33;   // EC11 "A" (sometimes labelled CLK)
const int ENC_B_PIN = 35;   // EC11 "B" (sometimes labelled DT)
const int ENC_SW_PIN = 34;  // EC11 push switch -> mute toggle
const int ENC_STEPS_PER_DETENT = 4;  // most EC11s give 4 quadrature steps per click; use 2 if one click = 2 volume steps
const bool ENC_REVERSED = false;     // flip if turning clockwise lowers the volume

// false: the ESP32's Wi-Fi stays off and Telegram + Firebase go through the laptop helper
// (tools/doomscroll_keys.py) over the USB serial cable. true: the old on-board Wi-Fi path.
const bool USE_WIFI = false;

const uint8_t DAILY_BUDGET = 10;
const uint32_t FRIEND_TIMEOUT_MS = 2UL * 60 * 1000;
const uint32_t DAY_CHECK_MS = 60000;
const uint32_t STATE_PRINT_MS = 1000;  // how often "$STATE,..." is printed for the laptop helper (browser lock)

const uint8_t PACKET_START = 0xAA;
const uint8_t PACKET_SIZE = 5;

enum PacketType : uint8_t {
  PKT_HELLO = 0x01,
  PKT_OPEN = 0x02,
  PKT_CLOSE = 0x03,
  PKT_NEXT = 0x04,
  PKT_PREV = 0x05,
  PKT_LIKE = 0x06,
  PKT_STATE = 0x07,
  PKT_ASK = 0x08,
  PKT_SYNC = 0x10,
  PKT_APPROVED = 0x11,
  PKT_DENIED = 0x12
  // 0x13-0x16 (break timer) were driven by the MPU-6050, which has been removed
};

enum McxState : uint8_t {
  ST_IDLE,
  ST_OPEN,
  ST_LOCKED,
  ST_WAITING,
  ST_BREAK
};

enum JobType : uint8_t {
  JOB_ASK,
  JOB_POST,
  JOB_PUT
};

struct NetJob {
  JobType type;
  char path[48];
  char json[192];
};

const uint32_t TELEGRAM_POLL_MS = 3000;

USBHIDKeyboard keyboard;
USBHIDConsumerControl consumer;
long long lastUpdateId = 0;
Preferences prefs;
SemaphoreHandle_t uartMutex;
QueueHandle_t netQueue;

volatile uint8_t mcxState = ST_IDLE;
volatile uint8_t videosLeft = DAILY_BUDGET;
volatile uint16_t videosToday = 0;
char today[11] = "";

volatile bool stateDirty = true;  // set when the browser lock should hear about a change straight away
volatile int32_t encoderSteps = 0;  // quadrature steps counted by the encoder interrupt

bool sessionActive = false;
uint32_t sessionStart = 0;
uint16_t sessionVideos = 0;
uint16_t sessionLikes = 0;
uint16_t sessionUnlocks = 0;

void sendPacket(uint8_t type, uint16_t value = 0) {
  uint8_t a = value >> 8;
  uint8_t b = value & 0xFF;
  uint8_t frame[PACKET_SIZE] = {PACKET_START, type, a, b, (uint8_t)(type ^ a ^ b)};
  xSemaphoreTake(uartMutex, portMAX_DELAY);
  Serial1.write(frame, PACKET_SIZE);
  xSemaphoreGive(uartMutex);
}

void queueJob(JobType type, const char* path, const char* json) {
  NetJob job = {type, "", ""};
  strlcpy(job.path, path, sizeof(job.path));
  strlcpy(job.json, json, sizeof(job.json));
  BaseType_t ok = (type == JOB_ASK) ? xQueueSendToFront(netQueue, &job, 0) : xQueueSend(netQueue, &job, 0);
  if (ok != pdTRUE) {
    Serial.printf("DBG net queue FULL, dropped job type %d\n", type);
  }
}

void updateToday() {
  struct tm now;
  if (getLocalTime(&now, 0)) {
    strftime(today, sizeof(today), "%Y-%m-%d", &now);
  }
}

void saveBudget() {
  prefs.putString("date", today);
  prefs.putUChar("left", videosLeft);
  prefs.putUShort("watched", videosToday);
}

void loadBudget() {
  if (today[0] == '\0' || prefs.getString("date", "") == today) {
    videosLeft = min(prefs.getUChar("left", DAILY_BUDGET), DAILY_BUDGET);  // old saves may hold the previous 30 budget
    videosToday = prefs.getUShort("watched", 0);
  } else {
    videosLeft = DAILY_BUDGET;
    videosToday = 0;
    saveBudget();
  }
}

void uploadBudget() {
  if (today[0] == '\0') {
    return;
  }
  char path[48];
  char json[64];
  snprintf(path, sizeof(path), "/budget/%s/%s", USER_NAME, today);
  snprintf(json, sizeof(json), "{\"left\":%u,\"watched\":%u}", videosLeft, videosToday);
  if (USE_WIFI) {
    queueJob(JOB_PUT, path, json);
  } else {
    Serial.printf("$BUDGET,%s,%u,%u\n", today, videosLeft, videosToday);  // helper writes it to Firebase
  }
}

void checkNewDay() {
  static uint32_t lastCheck = 0;
  if (millis() - lastCheck < DAY_CHECK_MS) {
    return;
  }
  lastCheck = millis();

  char previous[sizeof(today)];
  strlcpy(previous, today, sizeof(previous));
  updateToday();
  if (previous[0] == '\0' && today[0] != '\0') {
    // booted offline, so the saved count was loaded without knowing the date; check it now
    loadBudget();
    sendPacket(PKT_SYNC, videosLeft);
    stateDirty = true;
  } else if (previous[0] != '\0' && strcmp(previous, today) != 0) {
    videosLeft = DAILY_BUDGET;
    videosToday = 0;
    saveBudget();
    sendPacket(PKT_SYNC, videosLeft);
    stateDirty = true;
  }
}

void startSession() {
  sessionActive = true;
  sessionStart = millis();
  sessionVideos = 0;
  sessionLikes = 0;
  sessionUnlocks = 0;
}

void endSession() {
  if (!sessionActive) {
    return;
  }
  sessionActive = false;
  char json[192];
  snprintf(json, sizeof(json),
           "{\"user\":\"%s\",\"date\":\"%s\",\"seconds\":%lu,\"videos\":%u,\"likes\":%u,"
           "\"friendUnlocks\":%u}",
           USER_NAME, today, (unsigned long)((millis() - sessionStart) / 1000), sessionVideos,
           sessionLikes, sessionUnlocks);
  if (USE_WIFI) {
    queueJob(JOB_POST, "/sessions", json);
  } else {
    Serial.printf("$SESSION,%s\n", json);  // helper posts it to Firebase
  }
  uploadBudget();
}

void pressKey(uint8_t key) {
  Serial.printf("DBG key %u\n", key);
  keyboard.write(key);
}

void handlePacket(uint8_t type, uint8_t a, uint8_t b) {
  Serial.printf("DBG packet type=0x%02X a=%u b=%u\n", type, a, b);
  switch (type) {
    case PKT_HELLO:
      mcxState = ST_IDLE;  // the FRDM just (re)started, so the app is closed
      stateDirty = true;
      sendPacket(PKT_SYNC, videosLeft);
      break;
    case PKT_OPEN:
      startSession();
      break;
    case PKT_CLOSE:
      endSession();
      break;
    case PKT_NEXT:
      pressKey(KEY_DOWN_ARROW);
      if (a) {
        videosToday++;
        sessionVideos++;
      }
      videosLeft = b;
      saveBudget();
      stateDirty = true;
      break;
    case PKT_PREV:
      pressKey(KEY_UP_ARROW);
      break;
    case PKT_LIKE:
      pressKey('l');
      sessionLikes++;
      break;
    case PKT_STATE:
      if (mcxState == ST_WAITING && a == ST_OPEN) {
        sessionUnlocks++;
      }
      mcxState = a;
      videosLeft = b;
      stateDirty = true;
      saveBudget();
      uploadBudget();
      break;
    case PKT_ASK:
      if (USE_WIFI) {
        queueJob(JOB_ASK, "", "");
      } else {
        Serial.printf("$ASK,%u\n", (unsigned)videosToday);  // helper asks the friend on Telegram
      }
      break;
  }
}

// Lines from the laptop helper:  "$DATE,YYYY-MM-DD"  and  "$REPLY,APPROVED" / "$REPLY,DENIED"
void handleLaptopLine(const char* line) {
  if (strncmp(line, "$DATE,", 6) == 0 && strlen(line + 6) == 10) {
    const char* date = line + 6;
    if (today[0] == '\0') {
      strlcpy(today, date, sizeof(today));
      loadBudget();  // resets the budget if the saved count is from an earlier day
      Serial.printf("Date from laptop: %s, videos left %u\n", today, videosLeft);
    } else if (strcmp(today, date) != 0) {
      strlcpy(today, date, sizeof(today));
      videosLeft = DAILY_BUDGET;
      videosToday = 0;
      saveBudget();
      Serial.printf("New day %s: budget reset to %u\n", today, videosLeft);
    } else {
      return;
    }
    sendPacket(PKT_SYNC, videosLeft);
    stateDirty = true;
  } else if (strcmp(line, "$REPLY,APPROVED") == 0) {
    sendPacket(PKT_APPROVED);
  } else if (strcmp(line, "$REPLY,DENIED") == 0) {
    sendPacket(PKT_DENIED);
  }
}

void readLaptop() {
  static char buf[48];
  static uint8_t len = 0;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (len > 0) {
        buf[len] = '\0';
        handleLaptopLine(buf);
        len = 0;
      }
    } else if (len < sizeof(buf) - 1) {
      buf[len++] = c;
    } else {
      len = 0;  // too long, drop it
    }
  }
}

void readMcx() {
  static uint8_t frame[PACKET_SIZE];
  static uint8_t idx = 0;

  while (Serial1.available()) {
    uint8_t byte = Serial1.read();
    if (idx == 0 && byte != PACKET_START) {
      continue;
    }
    frame[idx++] = byte;
    if (idx == PACKET_SIZE) {
      idx = 0;
      if ((frame[1] ^ frame[2] ^ frame[3]) == frame[4]) {
        handlePacket(frame[1], frame[2], frame[3]);
      }
    }
  }
}

// ---------------------------------------------------------------------------------------------
// EC11 rotary encoder -> laptop volume (USB consumer-control keys, like a keyboard's volume keys).
// Both encoder pins interrupt on every edge; the ISR decodes the Gray-code sequence with a lookup
// table, so contact bounce just cancels itself out (+1 then -1). loop() turns whole detents into
// volume presses, so the ISR never touches USB.
// ---------------------------------------------------------------------------------------------
volatile uint8_t encoderLast = 0;

void IRAM_ATTR encoderIsr() {
  static const int8_t table[16] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};
  uint8_t now = (digitalRead(ENC_A_PIN) << 1) | digitalRead(ENC_B_PIN);
  encoderSteps += table[(encoderLast << 2) | now];
  encoderLast = now;
}

void initEncoder() {
  pinMode(ENC_A_PIN, INPUT_PULLUP);
  pinMode(ENC_B_PIN, INPUT_PULLUP);
  pinMode(ENC_SW_PIN, INPUT_PULLUP);
  encoderLast = (digitalRead(ENC_A_PIN) << 1) | digitalRead(ENC_B_PIN);
  attachInterrupt(digitalPinToInterrupt(ENC_A_PIN), encoderIsr, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENC_B_PIN), encoderIsr, CHANGE);
}

void pressConsumer(uint16_t usage, const char* name) {
  Serial.printf("DBG vol %s\n", name);  // also read by tools/doomscroll_keys.py (no-2nd-cable option)
  consumer.press(usage);
  consumer.release();
}

void pollEncoder() {
  noInterrupts();
  int32_t steps = encoderSteps;
  int32_t detents = steps / ENC_STEPS_PER_DETENT;
  encoderSteps = steps - detents * ENC_STEPS_PER_DETENT;  // keep any half-turned remainder
  interrupts();

  if (ENC_REVERSED) {
    detents = -detents;
  }
  for (; detents > 0; detents--) {
    pressConsumer(CONSUMER_CONTROL_VOLUME_INCREMENT, "up");
  }
  for (; detents < 0; detents++) {
    pressConsumer(CONSUMER_CONTROL_VOLUME_DECREMENT, "down");
  }

  // push switch: debounced, acts on the press (falling edge)
  static bool lastLevel = HIGH;
  static bool stable = HIGH;
  static uint32_t changedAt = 0;
  bool level = digitalRead(ENC_SW_PIN);
  if (level != lastLevel) {
    lastLevel = level;
    changedAt = millis();
  }
  if (level != stable && millis() - changedAt > 30) {
    stable = level;
    if (stable == LOW) {
      pressConsumer(CONSUMER_CONTROL_MUTE, "mute");
    }
  }
}

int postToFirebase(const char* method, const char* path, const char* json, bool quiet = false) {
  if (WiFi.status() != WL_CONNECTED) {
    if (!quiet) Serial.printf("DBG firebase %s %s skipped: no WiFi\n", method, path);
    return -1;
  }
  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(10);
  HTTPClient http;
  http.setConnectTimeout(8000);
  http.setTimeout(8000);
  String url = String(FIREBASE_URL) + path + ".json?auth=" + FIREBASE_SECRET;
  uint32_t t0 = millis();
  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");
  int code = http.sendRequest(method, String(json));
  http.end();
  if (!quiet || code != 200) {
    char tlsErr[64] = "";
    client.lastError(tlsErr, sizeof(tlsErr));
    Serial.printf("DBG firebase %s %s -> %d %s | tls: %s (%lu ms, heap %u, block %u, rssi %d)\n", method, path,
                  code, code < 0 ? HTTPClient::errorToString(code).c_str() : "", tlsErr,
                  (unsigned long)(millis() - t0), ESP.getFreeHeap(), ESP.getMaxAllocHeap(), WiFi.RSSI());
  }
  return code;
}

// After (re)connecting, SNTP looks up pool.ntp.org. Starting HTTPS during that lookup can trip an
// lwIP assert in the core, so network jobs wait until the clock is set (or 20 s have passed).
volatile uint32_t netGraceUntil = 0;

bool timeSynced() {
  return time(nullptr) > 1700000000;
}

bool netSettling() {
  return !timeSynced() && (int32_t)(netGraceUntil - millis()) > 0;
}

void waitForClock(uint32_t maxMs) {
  uint32_t start = millis();
  while (!timeSynced() && millis() - start < maxMs) {
    delay(100);
  }
}

bool ensureWiFi(uint32_t waitMs) {
  if (WiFi.status() == WL_CONNECTED) {
    return true;
  }
  Serial.printf("DBG wifi: not connected (status %d), reconnecting...\n", WiFi.status());
  // reconnect() retries with the saved config; disconnect()+begin() here clashed with auto-reconnect
  // ("sta is connecting, cannot set config")
  WiFi.reconnect();
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < waitMs) {
    delay(250);
  }
  bool ok = WiFi.status() == WL_CONNECTED;
  if (ok) {
    netGraceUntil = millis() + 20000;
  }
  Serial.printf("DBG wifi: %s\n", ok ? "reconnected" : "still offline - is the hotspot on and 2.4 GHz?");
  if (ok && today[0] == '\0') {
    configTime(8 * 3600, 0, "pool.ntp.org");
  }
  return ok;
}

// ---------------------------------------------------------------------------------------------
// Telegram, done the same way as the 2028 STM32 wearable (Core/Src/telegram.c), which worked:
//   plain HTTPS GET  "GET /bot<TOKEN>/<method>?<params url-encoded> HTTP/1.1"
//   "Host: api.telegram.org", "Connection: close", success = " 200" in the status line.
// One TLS connection per request, closed straight after, so nothing stays allocated.
// Log lines use the same $TELEGRAM / $TLS style so the result is easy to spot.
// ---------------------------------------------------------------------------------------------
const char* TELEGRAM_HOST = "api.telegram.org";
const uint16_t TELEGRAM_PORT = 443;
IPAddress telegramIp;          // looked up once, like the STM32 version
bool telegramIpKnown = false;
char telegramError[24] = "";   // which step failed
char telegramDetail[64] = "";  // http status line / extra detail

// all but A-Z a-z 0-9 - _ . ~ become %XX (same rule as telegram_send() on the STM32)
String urlEncode(const char* text) {
  static const char hex[] = "0123456789ABCDEF";
  String out;
  out.reserve(strlen(text) * 3);
  for (const unsigned char* c = (const unsigned char*)text; *c; c++) {
    if ((*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') ||
        *c == '-' || *c == '_' || *c == '.' || *c == '~') {
      out += (char)*c;
    } else {
      out += '%';
      out += hex[*c >> 4];
      out += hex[*c & 0x0F];
    }
  }
  return out;
}

// keeps the http status line (e.g. "HTTP/1.1 403 Forbidden") for the log
void keepStatusLine(const String& line) {
  size_t n = 0;
  for (size_t i = 0; i < line.length() && n + 1 < sizeof(telegramDetail); i++) {
    char c = line[i];
    if (c == '\r' || c == '\n') break;
    telegramDetail[n++] = (c == ',' || c < ' ' || c > '~') ? ' ' : c;
  }
  telegramDetail[n] = '\0';
}

// https GET to the bot api; true if telegram replies 200. `body` gets the JSON reply.
bool telegramGet(const String& path, String& body) {
  body = "";
  telegramError[0] = '\0';
  telegramDetail[0] = '\0';

  if (WiFi.status() != WL_CONNECTED) {
    strlcpy(telegramError, "WIFI_OFFLINE", sizeof(telegramError));
    return false;
  }
  // No WiFi.hostByName() here: in core 3.3.x it clears the DNS cache without the lwIP lock, which
  // crashed the board ("Required to lock TCPIP core") when SNTP's own lookup was still pending.
  // client.connect(host) below resolves the name itself.

  uint32_t heap = ESP.getFreeHeap();
  uint32_t block = ESP.getMaxAllocHeap();
  uint32_t t0 = millis();
  bool ok = false;
  {
    WiFiClientSecure client;
    client.setInsecure();          // no CA check (the STM32 verified; skipped here to save RAM)
    client.setHandshakeTimeout(15);
    client.setTimeout(10);
    // connect by hostname so SNI is sent (api.telegram.org needs it)
    if (!client.connect(TELEGRAM_HOST, TELEGRAM_PORT)) {
      telegramIpKnown = false;  // look the address up again next time
      strlcpy(telegramError, "TLS_CONNECT", sizeof(telegramError));
      char err[48] = "";
      int code = client.lastError(err, sizeof(err));
      snprintf(telegramDetail, sizeof(telegramDetail), "mbedtls=-0x%04x heap=%u block=%u",
               (unsigned)(code < 0 ? -code : code), heap, block);
      return false;
    }

    client.print(String("GET /bot") + BOT_TOKEN + path + " HTTP/1.1\r\n" +
                 "Host: " + TELEGRAM_HOST + "\r\n" +
                 "Connection: close\r\n\r\n");

    String status = client.readStringUntil('\n');
    if (status.length() == 0) {
      strlcpy(telegramError, "NO_REPLY", sizeof(telegramError));
      client.stop();
      return false;
    }
    keepStatusLine(status);
    ok = status.indexOf(" 200") >= 0;  // expects "HTTP/1.1 200 OK"

    // headers
    bool chunked = false;
    while (client.connected() || client.available()) {
      String h = client.readStringUntil('\n');
      if (h == "\r" || h.length() == 0) break;
      h.toLowerCase();
      if (h.startsWith("transfer-encoding:") && h.indexOf("chunked") >= 0) chunked = true;
    }
    // body (Connection: close, so read until the server hangs up)
    uint32_t last = millis();
    while ((client.connected() || client.available()) && millis() - last < 8000) {
      while (client.available()) {
        body += (char)client.read();
        last = millis();
      }
      delay(5);
    }
    client.stop();

    if (chunked) {  // strip "<hex len>\r\n ... \r\n" framing
      String plain;
      int pos = 0;
      while (pos < (int)body.length()) {
        int eol = body.indexOf("\r\n", pos);
        if (eol < 0) break;
        long len = strtol(body.substring(pos, eol).c_str(), nullptr, 16);
        if (len <= 0) break;
        plain += body.substring(eol + 2, eol + 2 + len);
        pos = eol + 2 + len + 2;
      }
      body = plain;
    }
  }
  if (!ok) {
    strlcpy(telegramError, "BAD_REPLY", sizeof(telegramError));  // telegram refused it
  } else {
    snprintf(telegramDetail, sizeof(telegramDetail), "ms=%lu", (unsigned long)(millis() - t0));
  }
  return ok;
}

void logTelegramError(const char* what) {
  Serial.printf("$TELEGRAM,ERROR,%s,%s,%s\n", what, telegramError, telegramDetail);
}

// tries a few times; 4xx errors (bad token / chat) are not retried
bool telegramGetRetry(const String& path, String& body, int tries) {
  for (int i = 0; i < tries; i++) {
    if (telegramGet(path, body)) return true;
    if (strstr(telegramDetail, " 4") != nullptr) return false;
    delay(1500);
    ensureWiFi(5000);
  }
  return false;
}

bool sendTelegramText(const char* text) {
  String path = String("/sendMessage?chat_id=") + FRIEND_CHAT_ID + "&text=" + urlEncode(text);
  String body;
  bool sent = telegramGetRetry(path, body, 2);
  if (!sent) logTelegramError("sendMessage");
  return sent;
}

// skip any old button presses so a stale "approve" can't unlock a new request
void skipOldUpdates() {
  String body;
  if (!telegramGet("/getUpdates?offset=-1&limit=1&timeout=0", body)) {
    logTelegramError("getUpdates");
    return;
  }
  JsonDocument doc;
  if (deserializeJson(doc, body)) return;
  for (JsonObject u : doc["result"].as<JsonArray>()) {
    lastUpdateId = u["update_id"].as<long long>();
  }
}

bool askFriend() {
  ensureWiFi(15000);
  skipOldUpdates();

  char text[128];
  snprintf(text, sizeof(text), "%s has watched %u videos today and wants 10 more.", USER_NAME,
           (unsigned)videosToday);
  const char* keyboardJson =
      "{\"inline_keyboard\":[[{\"text\":\"Approve\",\"callback_data\":\"approve\"},"
      "{\"text\":\"Deny\",\"callback_data\":\"deny\"}]]}";
  String path = String("/sendMessage?chat_id=") + FRIEND_CHAT_ID + "&text=" + urlEncode(text) +
                "&reply_markup=" + urlEncode(keyboardJson);

  Serial.println("DBG telegram: sending request to friend...");
  String body;
  bool sent = telegramGetRetry(path, body, 3);
  if (!sent) logTelegramError("sendMessage");
  Serial.printf("$TELEGRAM,%s,ASK\n", sent ? "SENT" : "FAILED");
  return sent;
}

bool checkFriendReply() {
  char path[128];
  snprintf(path, sizeof(path), "/getUpdates?offset=%lld&limit=5&timeout=0&allowed_updates=%s",
           lastUpdateId + 1, urlEncode("[\"callback_query\"]").c_str());
  String body;
  if (!telegramGet(path, body)) {
    logTelegramError("getUpdates");
    return false;
  }

  JsonDocument filter;
  filter["result"][0]["update_id"] = true;
  filter["result"][0]["callback_query"]["id"] = true;
  filter["result"][0]["callback_query"]["data"] = true;
  filter["result"][0]["callback_query"]["from"]["id"] = true;
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body, DeserializationOption::Filter(filter));
  if (err) {
    Serial.printf("DBG telegram: bad getUpdates JSON (%s)\n", err.c_str());
    return false;
  }

  long long friendId = atoll(FRIEND_CHAT_ID);
  for (JsonObject u : doc["result"].as<JsonArray>()) {
    lastUpdateId = u["update_id"].as<long long>();
    JsonObject cb = u["callback_query"];
    if (cb.isNull()) continue;
    long long fromId = cb["from"]["id"].as<long long>();
    const char* data = cb["data"] | "";
    if (fromId != friendId) {
      Serial.printf("DBG telegram: ignoring tap from %lld (not FRIEND_CHAT_ID)\n", fromId);
      continue;
    }
    bool approved = strcmp(data, "approve") == 0;
    Serial.printf("DBG telegram: friend tapped %s\n", data);

    String ackPath = String("/answerCallbackQuery?callback_query_id=") +
                     urlEncode(cb["id"].as<const char*>()) + "&text=" +
                     (approved ? "Approved" : "Denied");
    String ackBody;
    telegramGet(ackPath, ackBody);

    sendPacket(approved ? PKT_APPROVED : PKT_DENIED);
    sendTelegramText(approved ? "Approved: +10 videos." : "Denied.");
    return true;
  }
  return false;
}

// test the https path once at boot with getMe, so TLS problems show up early (TELEGRAM_BOOT_CHECK)
void telegramBootCheck() {
  Serial.println("$TLS,CHECKING,getMe");
  String body;
  if (telegramGet("/getMe", body)) {
    Serial.printf("$TLS,CHECK_OK,%s\n", telegramDetail);
  } else {
    Serial.printf("$TLS,CHECK_FAILED,%s,%s\n", telegramError, telegramDetail);
  }
}

// ---------------------------------------------------------------------------------------------
// Browser lock: the state goes to the laptop over the USB serial cable as "$STATE,<state>,<left>",
// once a second and straight away on every change. tools/doomscroll_keys.py reads it and serves it
// on http://127.0.0.1:47271/state to the Chrome extension (Project/extension). No Wi-Fi or TLS
// needed, and pulling the cable stops the lines, so the sites lock again (fail closed).
// ---------------------------------------------------------------------------------------------
const char* stateName(uint8_t st) {
  switch (st) {
    case ST_OPEN: return "open";
    case ST_LOCKED: return "locked";
    case ST_WAITING: return "waiting";
    case ST_BREAK: return "break";
    default: return "idle";
  }
}

void printState() {
  static uint32_t lastPrint = 0;
  if (!stateDirty && millis() - lastPrint < STATE_PRINT_MS) {
    return;
  }
  stateDirty = false;
  lastPrint = millis();
  Serial.printf("$STATE,%s,%u\n", stateName(mcxState), videosLeft);
}

void netTask(void*) {
  NetJob job;
  bool waiting = false;
  uint32_t askedAt = 0;
  uint32_t lastPoll = 0;

  uint32_t lastWifiCheck = 0;

  for (;;) {
    if (netSettling()) {
      vTaskDelay(pdMS_TO_TICKS(250));  // jobs stay queued until SNTP has finished its lookup
      continue;
    }
    if (WiFi.status() != WL_CONNECTED && millis() - lastWifiCheck > 15000) {
      lastWifiCheck = millis();
      ensureWiFi(10000);
    }
    if (xQueueReceive(netQueue, &job, pdMS_TO_TICKS(100)) == pdTRUE) {
      if (job.type == JOB_ASK) {
        Serial.println("DBG telegram: ASK job started");
        if (waiting) {
          Serial.println("DBG telegram: already waiting for the friend, ignoring repeat ASK");
        } else if (askFriend()) {
          waiting = true;
          askedAt = millis();
          lastPoll = millis();
        } else {
          sendPacket(PKT_DENIED);  // don't leave the FRDM stuck on "waiting" if the message never went out
        }
      } else {
        postToFirebase(job.type == JOB_POST ? "POST" : "PUT", job.path, job.json);
      }
    }
    if (!waiting || millis() - lastPoll < TELEGRAM_POLL_MS) {
      continue;
    }
    lastPoll = millis();
    if (checkFriendReply()) {
      waiting = false;
    } else if (millis() - askedAt >= FRIEND_TIMEOUT_MS) {
      waiting = false;
      sendPacket(PKT_DENIED);
      sendTelegramText("Request expired.");
    }
  }
}

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  // list what the radio can hear: all weak (below -85) = antenna problem; ours missing = 5 GHz/off
  int n = WiFi.scanNetworks();
  Serial.printf("WiFi scan: %d networks\n", n);
  for (int i = 0; i < n && i < 15; i++) {
    Serial.printf("  %-24s %4d dBm  ch %2d%s\n", WiFi.SSID(i).c_str(), WiFi.RSSI(i), WiFi.channel(i),
                  WiFi.SSID(i) == WIFI_SSID ? "  <-- ours" : "");
  }
  WiFi.scanDelete();
  Serial.printf("Connecting to %s", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);  // ESP32-S2 boards often fail to associate at full TX power
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(500);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    WiFi.setSleep(false);  // modem sleep on a phone hotspot often breaks TLS handshakes
    netGraceUntil = millis() + 20000;
    Serial.print("\nConnected: ");
    Serial.print(WiFi.localIP());
    Serial.printf("  rssi %d dBm, dns %s\n", WiFi.RSSI(), WiFi.dnsIP().toString().c_str());
  } else {
    Serial.printf("\nWiFi failed (status %d): check the hotspot name, password and 2.4 GHz\n", WiFi.status());
  }
}

void setup() {
  Serial.begin(115200);
  uint32_t start = millis();
  while (!Serial && millis() - start < 5000) {
    delay(10);
  }
  Serial.println("\nDoomscroll Breaker starting");
  Serial1.begin(9600, SERIAL_8N1, MCX_RX_PIN, MCX_TX_PIN);
  uartMutex = xSemaphoreCreateMutex();
  netQueue = xQueueCreate(8, sizeof(NetJob));

  keyboard.begin();
  consumer.begin();
  USB.productName("Doomscroll Breaker");
  USB.begin();
  Serial.println("USB keyboard + volume keys ready");

  initEncoder();
  Serial.println("Volume knob ready (EC11 A=33 B=35 SW=34)");

  if (USE_WIFI) {
    connectWiFi();
    configTime(8 * 3600, 0, "pool.ntp.org");
    struct tm now;
    waitForClock(15000);
    getLocalTime(&now, 0);
    updateToday();
    Serial.printf("Date: %s\n", today[0] ? today : "unknown");
    if (timeSynced()) {
      telegramBootCheck();
    } else {
      Serial.println("$TLS,CHECK_SKIPPED,clock not synced yet");
    }
  } else {
    WiFi.mode(WIFI_OFF);
    Serial.println("Wi-Fi off: Telegram/Firebase/date come from the laptop helper over USB");
  }

  prefs.begin("doomscroll", false);
  loadBudget();
  Serial.printf("Videos left today: %u\n", videosLeft);
  if (USE_WIFI) {
    WiFi.setAutoReconnect(true);
    xTaskCreate(netTask, "net", 12288, NULL, 1, NULL);
  }
  sendPacket(PKT_SYNC, videosLeft);
  Serial.printf("Ready (free heap %u, biggest block %u)\n", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
}

void loop() {
  readMcx();
  readLaptop();
  pollEncoder();
  printState();
  if (USE_WIFI) {
    checkNewDay();
  }
  delay(5);
}
