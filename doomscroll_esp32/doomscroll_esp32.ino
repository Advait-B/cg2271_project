// Doomscroll Breaker - ESP32-S2 bridge.
//   FRDM <-> UART1 (9600, 5-byte packets)
//   laptop <-> USB serial (CP2102, 115200, "$..." text lines) - tools/doomscroll_keys.py does
//              Telegram, Firebase, the browser-lock server and the date
//   laptop <- native USB (GPIO19/20) as a HID keyboard + volume keys
//   EC11 rotary encoder = laptop volume knob
#include <Preferences.h>
#include <USB.h>
#include <USBHIDKeyboard.h>
#include <USBHIDConsumerControl.h>

#include "secrets.h"  // USER_NAME (the helper reads the other keys from the same file)

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
  Serial1.write(frame, PACKET_SIZE);
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
    Serial.printf("$BUDGET,%s,%u,%u\n", today, videosLeft, videosToday);
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
  Serial.printf("$SESSION,{\"user\":\"%s\",\"date\":\"%s\",\"seconds\":%lu,\"videos\":%u,\"likes\":%u,"
                "\"friendUnlocks\":%u}\n",
                USER_NAME, today, (unsigned long)((millis() - sessionStart) / 1000), sessionVideos,
                sessionLikes, sessionUnlocks);
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
    case PKT_ASK:
      Serial.printf("$ASK,%u,%u\n", (unsigned)videosToday, (unsigned)b);
      break;
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
// "$DATE,YYYY-MM-DD" (every 30 s) and "$REPLY,APPROVED" / "$REPLY,DENIED"
void handleLaptopLine(const char* line) {
  if (strncmp(line, "$DATE,", 6) == 0 && strlen(line + 6) == 10) {
    const char* date = line + 6;
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

// -------------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  uint32_t start = millis();
  while (!Serial && millis() - start < 5000) {
    delay(10);
  }
  Serial.println("\nDoomscroll Breaker starting");
  Serial1.begin(9600, SERIAL_8N1, MCX_RX_PIN, MCX_TX_PIN);

  keyboard.begin();
  consumer.begin();
  USB.productName("Doomscroll Breaker");
  USB.begin();
  initEncoder();

  prefs.begin("doomscroll", false);
  loadBudget();
  syncFrdm();
  Serial.printf("Ready: %u videos left (date comes from the laptop helper)\n", videosLeft);
}

void loop() {
  readMcx();
  readLaptop();
  pollEncoder();
  printState();
  delay(5);
}
