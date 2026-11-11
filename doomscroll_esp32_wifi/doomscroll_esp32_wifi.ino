// Doomscroll Breaker - ESP32-S2 bridge, OPTION 2: on-board Wi-Fi.
// Same as doomscroll_esp32/doomscroll_esp32.ino, except the ESP32 itself connects to Wi-Fi and calls
// the cloud services (the brief: "The ESP32 must connect to the internet"):
//   Telegram Bot API - ask the friend for more videos (Approve / Deny buttons)
//   Firebase Realtime Database - session stats (/sessions) and the daily budget (/budget/<user>/<date>)
//   SNTP - today's date for the midnight budget reset
// The network work runs in its own FreeRTOS task (netTask) fed by a queue, so loop() never blocks.
// If Wi-Fi or an HTTPS call fails, the same request is handed to the laptop helper as the usual
// "$ASK / $SESSION / $BUDGET" line, so the demo keeps working without Wi-Fi.
//   FRDM <-> UART1 (9600, 5-byte packets)
//   laptop <-> USB serial (CP2102, 115200, "$..." text lines): browser lock, fallback networking, date
//   laptop <- native USB (GPIO19/20) as a HID keyboard + volume keys
//   EC11 rotary encoder = laptop volume knob
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <Preferences.h>
#include <USB.h>
#include <USBHIDKeyboard.h>
#include <USBHIDConsumerControl.h>

#include "secrets.h"  // WIFI_SSID/PASSWORD, BOT_TOKEN, FRIEND_CHAT_ID, USER_NAME, FIREBASE_URL/SECRET

const int MCX_RX_PIN = 2;
const int MCX_TX_PIN = 1;
// EC11: common pin -> GND, internal pull-ups used
const int ENC_A_PIN = 33;
const int ENC_B_PIN = 35;
const int ENC_SW_PIN = 34;           // push switch -> mute
const int ENC_STEPS_PER_DETENT = 4;  // most EC11s give 4 quadrature steps per click
const bool ENC_REVERSED = false;     // flip if turning clockwise lowers the volume

const uint8_t DAILY_BUDGET = 10;
const uint32_t STATE_PRINT_MS = 1000;  // "$STATE,..." heartbeat for the browser lock
const uint32_t FRIEND_TIMEOUT_MS = 2UL * 60 * 1000;  // no reply in 2 min = Deny (FRDM backstop is 150 s)
const uint32_t TELEGRAM_POLL_MS = 3000;              // how often netTask checks for the friend's tap
const uint32_t WIFI_RETRY_MS = 15000;                // reconnect attempt interval while offline
const long GMT_OFFSET_S = 8 * 3600;                  // Singapore time, for the midnight reset

const uint8_t PACKET_START = 0xAA;
const uint8_t PACKET_SIZE = 5;

// must match mcxc444/MCXC444_Project/source/MCXC444_Project.c
enum PacketType : uint8_t {
  PKT_HELLO = 0x01,
  PKT_OPEN = 0x02,
  PKT_CLOSE = 0x03,
  PKT_NEXT = 0x04,   // a = new video?, b = left
  PKT_PREV = 0x05,
  PKT_LIKE = 0x06,
  PKT_STATE = 0x07,  // a = McxState, b = left
  PKT_ASK = 0x08,    // b = videos asked for
  PKT_SYNC = 0x10,
  PKT_APPROVED = 0x11,
  PKT_DENIED = 0x12
};

enum McxState : uint8_t {
  ST_IDLE,
  ST_OPEN,
  ST_LOCKED,
  ST_WAITING
};

USBHIDKeyboard keyboard;
USBHIDConsumerControl consumer;
Preferences prefs;
SemaphoreHandle_t uartMutex;  // loop() and netTask both send packets to the FRDM

// ---- jobs for netTask (one queue, so loop() never waits for the network) ----
enum JobType : uint8_t { JOB_ASK, JOB_SESSION, JOB_BUDGET };
struct NetJob {
  JobType type;
  uint16_t a;      // ASK: videos watched today  | BUDGET: videos left
  uint16_t b;      // ASK: videos asked for      | BUDGET: videos watched today
  char date[11];   // BUDGET: YYYY-MM-DD
  char json[192];  // SESSION: the stats record
};
QueueHandle_t netQueue;

uint8_t mcxState = ST_IDLE;
uint8_t videosLeft = DAILY_BUDGET;
uint16_t videosToday = 0;
char today[11] = "";  // "YYYY-MM-DD" from the laptop, empty until the first $DATE
bool stateDirty = true;

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

// ------------------------------------------------------------------------------- fallback + job queue
// Hands a job to the laptop helper (tools/doomscroll_keys.py) in the same format the Option 1 sketch uses.
void handToHelper(const NetJob& job) {
  switch (job.type) {
    case JOB_ASK:
      Serial.printf("$ASK,%u,%u\n", job.a, job.b);
      break;
    case JOB_SESSION:
      Serial.printf("$SESSION,%s\n", job.json);
      break;
    case JOB_BUDGET:
      Serial.printf("$BUDGET,%s,%u,%u\n", job.date, job.a, job.b);
      break;
  }
}

void queueJob(const NetJob& job) {
  // the friend request jumps the queue; if the queue is full the helper does it instead
  BaseType_t ok = (job.type == JOB_ASK) ? xQueueSendToFront(netQueue, &job, 0) : xQueueSend(netQueue, &job, 0);
  if (ok != pdTRUE) {
    Serial.println("DBG net queue full, handing the job to the laptop helper");
    handToHelper(job);
  }
}

// ------------------------------------------------------------------------------- budget (NVS)
void saveBudget() {
  static char savedDate[sizeof(today)] = "";
  static int savedLeft = -1;
  static int savedToday = -1;
  if (strcmp(savedDate, today) == 0 && savedLeft == videosLeft && savedToday == videosToday) {
    return;  // nothing changed, spare the flash
  }
  prefs.putString("date", today);
  prefs.putUChar("left", videosLeft);
  prefs.putUShort("watched", videosToday);
  strlcpy(savedDate, today, sizeof(savedDate));
  savedLeft = videosLeft;
  savedToday = videosToday;
}

void resetBudget() {
  videosLeft = DAILY_BUDGET;
  videosToday = 0;
  saveBudget();
}

// the saved count only counts if it's from today (or the date isn't known yet)
void loadBudget() {
  if (today[0] == '\0' || prefs.getString("date", "") == today) {
    videosLeft = min(prefs.getUChar("left", DAILY_BUDGET), DAILY_BUDGET);
    videosToday = prefs.getUShort("watched", 0);
  } else {
    resetBudget();
  }
}

void syncFrdm() {
  sendPacket(PKT_SYNC, videosLeft);
  stateDirty = true;
}

void uploadBudget() {
  if (today[0] != '\0') {
    NetJob job = {JOB_BUDGET, videosLeft, videosToday, "", ""};
    strlcpy(job.date, today, sizeof(job.date));
    queueJob(job);
  }
}

// ------------------------------------------------------------------------------- session stats
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
  NetJob job = {JOB_SESSION, 0, 0, "", ""};
  snprintf(job.json, sizeof(job.json),
           "{\"user\":\"%s\",\"date\":\"%s\",\"seconds\":%lu,\"videos\":%u,\"likes\":%u,"
           "\"friendUnlocks\":%u}",
           USER_NAME, today, (unsigned long)((millis() - sessionStart) / 1000), sessionVideos,
           sessionLikes, sessionUnlocks);
  queueJob(job);
  uploadBudget();
}

// ------------------------------------------------------------------------------- FRDM link
// The $KEY line lets the helper press the key on the Mac when the HID cable isn't plugged in.
void pressKey(uint8_t key, const char* name) {
  Serial.printf("$KEY,%s\n", name);
  keyboard.write(key);
}

void handlePacket(uint8_t type, uint8_t a, uint8_t b) {
  Serial.printf("DBG packet type=0x%02X a=%u b=%u\n", type, a, b);
  switch (type) {
    case PKT_HELLO:  // the FRDM just (re)started, so any open session is over
      endSession();
      mcxState = ST_IDLE;
      syncFrdm();
      break;
    case PKT_OPEN:
      startSession();
      break;
    case PKT_CLOSE:
      endSession();
      break;
    case PKT_NEXT:
      pressKey(KEY_DOWN_ARROW, "down");
      if (a) {
        videosToday++;
        sessionVideos++;
      }
      videosLeft = b;
      saveBudget();
      stateDirty = true;
      break;
    case PKT_PREV:
      pressKey(KEY_UP_ARROW, "up");
      break;
    case PKT_LIKE:
      pressKey('l', "like");
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
    case PKT_ASK: {
      NetJob job = {JOB_ASK, videosToday, b, "", ""};
      queueJob(job);  // netTask asks on Telegram (or hands it to the helper if offline)
      break;
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

// ------------------------------------------------------------------------------- laptop link
// Today's date comes from SNTP (Wi-Fi) or from the laptop helper's "$DATE" line, whichever is first.
void applyDate(const char* date) {
  if (strcmp(today, date) == 0) {
    return;
  }
  bool firstDate = today[0] == '\0';
  strlcpy(today, date, sizeof(today));
  if (firstDate) {
    loadBudget();  // the count loaded at boot didn't know the date; resets it if it's from an earlier day
  } else {
    resetBudget();  // midnight
  }
  Serial.printf("Date %s: %u videos left\n", today, videosLeft);
  syncFrdm();
}

// "$DATE,YYYY-MM-DD" (every 30 s) and "$REPLY,APPROVED" / "$REPLY,DENIED" (only when the helper
// did a fallback request)
void handleLaptopLine(const char* line) {
  if (strncmp(line, "$DATE,", 6) == 0 && strlen(line + 6) == 10) {
    applyDate(line + 6);
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

// Browser lock heartbeat: once a second and straight away on a change. Pulling the cable stops
// the lines, so the extension locks again (fail closed).
const char* stateName(uint8_t st) {
  switch (st) {
    case ST_OPEN: return "open";
    case ST_LOCKED: return "locked";
    case ST_WAITING: return "waiting";
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

// ------------------------------------------------------------------------------- volume knob
// Both encoder pins interrupt on every edge; the Gray-code table makes contact bounce cancel
// itself out (+1 then -1). loop() turns whole detents into volume presses, so the ISR never
// touches USB.
volatile int32_t encoderSteps = 0;
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

void pressVolume(uint16_t usage, const char* name) {
  Serial.printf("$VOL,%s\n", name);
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
    pressVolume(CONSUMER_CONTROL_VOLUME_INCREMENT, "up");
  }
  for (; detents < 0; detents++) {
    pressVolume(CONSUMER_CONTROL_VOLUME_DECREMENT, "down");
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
      pressVolume(CONSUMER_CONTROL_MUTE, "mute");
    }
  }
}

// ------------------------------------------------------------------------------- online (netTask)
bool timeSynced() {
  return time(nullptr) > 1700000000;  // SNTP has set the clock
}

// every 30 s: take today's date from the SNTP clock once it is set
void checkClockDate() {
  static uint32_t lastCheck = 0;
  if (millis() - lastCheck < 30000) {
    return;
  }
  lastCheck = millis();
  struct tm now;
  if (timeSynced() && getLocalTime(&now, 0)) {
    char date[11];
    strftime(date, sizeof(date), "%Y-%m-%d", &now);
    applyDate(date);
  }
}

// all but A-Z a-z 0-9 - _ . ~ become %XX
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

// Telegram Bot API: plain HTTPS GET /bot<TOKEN>/<method>?<params>. One TLS connection per call,
// closed straight away so nothing stays allocated. true if Telegram answers 200; body = the JSON.
bool telegramGet(const String& path, String& body) {
  body = "";
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }
  WiFiClientSecure client;
  client.setInsecure();  // no CA check, to save RAM
  client.setHandshakeTimeout(15);
  client.setTimeout(10);
  if (!client.connect("api.telegram.org", 443)) {  // by hostname, so SNI is sent
    Serial.printf("DBG telegram: TLS connect failed (heap %u, biggest block %u)\n", ESP.getFreeHeap(),
                  ESP.getMaxAllocHeap());
    return false;
  }
  client.print(String("GET /bot") + BOT_TOKEN + path + " HTTP/1.1\r\n" +
               "Host: api.telegram.org\r\nConnection: close\r\n\r\n");
  String status = client.readStringUntil('\n');
  bool ok = status.indexOf(" 200") >= 0;
  bool chunked = false;
  while (client.connected() || client.available()) {  // headers
    String h = client.readStringUntil('\n');
    if (h == "\r" || h.length() == 0) break;
    h.toLowerCase();
    if (h.startsWith("transfer-encoding:") && h.indexOf("chunked") >= 0) chunked = true;
  }
  uint32_t last = millis();
  while ((client.connected() || client.available()) && millis() - last < 8000) {  // body
    while (client.available()) {
      body += (char)client.read();
      last = millis();
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
  client.stop();
  if (chunked) {  // strip the "<hex length>\r\n ... \r\n" framing
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
  if (!ok) {
    Serial.printf("DBG telegram: %s\n", status.c_str());
  }
  return ok;
}

long long lastUpdateId = 0;

bool sendTelegramText(const char* text) {
  String body;
  return telegramGet(String("/sendMessage?chat_id=") + FRIEND_CHAT_ID + "&text=" + urlEncode(text), body);
}

// Sends the Approve / Deny message. Skips old button taps first so a stale "Approve" can't unlock it.
bool askFriend(uint16_t watched, uint16_t bonus) {
  String body;
  if (telegramGet("/getUpdates?offset=-1&limit=1&timeout=0", body)) {
    JsonDocument doc;
    if (!deserializeJson(doc, body)) {
      for (JsonObject u : doc["result"].as<JsonArray>()) {
        lastUpdateId = u["update_id"].as<long long>();
      }
    }
  }
  char text[128];
  snprintf(text, sizeof(text), "%s has watched %u videos today and wants %u more.", USER_NAME,
           (unsigned)watched, (unsigned)bonus);
  const char* keyboardJson =
      "{\"inline_keyboard\":[[{\"text\":\"Approve\",\"callback_data\":\"approve\"},"
      "{\"text\":\"Deny\",\"callback_data\":\"deny\"}]]}";
  String path = String("/sendMessage?chat_id=") + FRIEND_CHAT_ID + "&text=" + urlEncode(text) +
                "&reply_markup=" + urlEncode(keyboardJson);
  for (int attempt = 0; attempt < 2; attempt++) {
    if (telegramGet(path, body)) {
      Serial.println("$TELEGRAM,SENT,ASK");
      return true;
    }
    vTaskDelay(pdMS_TO_TICKS(1500));
  }
  Serial.println("$TELEGRAM,FAILED,ASK");
  return false;
}

// Looks for the friend's button tap. Only taps from FRIEND_CHAT_ID count. true once answered.
bool checkFriendReply(uint16_t bonus) {
  char path[128];
  snprintf(path, sizeof(path), "/getUpdates?offset=%lld&limit=5&timeout=0&allowed_updates=%s",
           lastUpdateId + 1, urlEncode("[\"callback_query\"]").c_str());
  String body;
  if (!telegramGet(path, body)) {
    return false;
  }
  JsonDocument filter;
  filter["result"][0]["update_id"] = true;
  filter["result"][0]["callback_query"]["id"] = true;
  filter["result"][0]["callback_query"]["data"] = true;
  filter["result"][0]["callback_query"]["from"]["id"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, body, DeserializationOption::Filter(filter))) {
    return false;
  }
  long long friendId = atoll(FRIEND_CHAT_ID);
  for (JsonObject u : doc["result"].as<JsonArray>()) {
    lastUpdateId = u["update_id"].as<long long>();
    JsonObject cb = u["callback_query"];
    if (cb.isNull() || cb["from"]["id"].as<long long>() != friendId) {
      continue;
    }
    bool approved = strcmp(cb["data"] | "", "approve") == 0;
    Serial.printf("DBG telegram: friend tapped %s\n", approved ? "approve" : "deny");
    String ack;
    telegramGet(String("/answerCallbackQuery?callback_query_id=") + urlEncode(cb["id"].as<const char*>()) +
                    "&text=" + (approved ? "Approved" : "Denied"),
                ack);
    sendPacket(approved ? PKT_APPROVED : PKT_DENIED);
    char reply[40] = "Denied.";
    if (approved) {
      snprintf(reply, sizeof(reply), "Approved: +%u videos.", (unsigned)bonus);
    }
    sendTelegramText(reply);
    return true;
  }
  return false;
}

// Firebase Realtime Database REST: <url><path>.json?auth=<secret>. Returns the HTTP code (-1 offline).
int firebaseWrite(const char* method, const String& path, const String& json) {
  if (WiFi.status() != WL_CONNECTED) {
    return -1;
  }
  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(10);
  HTTPClient http;
  http.setConnectTimeout(8000);
  http.setTimeout(8000);
  http.begin(client, String(FIREBASE_URL) + path + ".json?auth=" + FIREBASE_SECRET);
  http.addHeader("Content-Type", "application/json");
  int code = http.sendRequest(method, json);
  http.end();
  Serial.printf("DBG firebase %s %s -> %d\n", method, path.c_str(), code);
  return code;
}

// Does one job online; false if it couldn't (then the helper does it).
bool runJob(const NetJob& job, bool& waiting, uint32_t& askedAt, uint16_t& askBonus) {
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }
  switch (job.type) {
    case JOB_ASK:
      if (waiting) {
        return true;  // already waiting for the friend
      }
      if (!askFriend(job.a, job.b)) {
        return false;
      }
      waiting = true;
      askedAt = millis();
      askBonus = job.b;
      return true;
    case JOB_SESSION:
      return firebaseWrite("POST", "/sessions", job.json) == 200;
    case JOB_BUDGET: {
      char json[48];
      snprintf(json, sizeof(json), "{\"left\":%u,\"watched\":%u}", job.a, job.b);
      return firebaseWrite("PUT", String("/budget/") + USER_NAME + "/" + job.date, json) == 200;
    }
  }
  return false;
}

// FreeRTOS task on the ESP32: keeps Wi-Fi up, runs the queued jobs and polls Telegram for the reply.
void netTask(void*) {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.printf("Wi-Fi: connecting to %s\n", WIFI_SSID);

  bool clockStarted = false;
  uint32_t settleUntil = 0;
  uint32_t lastRetry = millis();
  bool waiting = false;
  uint32_t askedAt = 0;
  uint32_t lastPoll = 0;
  uint16_t askBonus = 0;

  for (;;) {
    if (WiFi.status() == WL_CONNECTED) {
      if (!clockStarted) {
        clockStarted = true;
        WiFi.setSleep(false);  // modem sleep on a phone hotspot often breaks TLS handshakes
        configTime(GMT_OFFSET_S, 0, "pool.ntp.org");
        settleUntil = millis() + 20000;
        Serial.printf("Wi-Fi connected: %s, %d dBm\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
      }
    } else if (millis() - lastRetry > WIFI_RETRY_MS) {
      lastRetry = millis();
      WiFi.reconnect();
    }
    // HTTPS during SNTP's own DNS lookup can trip an lwIP assert, so wait for the clock (max 20 s)
    bool settling = clockStarted && !timeSynced() && (int32_t)(settleUntil - millis()) > 0;

    NetJob job;
    if (!settling && xQueueReceive(netQueue, &job, pdMS_TO_TICKS(100)) == pdTRUE) {
      if (!runJob(job, waiting, askedAt, askBonus)) {
        Serial.println("DBG net: online call failed, handing it to the laptop helper");
        handToHelper(job);
      }
    } else if (settling) {
      vTaskDelay(pdMS_TO_TICKS(250));
    }

    if (waiting && millis() - lastPoll >= TELEGRAM_POLL_MS) {
      lastPoll = millis();
      if (checkFriendReply(askBonus)) {
        waiting = false;
      } else if (millis() - askedAt >= FRIEND_TIMEOUT_MS) {
        waiting = false;
        sendPacket(PKT_DENIED);
        sendTelegramText("Request expired.");
      }
    }
  }
}

// -------------------------------------------------------------------------------
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
  initEncoder();

  prefs.begin("doomscroll", false);
  loadBudget();
  syncFrdm();
  xTaskCreate(netTask, "net", 12288, NULL, 1, NULL);  // Wi-Fi, Telegram, Firebase
  Serial.printf("Ready: %u videos left (Wi-Fi connecting in the background)\n", videosLeft);
}

void loop() {
  readMcx();
  readLaptop();
  pollEncoder();
  printState();
  checkClockDate();
  delay(5);
}
