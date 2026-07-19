/* ═══════════════════════════════════════════════════════════════
 *   CLAWD ARTEMIS — Clawd on the CircuitMess Artemis watch
 *   ESP32-S3 · ST7735S 128×128 · 4 buttons · BM8563 RTC · buzzer
 *
 *   Hardware: board revision 2 (efuse), pin map from official
 *   GC_Artemis-Firmware Pins.cpp (Revision2).
 *   Restore stock: esptool write-flash 0 Artemis-v2.1.1.bin
 *
 *   Buttons:  UP/DOWN cycle views · SEL action (roll 8-ball, hearts)
 *             SEL long = brightness · ALT short = watchface
 *             ALT long (1.5s) = power off
 *   Serial:   w watch · e eyes · s squish · m aquarium · o 8ball
 *             r roll · h hearts · b battery · p status · time=EPOCH
 * ═══════════════════════════════════════════════════════════════ */

#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <SPI.h>
#include <Wire.h>
#include <WiFi.h>
#include "time.h"
#include "esp_random.h"
#include "esp_sleep.h"
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLE2902.h>
#include <Preferences.h>

// ── Pins (Artemis board revision 2) ───────────────────────────
#define PIN_BTN_UP    1
#define PIN_BTN_DOWN  2
#define PIN_BTN_SEL   3
#define PIN_BTN_ALT   21
#define PIN_TFT_SCK   36
#define PIN_TFT_MOSI  35
#define PIN_TFT_DC    48
#define PIN_TFT_RST   34
#define PIN_BL        33
#define PIN_BUZZ      47
#define PIN_BATT      5
#define PIN_USB       42
#define PIN_SDA       40
#define PIN_SCL       41
#define PIN_PWDN      37    // power latch — LOW = power off

#define SCREEN_ROT 3        // official rev-2 default; use 1 if upside down

// ── WiFi (one-shot NTP sync → RTC, then radio off) ────────────
#include "secrets.h"          // WIFI_SSID / WIFI_PASS — gitignored, see secrets.example.h
const char* STA_SSID = WIFI_SSID;
const char* STA_PASS = WIFI_PASS;
#define TZ_STR "PET5"

// ── Display ───────────────────────────────────────────────────
#define DISP_W 128
#define DISP_H 128
Adafruit_ST7735 tft(&SPI, -1, PIN_TFT_DC, PIN_TFT_RST);

// ── Eye constants (scaled from clawd_mochi 240→128) ───────────
#define EYE_W   16
#define EYE_H   32
#define EYE_GAP 40
#define EYE_OY  10

// ── Colours ───────────────────────────────────────────────────
uint16_t C_ORANGE, C_DARKBG, C_MUTED, C_GREEN, C_RED;
#define C_WHITE ST77XX_WHITE
#define C_BLACK ST77XX_BLACK

// ── Views ─────────────────────────────────────────────────────
#define VIEW_WATCH  0
#define VIEW_EYES   1
#define VIEW_SQUISH 2
#define VIEW_AQUA   3
#define VIEW_BALL   4
#define VIEW_USAGE  5
#define VIEW_NOTIF  6
#define VIEW_SET    7
#define VIEW_N      8
uint8_t currentView = VIEW_WATCH;
// Button order: DOWN from the watchface → notifications; UP → settings
const uint8_t VIEW_ORDER[VIEW_N] = { VIEW_WATCH, VIEW_NOTIF, VIEW_EYES, VIEW_SQUISH,
                                     VIEW_AQUA, VIEW_BALL, VIEW_USAGE, VIEW_SET };

// ── Settings (persisted in NVS, like the stock firmware) ─────
Preferences prefs;
bool     soundOn  = true;
uint8_t  sleepIdx = 2;               // 0=15s 1=30s 2=1m 3=never
bool     rotFlip  = false;
uint8_t  setSel   = 0;               // selected row in the settings menu
const uint32_t SLEEP_MS[4] = { 15000, 30000, 60000, 0 };

// ── Notifications (pushed via notif=<text> over USB/BLE) ─────
#define NOTIF_MAX 8
String  notifList[NOTIF_MAX];
uint8_t notifN = 0, notifUnread = 0;
bool    prevUsbIn = false;
uint8_t wfIconState = 255;    // forces first status-icon draw

// ── Claude usage (pushed by clawd-server.js over USB serial) ──
String        stRows[4]  = { "", "", "", "" };
String        stPrev[4]  = { "", "", "", "" };
String        stAct      = "";
bool          statsReady = false;
unsigned long lastStatMs = 0;

// ── Stock LEDs: 6 white (active HIGH) + RGB (active LOW) ─────
const uint8_t LED_PINS[6] = { 46, 45, 44, 43, 18, 17 };
#define PIN_RGB_R 14
#define PIN_RGB_G 12
#define PIN_RGB_B 13
bool ledsOn  = false;     // LEDs stay OFF — they used to glow on their own at boot
bool ledsNow = false;     // currently applied state

// ── Menu-style usage UI (Claude model-picker look) ───────────
uint16_t C_MENU_BG, C_MENU_HI, C_MENU_BADGE, C_MENU_LINE, C_MENU_TXT2;
int rowPcts[4] = { -1, -1, -1, -1 };
int usageHiRow = -1;

// ── BLE serial (Nordic UART service — same protocol as USB) ──
#define NUS_SVC "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_RX  "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_TX  "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"
volatile bool bleConnected = false;
#define BLE_RING_N 2048
volatile char bleRing[BLE_RING_N];
volatile uint16_t bleHead = 0, bleTail = 0;   // SPSC: BLE task writes, loop reads
String bleBuf = "";

// ── State ─────────────────────────────────────────────────────
struct Fish { int16_t x, y; int8_t vx; uint16_t col; };
struct Bub  { int16_t x, y; uint8_t r, vy; };
struct Btn  { uint8_t pin; bool prev; unsigned long downAt; bool longFired; };

bool          eyesAuto      = false;
bool          eyeClosed     = false;
unsigned long lastEyeToggle = 0;
unsigned long nextBlinkMs   = 0;

float         battEma   = -1;      // EMA of battery millivolts
int           battPct   = -1;
bool          usbIn     = false;
unsigned long lastBatt  = 0;

unsigned long lastActive = 0;      // backlight idle tracking
uint8_t       blLevel    = 100;    // user brightness (SEL long cycles)
uint8_t       blNow      = 255;    // currently applied level (forces first set)
bool          screenOff  = false;

int           wfLastMin  = -1;     // watchface incremental redraw
int           wfLastDay  = -1;
int           wfLastPct  = -999;
unsigned long wfNextBlink = 0;
bool          wfEyesShut  = false;
unsigned long wfBlinkEnd  = 0;

enum NetSt { NET_CONNECTING, NET_SYNCING, NET_DONE };
NetSt         netSt     = NET_CONNECTING;
unsigned long netStart  = 0;
bool          timeValid = false;
String        serialBuf = "";

// ── Logo (Anthropic mark, 240-space coords drawn at /2) ───────
#define LOGO_TRI_COUNT 162
static const int16_t LOGO_TRIS[][6] PROGMEM = {
  {120,105,65,134,100,114},{120,105,100,114,101,113},{120,105,101,113,100,112},
  {120,105,100,112,99,112},{120,105,99,112,93,111},{120,105,93,111,73,111},
  {120,105,73,111,55,110},{120,105,55,110,38,109},{120,105,38,109,34,108},
  {120,105,34,108,30,103},{120,105,30,103,30,100},{120,105,30,100,34,98},
  {120,105,34,98,39,98},{120,105,39,98,50,99},{120,105,50,99,67,100},
  {120,105,67,100,80,101},{120,105,80,101,98,103},{120,105,98,103,101,103},
  {120,105,101,103,101,102},{120,105,101,102,100,101},{120,105,100,101,100,100},
  {120,105,100,100,82,88},{120,105,82,88,63,76},{120,105,63,76,53,69},
  {120,105,53,69,48,65},{120,105,48,65,45,61},{120,105,45,61,44,54},
  {120,105,44,54,49,49},{120,105,49,49,55,49},{120,105,55,49,57,49},
  {120,105,57,49,64,55},{120,105,64,55,78,66},{120,105,78,66,96,79},
  {120,105,96,79,99,81},{120,105,99,81,100,81},{120,105,100,81,100,80},
  {120,105,100,80,99,78},{120,105,99,78,89,60},{120,105,89,60,78,41},
  {120,105,78,41,73,34},{120,105,73,34,72,29},{120,105,72,29,72,28},
  {120,105,72,28,72,27},{120,105,72,27,71,26},{120,105,71,26,71,25},
  {120,105,71,25,71,24},{120,105,71,24,77,16},{120,105,77,16,80,15},
  {120,105,80,15,87,16},{120,105,87,16,91,19},{120,105,91,19,95,29},
  {120,105,95,29,103,46},{120,105,103,46,114,68},{120,105,114,68,118,75},
  {120,105,118,75,119,81},{120,105,119,81,120,83},{120,105,120,83,121,83},
  {120,105,121,83,121,82},{120,105,121,82,122,69},{120,105,122,69,124,54},
  {120,105,124,54,126,34},{120,105,126,34,126,28},{120,105,126,28,129,21},
  {120,105,129,21,135,18},{120,105,135,18,139,20},{120,105,139,20,143,25},
  {120,105,143,25,142,28},{120,105,142,28,140,42},{120,105,140,42,136,64},
  {120,105,136,64,133,78},{120,105,133,78,135,78},{120,105,135,78,136,76},
  {120,105,136,76,144,67},{120,105,144,67,156,51},{120,105,156,51,162,45},
  {120,105,162,45,168,38},{120,105,168,38,172,35},{120,105,172,35,180,35},
  {120,105,180,35,185,43},{120,105,185,43,183,52},{120,105,183,52,175,62},
  {120,105,175,62,168,71},{120,105,168,71,159,83},{120,105,159,83,153,94},
  {120,105,153,94,154,94},{120,105,154,94,155,94},{120,105,155,94,176,90},
  {120,105,176,90,188,88},{120,105,188,88,201,85},{120,105,201,85,208,88},
  {120,105,208,88,208,91},{120,105,208,91,206,97},{120,105,206,97,191,101},
  {120,105,191,101,174,104},{120,105,174,104,148,110},{120,105,148,110,148,111},
  {120,105,148,111,148,111},{120,105,148,111,160,112},{120,105,160,112,165,112},
  {120,105,165,112,177,112},{120,105,177,112,200,114},{120,105,200,114,205,118},
  {120,105,205,118,209,123},{120,105,209,123,208,126},{120,105,208,126,199,131},
  {120,105,199,131,187,128},{120,105,187,128,159,121},{120,105,159,121,149,119},
  {120,105,149,119,147,119},{120,105,147,119,147,120},{120,105,147,120,156,128},
  {120,105,156,128,170,141},{120,105,170,141,189,158},{120,105,189,158,190,163},
  {120,105,190,163,188,166},{120,105,188,166,185,166},{120,105,185,166,169,153},
  {120,105,169,153,162,148},{120,105,162,148,148,136},{120,105,148,136,147,136},
  {120,105,147,136,147,137},{120,105,147,137,150,142},{120,105,150,142,168,168},
  {120,105,168,168,169,176},{120,105,169,176,168,179},{120,105,168,179,163,180},
  {120,105,163,180,158,179},{120,105,158,179,148,165},{120,105,148,165,137,149},
  {120,105,137,149,129,134},{120,105,129,134,128,135},{120,105,128,135,123,189},
  {120,105,123,189,120,192},{120,105,120,192,115,194},{120,105,115,194,110,191},
  {120,105,110,191,108,185},{120,105,108,185,110,174},{120,105,110,174,113,160},
  {120,105,113,160,116,148},{120,105,116,148,118,134},{120,105,118,134,119,129},
  {120,105,119,129,119,129},{120,105,119,129,118,129},{120,105,118,129,107,144},
  {120,105,107,144,91,166},{120,105,91,166,78,180},{120,105,78,180,75,181},
  {120,105,75,181,70,178},{120,105,70,178,70,173},{120,105,70,173,73,169},
  {120,105,73,169,91,146},{120,105,91,146,102,132},{120,105,102,132,109,124},
  {120,105,109,124,109,123},{120,105,109,123,108,123},{120,105,108,123,61,153},
  {120,105,61,153,52,155},{120,105,52,155,49,151},{120,105,49,151,49,146},
  {120,105,49,146,51,144},{120,105,51,144,65,134},{120,105,65,134,65,134},
};

// ═══════════════════════════════════════════════════════════════
//  SOUND + BACKLIGHT + POWER
// ═══════════════════════════════════════════════════════════════

void tone1(uint16_t f, uint16_t ms) {
  if (!soundOn) { delay(ms); return; }
  ledcWriteTone(PIN_BUZZ, f);
  delay(ms);
  ledcWriteTone(PIN_BUZZ, 0);
}
void chirpBtn()  { tone1(2200, 14); }
void chirpView() { tone1(1600, 18); tone1(2400, 22); }
void chirpBoot() { tone1(1300, 40); tone1(1900, 40); tone1(2600, 60); }
void chirpOff()  { tone1(2600, 50); tone1(1900, 50); tone1(1200, 80); }

void blApply(uint8_t pct) {
  if (pct == blNow) return;
  blNow = pct;
  uint32_t duty = (uint32_t)pct * pct * 255 / 10000;   // quadratic like stock
  ledcWrite(PIN_BL, duty);
}

void activity() {
  lastActive = millis();
  if (screenOff) screenOff = false;
  blApply(blLevel);
}

void ledsApply(bool on) {
  if (on == ledsNow) return;
  ledsNow = on;
  for (uint8_t p : LED_PINS) digitalWrite(p, on ? HIGH : LOW);
  // RGB glows clawd-orange; common-anode → inverted duty
  ledcWrite(PIN_RGB_R, on ? 255 - 230 : 255);
  ledcWrite(PIN_RGB_G, on ? 255 - 45  : 255);
  ledcWrite(PIN_RGB_B, 255);
}

void backlightTick() {
  unsigned long idle = millis() - lastActive;
  uint32_t offMs = SLEEP_MS[sleepIdx];
  if (offMs && idle > offMs && !usbIn)   { screenOff = true; blApply(0); }
  else if (offMs && idle > offMs * 2/3)  { blApply(blLevel > 30 ? 30 : blLevel); }
  else                                   { blApply(blLevel); }
  ledsApply(ledsOn && !screenOff);   // LEDs sleep with the screen
}

void powerOff() {
  chirpOff();
  tft.fillScreen(C_BLACK);
  blApply(0);
  digitalWrite(PIN_PWDN, LOW);
  delay(400);
  // Still alive → USB is powering the rail. Deep sleep, ALT wakes us.
  esp_sleep_enable_ext1_wakeup(1ULL << PIN_BTN_ALT, ESP_EXT1_WAKEUP_ANY_HIGH);
  esp_deep_sleep_start();
}

// ═══════════════════════════════════════════════════════════════
//  BM8563 RTC  (I2C 0x51 — register map from official RTC.cpp)
// ═══════════════════════════════════════════════════════════════
#define RTC_ADDR 0x51

uint8_t bcd2dec(uint8_t b) { return (b >> 4) * 10 + (b & 0x0F); }
uint8_t dec2bcd(uint8_t d) { return ((d / 10) << 4) | (d % 10); }

bool rtcRead(tm& t) {
  Wire.beginTransmission(RTC_ADDR);
  Wire.write(0x02);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(RTC_ADDR, 7) != 7) return false;
  uint8_t d[7];
  for (int i = 0; i < 7; i++) d[i] = Wire.read();
  if (d[0] & 0x80) return false;               // VL bit: time not trustworthy
  t = {};
  t.tm_sec  = bcd2dec(d[0] & 0x7F);
  t.tm_min  = bcd2dec(d[1] & 0x7F);
  t.tm_hour = bcd2dec(d[2] & 0x3F);
  t.tm_mday = bcd2dec(d[3] & 0x3F);
  t.tm_wday = bcd2dec(d[4] & 0x07);
  t.tm_mon  = bcd2dec(d[5] & 0x1F) - 1;
  t.tm_year = bcd2dec(d[6]) + ((d[5] & 0x80) ? 100 : 0);
  return t.tm_year >= 124;                     // sanity: 2024+
}

bool rtcWrite(const tm& t) {
  Wire.beginTransmission(RTC_ADDR);
  Wire.write(0x02);
  Wire.write(dec2bcd(t.tm_sec)  & 0x7F);
  Wire.write(dec2bcd(t.tm_min)  & 0x7F);
  Wire.write(dec2bcd(t.tm_hour) & 0x3F);
  Wire.write(dec2bcd(t.tm_mday) & 0x3F);
  Wire.write(dec2bcd(t.tm_wday) & 0x07);
  Wire.write((dec2bcd(t.tm_mon + 1) & 0x1F) | (t.tm_year >= 100 ? 0x80 : 0));
  Wire.write(dec2bcd(t.tm_year % 100));
  return Wire.endTransmission() == 0;
}

// ═══════════════════════════════════════════════════════════════
//  BATTERY  (divider ×4 + 75mV offset, 3600–4150mV = 0–100%)
// ═══════════════════════════════════════════════════════════════

void batteryTick() {
  if (millis() - lastBatt < 5000 && battEma > 0) return;
  lastBatt = millis();
  float mv = analogReadMilliVolts(PIN_BATT) * 4.0f + 75.0f;
  battEma = (battEma < 0) ? mv : battEma * 0.75f + mv * 0.25f;
  battPct = constrain((int)((battEma - 3600.0f) / (4150.0f - 3600.0f) * 100.0f), 0, 100);
}

void usbTick() {                       // plugging in wakes the main screen
  bool u = digitalRead(PIN_USB);
  if (u == prevUsbIn) return;
  prevUsbIn = u; usbIn = u;
  if (u) { tone1(1800, 25); tone1(2400, 35); }
  activity();
  if (currentView == VIEW_WATCH) { wfLastPct = -999; drawWatchface(false); }
  else if (u) showView(VIEW_WATCH);    // charge connect → show the watchface
}

// ═══════════════════════════════════════════════════════════════
//  LOGO SPLASH
// ═══════════════════════════════════════════════════════════════

void drawLogoSplash() {
  tft.fillScreen(C_ORANGE);
  for (uint16_t i = 0; i < LOGO_TRI_COUNT; i++) {
    tft.fillTriangle(
      pgm_read_word(&LOGO_TRIS[i][0]) / 2, pgm_read_word(&LOGO_TRIS[i][1]) / 2,
      pgm_read_word(&LOGO_TRIS[i][2]) / 2, pgm_read_word(&LOGO_TRIS[i][3]) / 2,
      pgm_read_word(&LOGO_TRIS[i][4]) / 2, pgm_read_word(&LOGO_TRIS[i][5]) / 2,
      C_WHITE);
    if (i % 8 == 0) delay(4);
  }
  tft.setTextColor(C_WHITE); tft.setTextSize(1);
  tft.setCursor(64 - 27, 110); tft.print("Anthropic");
}

// ═══════════════════════════════════════════════════════════════
//  EYES  (ported from clawd_mochi, scaled to 128)
// ═══════════════════════════════════════════════════════════════

int16_t eyeLX(int16_t ox) { return (DISP_W - (EYE_W * 2 + EYE_GAP)) / 2 + ox; }
int16_t eyeRX(int16_t ox) { return eyeLX(ox) + EYE_W + EYE_GAP; }
int16_t eyeY()            { return (DISP_H - EYE_H) / 2 - EYE_OY; }
int16_t eyeCY()           { return eyeY() + EYE_H / 2; }

void drawNormalEyes(int16_t ox, bool blink) {
  tft.fillScreen(C_ORANGE);
  const int16_t lx = eyeLX(ox), rx = eyeRX(ox), ey = eyeY();
  if (!blink) {
    tft.fillRect(lx, ey, EYE_W, EYE_H, C_BLACK);
    tft.fillRect(rx, ey, EYE_W, EYE_H, C_BLACK);
  } else {
    tft.fillRect(lx, ey + EYE_H / 2 - 2, EYE_W, 4, C_BLACK);
    tft.fillRect(rx, ey + EYE_H / 2 - 2, EYE_W, 4, C_BLACK);
  }
}

void drawChevron(int16_t cx, int16_t cy, int16_t arm, int16_t reach,
                 uint8_t thk, bool rightFacing, uint16_t col) {
  for (int8_t t = -(int8_t)thk; t <= (int8_t)thk; t++) {
    if (rightFacing) {
      tft.drawLine(cx - reach/2, cy - arm + t, cx + reach/2, cy + t,       col);
      tft.drawLine(cx + reach/2, cy + t,       cx - reach/2, cy + arm + t, col);
    } else {
      tft.drawLine(cx + reach/2, cy - arm + t, cx - reach/2, cy + t,       col);
      tft.drawLine(cx - reach/2, cy + t,       cx + reach/2, cy + arm + t, col);
    }
  }
}

void drawSquishEyes(bool closed) {
  tft.fillScreen(C_ORANGE);
  const int16_t lx = eyeLX(0), rx = eyeRX(0), cy = eyeCY();
  const int16_t arm = EYE_H / 2, reach = EYE_W / 2;
  if (!closed) {
    drawChevron(lx + EYE_W / 2, cy, arm, reach, 5, true,  C_BLACK);
    drawChevron(rx + EYE_W / 2, cy, arm, reach, 5, false, C_BLACK);
  } else {
    tft.fillRect(lx, cy - 3, EYE_W, 6, C_BLACK);
    tft.fillRect(rx, cy - 3, EYE_W, 6, C_BLACK);
  }
}

void animNormalEyes() {
  const int16_t offs[] = { -8, 8, -8, 8, 0 };
  for (uint8_t i = 0; i < 5; i++) { drawNormalEyes(offs[i], false); delay(70); }
  drawNormalEyes(0, true);  delay(90);
  drawNormalEyes(0, false);
}

void animSquishEyes() {
  for (uint8_t i = 0; i < 2; i++) {
    drawSquishEyes(false); delay(140);
    drawSquishEyes(true);  delay(90);
  }
  drawSquishEyes(false);
}

void animateEyes() {
  if (!eyesAuto) return;
  unsigned long now = millis();
  if (currentView == VIEW_EYES && now > nextBlinkMs) {   // quick blink
    drawNormalEyes(0, true); delay(90); drawNormalEyes(0, false);
    nextBlinkMs = now + 2500 + esp_random() % 3500;
  }
  if (now - lastEyeToggle < 8000) return;                // slow mood toggle
  lastEyeToggle = now;
  eyeClosed = !eyeClosed;
  if (eyeClosed) { currentView = VIEW_SQUISH; drawSquishEyes(false); }
  else           { currentView = VIEW_EYES;   drawNormalEyes(0, false); }
}

void popHearts() {
  for (int i = 0; i < 6; i++) {
    int x = 14 + esp_random() % 100, y = 18 + esp_random() % 74;
    uint16_t r = C_RED;
    tft.fillCircle(x - 3, y, 3, r); tft.fillCircle(x + 3, y, 3, r);
    tft.fillTriangle(x - 6, y + 1, x + 6, y + 1, x, y + 9, r);
  }
}

// ═══════════════════════════════════════════════════════════════
//  AQUARIUM  (scaled to 128)
// ═══════════════════════════════════════════════════════════════
#define FISH_N 3
#define BUB_N  4
Fish fish[FISH_N];
Bub  bub[BUB_N];
uint16_t C_SEA, C_SEA2, C_SAND;
bool          aquaInit = false;
unsigned long lastAqua = 0;
#define SEA_SPLIT 62
#define SAND_H    12

void aquaSetup() {
  C_SEA  = tft.color565(12, 70, 105);
  C_SEA2 = tft.color565(7, 45, 72);
  C_SAND = tft.color565(205, 175, 115);
  uint16_t cols[3] = { tft.color565(255,140,0), tft.color565(255,80,90),
                       tft.color565(120,210,255) };
  for (int i = 0; i < FISH_N; i++)
    fish[i] = { (int16_t)(14 + esp_random() % 100), (int16_t)(16 + esp_random() % 84),
                (int8_t)((esp_random() & 1) ? 2 : -2), cols[i % 3] };
  for (int i = 0; i < BUB_N; i++)
    bub[i]  = { (int16_t)(esp_random() % 128), (int16_t)(40 + esp_random() % 70),
                (uint8_t)(2 + esp_random() % 2), (uint8_t)(1 + esp_random() % 2) };
  aquaInit = true;
}

void drawFish(Fish &f) {
  int dir = f.vx > 0 ? 1 : -1;
  tft.fillCircle(f.x, f.y, 4, f.col);
  tft.fillTriangle(f.x - dir*4, f.y, f.x - dir*8, f.y - 3, f.x - dir*8, f.y + 3, f.col);
  tft.fillCircle(f.x + dir*2, f.y - 1, 1, C_BLACK);
}

void drawAquarium() {
  if (!aquaInit) aquaSetup();
  tft.fillRect(0, 0, DISP_W, SEA_SPLIT, C_SEA2);
  tft.fillRect(0, SEA_SPLIT, DISP_W, DISP_H - SEA_SPLIT, C_SEA);
  tft.fillRect(0, DISP_H - SAND_H, DISP_W, SAND_H, C_SAND);
  int cx = DISP_W / 2, cy = DISP_H - 7;              // little crab on the sand
  tft.fillCircle(cx, cy, 5, C_ORANGE);
  tft.fillCircle(cx - 8, cy, 3, C_ORANGE); tft.fillCircle(cx + 8, cy, 3, C_ORANGE);
  tft.fillCircle(cx - 2, cy - 2, 1, C_BLACK); tft.fillCircle(cx + 2, cy - 2, 1, C_BLACK);
  for (int i = 0; i < FISH_N; i++) drawFish(fish[i]);
  lastAqua = millis();
}

void animateAquarium() {
  if (currentView != VIEW_AQUA) return;
  unsigned long now = millis();
  if (now - lastAqua < 60) return;
  lastAqua = now;
  for (int i = 0; i < FISH_N; i++) {
    Fish &f = fish[i];
    tft.fillRect(f.x - 10, f.y - 6, 20, 12, f.y < SEA_SPLIT ? C_SEA2 : C_SEA);
    f.x += f.vx;
    if (f.x > DISP_W - 10) f.vx = -abs(f.vx);
    if (f.x < 10)          f.vx =  abs(f.vx);
    drawFish(f);
  }
  for (int i = 0; i < BUB_N; i++) {
    Bub &b = bub[i];
    tft.fillCircle(b.x, b.y, b.r, b.y < SEA_SPLIT ? C_SEA2 : C_SEA);
    b.y -= b.vy;
    if (b.y < 5) { b.y = DISP_H - 20; b.x = esp_random() % 128; }
    tft.drawCircle(b.x, b.y, b.r, C_WHITE);
  }
}

// ═══════════════════════════════════════════════════════════════
//  MAGIC 8-BALL
// ═══════════════════════════════════════════════════════════════
const char* BALL_ANS[] = { "Si", "No", "Tal vez", "Claro que si", "Lo dudo",
                           "Pregunta luego", "Sin duda", "Mejor no",
                           "Quiza", "Confia en ti" };
int ballIdx = -1;

void draw8Ball() {
  tft.fillScreen(C_DARKBG);
  tft.fillCircle(64, 58, 44, C_BLACK);
  tft.drawCircle(64, 58, 44, C_MUTED);
  tft.setTextColor(C_WHITE); tft.setTextSize(2);
  tft.setCursor(58, 26); tft.print("8");
  tft.fillTriangle(64, 82, 40, 60, 88, 60, tft.color565(28, 64, 168));
  const char* a = (ballIdx < 0) ? "..." : BALL_ANS[ballIdx];
  int w = strlen(a) * 6;
  tft.setTextColor(C_WHITE); tft.setTextSize(1);
  tft.setCursor(64 - w / 2, 64); tft.print(a);
  tft.setTextColor(C_MUTED);
  tft.setCursor(64 - 42, 116); tft.print("SEL: pregunta");
}

void roll8Ball() {
  ballIdx = esp_random() % (sizeof(BALL_ANS) / sizeof(BALL_ANS[0]));
  tone1(1800, 15); tone1(1400, 15);
  draw8Ball();
}

// ═══════════════════════════════════════════════════════════════
//  WATCHFACE
// ═══════════════════════════════════════════════════════════════
const char* DOW[]  = { "DOM", "LUN", "MAR", "MIE", "JUE", "VIE", "SAB" };
const char* MON[]  = { "ENE", "FEB", "MAR", "ABR", "MAY", "JUN",
                       "JUL", "AGO", "SEP", "OCT", "NOV", "DIC" };

void wfDrawBattery() {
  tft.fillRect(88, 4, 40, 10, C_ORANGE);
  tft.drawRect(112, 5, 12, 7, C_BLACK);
  tft.fillRect(124, 7, 2, 3, C_BLACK);
  if (battPct >= 0)
    tft.fillRect(113, 6, map(battPct, 0, 100, 0, 10), 5, C_BLACK);
  tft.setTextSize(1);
  tft.setTextColor(C_BLACK);
  if (battPct >= 0) {
    char b[6]; snprintf(b, sizeof(b), "%d", battPct);
    tft.setCursor(110 - strlen(b) * 6, 5); tft.print(b);
  }
}

uint8_t wfStatusCode() {
  uint8_t n = notifUnread > 9 ? 9 : notifUnread;
  return (bleConnected ? 1 : 0) | (usbIn ? 2 : 0) | (n << 2);
}

void wfDrawStatus() {          // top-center icons: BT · charging bolt · unread badge
  wfIconState = wfStatusCode();
  tft.fillRect(38, 1, 48, 14, C_ORANGE);
  int x = 40;
  if (bleConnected) {          // Bluetooth rune
    tft.drawLine(x + 4, 2, x + 4, 12, C_BLACK);
    tft.drawLine(x + 4, 2, x + 8, 5, C_BLACK);
    tft.drawLine(x + 8, 5, x, 10, C_BLACK);
    tft.drawLine(x + 4, 12, x + 8, 9, C_BLACK);
    tft.drawLine(x + 8, 9, x, 4, C_BLACK);
    x += 13;
  }
  if (usbIn) {                 // charging bolt
    tft.fillTriangle(x + 5, 1, x + 1, 8, x + 4, 8, C_BLACK);
    tft.fillTriangle(x + 3, 13, x + 7, 6, x + 4, 6, C_BLACK);
    x += 11;
  }
  if (notifUnread) {           // unread notifications badge
    tft.fillCircle(x + 5, 7, 5, C_BLACK);
    tft.setTextSize(1); tft.setTextColor(C_ORANGE);
    tft.setCursor(x + 3, 4); tft.print(notifUnread > 9 ? 9 : notifUnread);
  }
}

void wfDrawEyes(bool shut) {
  tft.fillRect(38, 96, 52, 18, C_ORANGE);
  if (!shut) {
    tft.fillRect(40, 96, 10, 16, C_BLACK);
    tft.fillRect(78, 96, 10, 16, C_BLACK);
  } else {
    tft.fillRect(40, 102, 10, 4, C_BLACK);
    tft.fillRect(78, 102, 10, 4, C_BLACK);
  }
}

void drawWatchface(bool full) {
  tm t;
  bool haveTime = getLocalTime(&t, 0);
  if (full) {
    tft.fillScreen(C_ORANGE);
    tft.setTextColor(C_BLACK); tft.setTextSize(1);
    tft.setCursor(4, 5); tft.print("clawd");
    wfLastMin = -1; wfLastDay = -1; wfLastPct = -999;
    wfDrawEyes(false); wfEyesShut = false;
    wfDrawStatus();
  }
  if (!haveTime) {
    if (full) {
      tft.setTextSize(2); tft.setCursor(28, 44); tft.print("--:--");
      tft.setTextSize(1); tft.setCursor(22, 70); tft.print("sin hora aun");
    }
    return;
  }
  if (t.tm_min != wfLastMin) {
    wfLastMin = t.tm_min;
    char hm[6]; snprintf(hm, sizeof(hm), "%02d:%02d", t.tm_hour, t.tm_min);
    tft.fillRect(0, 30, 128, 34, C_ORANGE);
    tft.setTextColor(C_BLACK); tft.setTextSize(4);
    tft.setCursor(4, 32); tft.print(hm);
  }
  // seconds progress bar
  int sw = map(t.tm_sec, 0, 59, 0, 120);
  tft.fillRect(4 + sw, 70, 120 - sw, 3, C_ORANGE);
  tft.fillRect(4, 70, sw, 3, C_BLACK);
  if (t.tm_mday != wfLastDay) {
    wfLastDay = t.tm_mday;
    char d[16]; snprintf(d, sizeof(d), "%s %d %s", DOW[t.tm_wday], t.tm_mday, MON[t.tm_mon]);
    tft.fillRect(0, 80, 128, 10, C_ORANGE);
    tft.setTextSize(1); tft.setTextColor(C_BLACK);
    tft.setCursor(64 - strlen(d) * 3, 80); tft.print(d);
  }
  if (battPct != wfLastPct) { wfLastPct = battPct; wfDrawBattery(); }
}

void watchTick() {
  if (currentView != VIEW_WATCH) return;
  static int lastSec = -1;
  tm t;
  if (getLocalTime(&t, 0)) {
    if (t.tm_sec == lastSec) {} else { lastSec = t.tm_sec; drawWatchface(false); }
  }
  if (wfStatusCode() != wfIconState) wfDrawStatus();
  unsigned long now = millis();
  if (!wfEyesShut && now > wfNextBlink) {
    wfEyesShut = true; wfDrawEyes(true); wfBlinkEnd = now + 110;
  } else if (wfEyesShut && now > wfBlinkEnd) {
    wfEyesShut = false; wfDrawEyes(false);
    wfNextBlink = now + 2800 + esp_random() % 3200;
  }
}

// ═══════════════════════════════════════════════════════════════
//  CLAUDE USAGE VIEW  (rows pushed by clawd-server.js: r1..r4 + act)
// ═══════════════════════════════════════════════════════════════

uint16_t pctColor(int pct) {
  if (pct >= 90) return C_RED;
  if (pct >= 70) return tft.color565(255, 180, 40);
  return C_MENU_TXT2;                        // healthy = quiet gray, menu-style
}

void drawUsageRow(uint8_t i) {
  int y = 16 + i * 24;
  tft.fillRect(0, y, 128, 24, C_MENU_BG);
  const String& s = stRows[i];
  if (!s.length()) return;
  int c1 = s.indexOf(':'), c2 = s.lastIndexOf(':');
  if (c1 < 0 || c2 <= c1) return;
  String label  = s.substring(0, c1);      if (label.length()  > 12) label  = label.substring(0, 12);
  String detail = s.substring(c1 + 1, c2); if (detail.length() > 16) detail = detail.substring(0, 16);
  int pct = s.substring(c2 + 1).toInt();
  bool hi = ((int) i == usageHiRow);
  if (hi) tft.fillRoundRect(2, y + 1, 124, 22, 5, C_MENU_HI);
  tft.setTextSize(1);
  tft.setTextColor(C_WHITE);
  tft.setCursor(8, y + 3); tft.print(label);
  char ps[6]; snprintf(ps, sizeof(ps), "%d%%", pct);
  tft.setTextColor(pctColor(pct));
  tft.setCursor(122 - strlen(ps) * 6, y + 3); tft.print(ps);
  if (detail.length()) {                      // rounded badge, like "Included until…"
    int bw = detail.length() * 6 + 8;
    tft.fillRoundRect(8, y + 13, bw, 10, 3, C_MENU_BADGE);
    tft.setTextColor(C_MENU_TXT2);
    tft.setCursor(12, y + 14); tft.print(detail);
  }
  stPrev[i] = s;
}

void drawUsageTicker() {
  tft.fillRect(0, 114, 128, 14, C_MENU_BG);
  tft.drawFastHLine(4, 114, 120, C_MENU_LINE);
  if (!stAct.length()) return;
  String a = stAct; if (a.length() > 18) a = a.substring(0, 18);
  tft.setTextSize(1); tft.setTextColor(C_MENU_TXT2);
  tft.setCursor(6, 119); tft.print(a);
  tft.setCursor(120, 119); tft.print(">");
}

void drawUsageView() {
  tft.fillScreen(C_MENU_BG);
  tft.setTextSize(1); tft.setTextColor(C_MENU_TXT2);
  tft.setCursor(6, 5); tft.print("Uso de Claude");
  if (bleConnected) { tft.setCursor(116, 5); tft.print("B"); }
  bool any = false;
  for (uint8_t i = 0; i < 4; i++) { drawUsageRow(i); if (stRows[i].length()) any = true; }
  if (!any) {
    tft.setTextColor(C_MENU_TXT2);
    tft.setCursor(64 - 45, 58); tft.print("esperando datos");
    tft.setCursor(64 - 51, 70); tft.print("(USB o Bluetooth)");
  }
  drawUsageTicker();
}

void applyStat(const String& k, const String& v) {
  if (k.length() == 2 && k[0] == 'r' && k[1] >= '1' && k[1] <= '4') {
    uint8_t i = k[1] - '1';
    stRows[i] = v;
    int c2 = v.lastIndexOf(':');
    rowPcts[i] = (c2 >= 0) ? v.substring(c2 + 1).toInt() : -1;
    int mi = -1, mv = -1;
    for (uint8_t j = 0; j < 4; j++) if (rowPcts[j] > mv) { mv = rowPcts[j]; mi = j; }
    lastStatMs = millis();
    if (!statsReady) { statsReady = true; usageHiRow = mi; showView(VIEW_USAGE); }
    else if (mi != usageHiRow) {
      usageHiRow = mi;
      if (currentView == VIEW_USAGE) for (uint8_t j = 0; j < 4; j++) drawUsageRow(j);
    } else if (currentView == VIEW_USAGE && stPrev[i] != v) drawUsageRow(i);
  } else if (k == "act") {
    stAct = v;
    if (currentView == VIEW_USAGE) drawUsageTicker();
  } else if (k == "notif") {
    addNotif(v);
  } else if (k == "cmd" && v.length() == 1) {
    switch (v[0]) {                        // crab web-panel (localhost:4848) compat
      case 'w': showView(VIEW_EYES);   break;
      case 's': showView(VIEW_SQUISH); break;
      case 'm': showView(VIEW_AQUA);   break;
      case 'o': showView(VIEW_BALL);   break;
      case 't': showView(VIEW_USAGE);  break;
      case 'r': showView(VIEW_BALL); roll8Ball(); break;
      case 'h': popHearts();           break;
    }
  }
  // tpm and unknown keys: ignored on purpose
}

// ═══════════════════════════════════════════════════════════════
//  SETTINGS  (SEL long opens it; persisted in NVS like stock)
// ═══════════════════════════════════════════════════════════════
const char* SET_LABELS[5] = { "Brillo", "Sonido", "Pantalla off", "Rotacion", "LEDs" };

void saveSettings() {
  prefs.putUChar("bl", blLevel);
  prefs.putBool("snd", soundOn);
  prefs.putUChar("slp", sleepIdx);
  prefs.putBool("rot", rotFlip);
  prefs.putBool("led", ledsOn);
}

void applyRotation() {
  tft.setRotation(rotFlip ? 1 : 3);
}

String setValue(uint8_t i) {
  switch (i) {
    case 0: return String(blLevel) + "%";
    case 1: return soundOn ? "On" : "Off";
    case 2: { const char* s[4] = { "15s", "30s", "1m", "Nunca" }; return s[sleepIdx]; }
    case 3: return rotFlip ? "Invertida" : "Normal";
    case 4: return ledsOn ? "On" : "Off";
  }
  return "";
}

void drawSettingsRow(uint8_t i) {
  int y = 18 + i * 18;
  tft.fillRect(0, y, 128, 18, C_MENU_BG);
  if (i == setSel) tft.fillRoundRect(2, y, 124, 17, 5, C_MENU_HI);
  tft.setTextSize(1);
  tft.setTextColor(C_WHITE);
  tft.setCursor(8, y + 5); tft.print(SET_LABELS[i]);
  String v = setValue(i);
  tft.setTextColor(C_MENU_TXT2);
  tft.setCursor(120 - v.length() * 6, y + 5); tft.print(v);
}

void drawSettingsView() {
  tft.fillScreen(C_MENU_BG);
  tft.setTextSize(1); tft.setTextColor(C_MENU_TXT2);
  tft.setCursor(6, 5); tft.print("Ajustes");
  for (uint8_t i = 0; i < 5; i++) drawSettingsRow(i);
  tft.setTextColor(C_MENU_TXT2);
  tft.setCursor(4, 115); tft.print("SEL cambia, ALT sale");
}

void settingsCycle() {
  switch (setSel) {
    case 0:
      blLevel = blLevel >= 100 ? 20 : (blLevel <= 20 ? 40 : (blLevel <= 40 ? 70 : 100));
      blNow = 255; blApply(blLevel);
      break;
    case 1: soundOn = !soundOn; break;
    case 2: sleepIdx = (sleepIdx + 1) % 4; break;
    case 3: rotFlip = !rotFlip; applyRotation(); break;
    case 4: ledsOn = !ledsOn; break;
  }
  saveSettings();
  if (setSel == 3) drawSettingsView();     // rotation redraws everything
  else drawSettingsRow(setSel);
}

// ═══════════════════════════════════════════════════════════════
//  NOTIFICATIONS  (DOWN from the watchface; pushed via notif=<text>)
// ═══════════════════════════════════════════════════════════════

void drawNotifView() {
  notifUnread = 0;
  tft.fillScreen(C_MENU_BG);
  tft.setTextSize(1); tft.setTextColor(C_MENU_TXT2);
  tft.setCursor(6, 5); tft.print("Notificaciones");
  if (!notifN) {
    tft.setCursor(64 - 21, 60); tft.print("(vacio)");
    return;
  }
  for (uint8_t i = 0; i < notifN && i < 4; i++) {
    int y = 17 + i * 21;
    int bar = notifList[i].indexOf('|');
    String stamp = bar >= 0 ? notifList[i].substring(0, bar) : "";
    String txt   = bar >= 0 ? notifList[i].substring(bar + 1) : notifList[i];
    if (txt.length() > 20) txt = txt.substring(0, 20);
    tft.setTextColor(C_MENU_TXT2);
    tft.setCursor(6, y); tft.print(stamp);
    tft.setTextColor(C_WHITE);
    tft.setCursor(6, y + 9); tft.print(txt);
    tft.drawFastHLine(4, y + 19, 120, C_MENU_LINE);
  }
  tft.setTextColor(C_MENU_TXT2);
  tft.setCursor(6, 120); tft.print("SEL: borrar");
}

void addNotif(String t) {
  if (t.length() > 60) t = t.substring(0, 60);
  tm tt; char stamp[8] = "";
  if (getLocalTime(&tt, 0)) snprintf(stamp, sizeof(stamp), "%02d:%02d", tt.tm_hour, tt.tm_min);
  for (int i = min((int) notifN, NOTIF_MAX - 1); i > 0; i--) notifList[i] = notifList[i - 1];
  notifList[0] = String(stamp) + "|" + t;
  if (notifN < NOTIF_MAX) notifN++;
  if (currentView != VIEW_NOTIF) notifUnread++;
  chirpView();
  activity();                                // wake the screen for it
  if (currentView == VIEW_NOTIF) drawNotifView();
}

// ═══════════════════════════════════════════════════════════════
//  VIEW SWITCHING + BUTTONS
// ═══════════════════════════════════════════════════════════════

void showView(uint8_t v) {
  eyesAuto = false;
  currentView = v;
  switch (v) {
    case VIEW_WATCH:  drawWatchface(true); wfNextBlink = millis() + 2000; break;
    case VIEW_EYES:   eyesAuto = true; eyeClosed = false;
                      lastEyeToggle = millis(); nextBlinkMs = millis() + 3000;
                      animNormalEyes(); break;
    case VIEW_SQUISH: eyesAuto = true; eyeClosed = true;
                      lastEyeToggle = millis(); animSquishEyes(); break;
    case VIEW_AQUA:   drawAquarium(); break;
    case VIEW_BALL:   draw8Ball(); break;
    case VIEW_USAGE:  drawUsageView(); break;
    case VIEW_NOTIF:  drawNotifView(); break;
    case VIEW_SET:    setSel = 0; drawSettingsView(); break;
  }
}

uint8_t viewOrderIdx(uint8_t v) {
  for (uint8_t i = 0; i < VIEW_N; i++) if (VIEW_ORDER[i] == v) return i;
  return 0;
}

Btn btns[4] = { { PIN_BTN_UP }, { PIN_BTN_DOWN }, { PIN_BTN_SEL }, { PIN_BTN_ALT } };

void btnShort(uint8_t i) {
  chirpBtn();
  if (currentView == VIEW_SET) {          // modal: buttons drive the menu
    uint8_t o = setSel;
    if      (i == 0) { setSel = (setSel + 4) % 5; drawSettingsRow(o); drawSettingsRow(setSel); }
    else if (i == 1) { setSel = (setSel + 1) % 5; drawSettingsRow(o); drawSettingsRow(setSel); }
    else if (i == 2) settingsCycle();
    else showView(VIEW_WATCH);
    return;
  }
  switch (i) {
    case 0: showView(VIEW_ORDER[(viewOrderIdx(currentView) + VIEW_N - 1) % VIEW_N]); break; // UP
    case 1: showView(VIEW_ORDER[(viewOrderIdx(currentView) + 1) % VIEW_N]); break;          // DOWN
    case 2:                                                          // SELECT
      if      (currentView == VIEW_BALL)  roll8Ball();
      else if (currentView == VIEW_EYES ||
               currentView == VIEW_SQUISH) popHearts();
      else if (currentView == VIEW_NOTIF) { notifN = 0; notifUnread = 0; drawNotifView(); }
      else if (currentView == VIEW_AQUA) { aquaInit = false; drawAquarium(); }
      else if (currentView == VIEW_WATCH) {                          // battery detail
        tft.fillRect(0, 116, 128, 12, C_ORANGE);
        tft.setTextSize(1); tft.setTextColor(C_BLACK);
        char s[24]; snprintf(s, sizeof(s), "%dmV %s", (int)battEma, usbIn ? "USB" : "BAT");
        tft.setCursor(64 - strlen(s) * 3, 117); tft.print(s);
      }
      break;
    case 3: showView(VIEW_WATCH); break;                             // ALT
  }
}

void btnLong(uint8_t i) {
  switch (i) {
    case 2: showView(VIEW_SET); break;                               // SELECT: settings
    case 3: powerOff(); break;                                       // ALT: off
    default: btnShort(i); break;
  }
}

void handleButtons() {
  unsigned long now = millis();
  for (uint8_t i = 0; i < 4; i++) {
    Btn &b = btns[i];
    bool s = digitalRead(b.pin);                 // active HIGH (pull-downs)
    if (!b.prev && s) { b.downAt = now; b.longFired = false; }
    if (b.prev && s && !b.longFired && now - b.downAt >= (i == 3 ? 1500 : 600)) {
      b.longFired = true;
      bool wasOff = screenOff;
      activity();
      if (!wasOff) btnLong(i);
    }
    if (b.prev && !s) {
      if (!b.longFired && now - b.downAt > 40) {
        bool wasOff = screenOff;
        activity();
        if (!wasOff) btnShort(i);                // first press only wakes screen
      }
    }
    b.prev = s;
  }
}

// ═══════════════════════════════════════════════════════════════
//  NET (one-shot NTP → RTC, then WiFi off)
// ═══════════════════════════════════════════════════════════════

void netTick() {
  if (netSt == NET_DONE) return;
  unsigned long now = millis();
  if (netSt == NET_CONNECTING) {
    if (WiFi.status() == WL_CONNECTED) {
      configTzTime(TZ_STR, "pool.ntp.org", "time.google.com");
      netSt = NET_SYNCING;
    } else if (now - netStart > 20000) {
      WiFi.disconnect(true); WiFi.mode(WIFI_OFF); netSt = NET_DONE;
    }
  } else if (netSt == NET_SYNCING) {
    tm t;
    if (getLocalTime(&t, 0) && t.tm_year >= 124) {
      rtcWrite(t);
      timeValid = true;
      WiFi.disconnect(true); WiFi.mode(WIFI_OFF); netSt = NET_DONE;
      Serial.println("ntp=ok");
      if (currentView == VIEW_WATCH) drawWatchface(true);
    } else if (now - netStart > 40000) {
      WiFi.disconnect(true); WiFi.mode(WIFI_OFF); netSt = NET_DONE;
    }
  }
}

// ═══════════════════════════════════════════════════════════════
//  PIN PROBE  (map Victor's hardware mods: button / LED / knob)
//  'D' digital watch · 'A' analog watch · 'X' off · led=N blink
// ═══════════════════════════════════════════════════════════════
const uint8_t PROBE_PINS[] = { 0, 6, 7, 8, 9, 10, 11, 15, 16, 38, 39 };
#define PROBE_N (sizeof(PROBE_PINS) / sizeof(PROBE_PINS[0]))
uint8_t probeMode = 0;          // 0 off · 1 digital · 2 analog
int     probeVal[PROBE_N];
unsigned long lastProbeMs = 0;

bool pinIsAdc(uint8_t p)  { return p >= 1 && p <= 20; }
bool pinLedSafe(uint8_t p) {
  // never drive: TFT/backlight/I2C/buzzer/buttons/batt/usb/power latch/USB D±
  const uint8_t banned[] = { 1, 2, 3, 5, 19, 20, 21, 33, 34, 35, 36, 37,
                             40, 41, 42, 47, 48 };
  for (uint8_t b : banned) if (p == b) return false;
  return true;
}

void probeStart(uint8_t mode) {
  probeMode = mode;
  for (uint8_t i = 0; i < PROBE_N; i++) {
    pinMode(PROBE_PINS[i], mode == 1 ? INPUT_PULLUP : INPUT);
    probeVal[i] = -9999;
  }
  Serial.printf("probe=%s\n", mode == 1 ? "digital" : "analog");
}

void probeStop() {
  for (uint8_t i = 0; i < PROBE_N; i++) pinMode(PROBE_PINS[i], INPUT);
  probeMode = 0;
  Serial.println("probe=off");
}

void probeTick() {
  if (!probeMode) return;
  unsigned long now = millis();
  if (now - lastProbeMs < 40) return;
  lastProbeMs = now;
  for (uint8_t i = 0; i < PROBE_N; i++) {
    uint8_t p = PROBE_PINS[i];
    if (probeMode == 1) {
      int v = digitalRead(p);
      if (v != probeVal[i]) {
        if (probeVal[i] != -9999) Serial.printf("P%d=%d\n", p, v);
        probeVal[i] = v;
      }
    } else if (pinIsAdc(p)) {
      int v = analogReadMilliVolts(p);
      if (abs(v - probeVal[i]) > 150) {
        if (probeVal[i] != -9999) Serial.printf("A%d=%dmV\n", p, v);
        probeVal[i] = v;
      }
    }
  }
}

void ledBlinkTest(uint8_t p) {
  if (!pinLedSafe(p)) { Serial.printf("led=%d BANNED\n", p); return; }
  pinMode(p, OUTPUT);
  for (int i = 0; i < 3; i++) {           // try both polarities
    digitalWrite(p, HIGH); delay(250);
    digitalWrite(p, LOW);  delay(250);
  }
  pinMode(p, INPUT);
  Serial.printf("led=%d done\n", p);
}

// ═══════════════════════════════════════════════════════════════
//  SERIAL
// ═══════════════════════════════════════════════════════════════

void printStatus() {
  tm t; bool ht = getLocalTime(&t, 0);
  Serial.printf("view=%d batt=%dmV pct=%d usb=%d time=%s ble=%d notifs=%d rev=2\n",
                currentView, (int)battEma, battPct, usbIn,
                ht ? "ok" : "unset", bleConnected, notifN);
}

void handleSerialLine(const String& line) {
  if (line.startsWith("time=")) {
    time_t epoch = (time_t) line.substring(5).toInt();
    timeval tv = { epoch, 0 };
    settimeofday(&tv, nullptr);
    setenv("TZ", TZ_STR, 1); tzset();
    tm t;
    if (getLocalTime(&t, 0)) { rtcWrite(t); timeValid = true; }
    Serial.println("time=set");
    if (currentView == VIEW_WATCH) drawWatchface(true);
    return;
  }
  if (line.startsWith("led=")) { ledBlinkTest((uint8_t) line.substring(4).toInt()); return; }
  if (line.length() > 1 && line.indexOf('=') > 0) {   // stats push: k=v;k=v;...
    int start = 0;
    while (start < (int) line.length()) {
      int semi = line.indexOf(';', start);
      if (semi < 0) semi = line.length();
      String pair = line.substring(start, semi);
      int eq = pair.indexOf('=');
      if (eq > 0) applyStat(pair.substring(0, eq), pair.substring(eq + 1));
      start = semi + 1;
    }
    return;
  }
  if (line.length() != 1) return;
  switch (line[0]) {
    case 'D': probeStart(1);         break;
    case 'A': probeStart(2);         break;
    case 'X': probeStop();           break;
    case 'w': showView(VIEW_WATCH);  break;
    case 'e': showView(VIEW_EYES);   break;
    case 's': showView(VIEW_SQUISH); break;
    case 'm': showView(VIEW_AQUA);   break;
    case 'o': showView(VIEW_BALL);   break;
    case 'u': showView(VIEW_USAGE);  break;
    case 'c': showView(VIEW_SET);    break;
    case 'r': showView(VIEW_BALL); roll8Ball(); break;
    case 'h': popHearts();           break;
    case 'l': ledsOn = !ledsOn; Serial.printf("leds=%d\n", ledsOn); break;
    case 'b': Serial.printf("batt=%dmV pct=%d usb=%d\n", (int)battEma, battPct, usbIn); break;
    case 'p': printStatus();         break;
  }
}

void handleSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (serialBuf.length()) { handleSerialLine(serialBuf); serialBuf = ""; }
    } else if (serialBuf.length() < 64) serialBuf += c;
  }
}

// ═══════════════════════════════════════════════════════════════
//  BLE  (Nordic UART service; runs the same protocol as USB serial)
// ═══════════════════════════════════════════════════════════════

class ClawdSrvCB : public BLEServerCallbacks {
  void onConnect(BLEServer*) override { bleConnected = true; }
  void onDisconnect(BLEServer*) override {
    bleConnected = false;
    BLEDevice::startAdvertising();
  }
};

class ClawdRxCB : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override {
    // BLE task context: only touch the ring buffer, never the display
    String v = c->getValue();
    for (int i = 0; i < (int) v.length(); i++) {
      uint16_t next = (bleHead + 1) % BLE_RING_N;
      if (next == bleTail) return;                 // full → drop the rest
      bleRing[bleHead] = v[i];
      bleHead = next;
    }
  }
};

void bleInit() {
  BLEDevice::init("Clawd Artemis");
  BLEDevice::setMTU(185);
  BLEServer* srv = BLEDevice::createServer();
  srv->setCallbacks(new ClawdSrvCB());
  BLEService* svc = srv->createService(NUS_SVC);
  BLECharacteristic* tx = svc->createCharacteristic(NUS_TX, BLECharacteristic::PROPERTY_NOTIFY);
  tx->addDescriptor(new BLE2902());
  BLECharacteristic* rx = svc->createCharacteristic(
      NUS_RX, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  rx->setCallbacks(new ClawdRxCB());
  svc->start();
  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(NUS_SVC);
  adv->setScanResponse(true);
  BLEDevice::startAdvertising();
  Serial.println("ble=ready");
}

void handleBle() {                                 // main-loop context: safe to draw
  while (bleTail != bleHead) {
    char ch = bleRing[bleTail];
    bleTail = (bleTail + 1) % BLE_RING_N;
    if (ch == '\n' || ch == '\r') {
      if (bleBuf.length()) { handleSerialLine(bleBuf); bleBuf = ""; }
    } else if (bleBuf.length() < 400) bleBuf += ch;
  }
}

// ═══════════════════════════════════════════════════════════════
//  SETUP + LOOP
// ═══════════════════════════════════════════════════════════════

void setup() {
  pinMode(PIN_PWDN, OUTPUT);            // FIRST: latch power on
  digitalWrite(PIN_PWDN, HIGH);

  Serial.begin(115200);

  pinMode(PIN_BTN_UP,   INPUT_PULLDOWN);
  pinMode(PIN_BTN_DOWN, INPUT_PULLDOWN);
  pinMode(PIN_BTN_SEL,  INPUT_PULLDOWN);
  pinMode(PIN_BTN_ALT,  INPUT_PULLDOWN);
  pinMode(PIN_USB,      INPUT);

  // Kill the self-glowing LEDs: drive every LED pin to its OFF state
  for (uint8_t p : LED_PINS) { pinMode(p, OUTPUT); digitalWrite(p, LOW); }
  ledcAttach(PIN_RGB_R, 5000, 8);
  ledcAttach(PIN_RGB_G, 5000, 8);
  ledcAttach(PIN_RGB_B, 5000, 8);
  ledcWrite(PIN_RGB_R, 255); ledcWrite(PIN_RGB_G, 255); ledcWrite(PIN_RGB_B, 255);

  ledcAttach(PIN_BL,   5000, 8);
  ledcAttach(PIN_BUZZ, 2000, 10);
  analogSetPinAttenuation(PIN_BATT, ADC_2_5db);
  pinMode(4, OUTPUT); digitalWrite(4, LOW);   // BattVref switch LOW = read battery

  Wire.begin(PIN_SDA, PIN_SCL, 400000);

  SPI.begin(PIN_TFT_SCK, -1, PIN_TFT_MOSI, -1);
  tft.initR(INITR_144GREENTAB);
  tft.setRotation(SCREEN_ROT);
  tft.setSPISpeed(40000000);

  C_ORANGE = tft.color565(218, 17, 0);
  C_DARKBG = tft.color565(10, 12, 16);
  C_MUTED  = tft.color565(90, 88, 86);
  C_GREEN  = tft.color565(80, 220, 130);
  C_RED    = tft.color565(255, 60, 90);
  C_MENU_BG    = tft.color565(30, 30, 32);
  C_MENU_HI    = tft.color565(56, 56, 60);
  C_MENU_BADGE = tft.color565(72, 72, 78);
  C_MENU_LINE  = tft.color565(58, 58, 62);
  C_MENU_TXT2  = tft.color565(152, 152, 158);

  prefs.begin("clawd", false);            // load persisted settings
  blLevel  = prefs.getUChar("bl", 100);
  soundOn  = prefs.getBool("snd", true);
  sleepIdx = prefs.getUChar("slp", 2);
  rotFlip  = prefs.getBool("rot", false);
  ledsOn   = prefs.getBool("led", false);
  applyRotation();

  blApply(blLevel);
  lastActive = millis();

  setenv("TZ", TZ_STR, 1); tzset();
  tm rt;
  if (rtcRead(rt)) {                    // seed clock from hardware RTC
    time_t epoch = mktime(&rt);
    timeval tv = { epoch, 0 };
    settimeofday(&tv, nullptr);
    timeValid = true;
  }

  drawLogoSplash();
  chirpBoot();
  usbIn = prevUsbIn = digitalRead(PIN_USB);
  batteryTick();
  delay(900);

  WiFi.mode(WIFI_STA);                  // one-shot NTP sync in background
  WiFi.begin(STA_SSID, STA_PASS);
  netStart = millis();

  bleInit();                            // "Clawd Artemis" — usage over Bluetooth

  Serial.println("CLAWD ARTEMIS boot rev=2");
  showView(VIEW_WATCH);
}

void loop() {
  handleButtons();
  handleSerial();
  handleBle();
  animateEyes();
  animateAquarium();
  watchTick();
  netTick();
  batteryTick();
  usbTick();
  backlightTick();
  probeTick();
  delay(5);
}
