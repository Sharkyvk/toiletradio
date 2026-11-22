/*****************************************************************************
  TOILETRADIO – MUURPANEEL  v2.1
  Touchscreen-afstandsbediening in de inbouwdoos. Toont klok en 5 zenders,
  stuurt keuzes draadloos (UDP via je wifi) naar de plafondspeler en toont
  wat die terugmeldt. Geen audio, geen sensoren: alleen 5V nodig.
  v2.1: verbinding via UDP i.p.v. ESP-NOW (werkt ook met mesh-wifi).

  BOARD   : Waveshare ESP32-S3-Touch-LCD-1.69
  CORE    : Arduino-ESP32 3.x

  Arduino IDE -> Tools:
    Board            : "ESP32S3 Dev Module"
    Flash Size       : 16MB
    Partition Scheme : "16M Flash (3MB APP/9.9MB FATFS)"
    PSRAM            : "OPI PSRAM"
    USB CDC On Boot  : "Enabled"

  Libraries:
    - GFX Library for Arduino (moononournation)
    - U8g2 (olikraus)  -> alleen fonts
    - WiFiManager (tzapu)

  Het paneel logt in op hetzelfde wifi-netwerk als de speler, vindt de
  speler automatisch, haalt de tijd via internet en kan later draadloos
  geüpdatet worden (OTA).
  Eerste start: verbind met wifi "Toiletpaneel-Setup".
*****************************************************************************/

#include <WiFi.h>
#include <ArduinoOTA.h>
#include <WiFiManager.h>
#include <WiFiUdp.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <Arduino_GFX_Library.h>
#include <time.h>

// ---------------- Pinnen board ----------------
#define LCD_DC    4
#define LCD_CS    5
#define LCD_SCK   6
#define LCD_MOSI  7
#define LCD_RST   8
#define LCD_BL    15
#define TP_SDA    11
#define TP_SCL    10
#define TP_RST    13
#define TP_ADDR   0x15

const char* HOSTNAME = "toiletpaneel";
const char* OTA_PASS = "toilet";                       // wijzigen!
const char* TZ_INFO  = "CET-1CEST,M3.5.0,M10.5.0/3";

// ---------------- Kleuren ----------------
#define C_BG   0x0841
#define C_CARD 0x18E3
#define C_GOLD 0xCD4D
#define C_TEXT 0xE73B
#define C_DIM  0x8C51

// =========================================================================
//  Paneelprotocol via UDP  (MOET identiek zijn in plafond- en paneelfirmware)
// =========================================================================
#define TR_MAGIC 0x54524432UL
enum : uint8_t { MSG_PANEL = 1, MSG_STATUS = 2 };
enum : uint8_t { CMD_TOUCH = 1, CMD_SELECT = 2, CMD_STOP = 3, CMD_MUTE = 4, CMD_HELLO = 5 };

struct __attribute__((packed)) PanelMsg {
  uint32_t magic; uint8_t type; uint8_t cmd; uint8_t arg; uint16_t seq;
};

struct __attribute__((packed)) StatusMsg {
  uint32_t magic; uint8_t type;
  uint8_t state, station, muted, quiet, someone;
  uint8_t bri, briN;
  uint16_t scrT;
  char names[5][19];
};

#define UDP_PORT 4210
WiFiUDP udp;
IPAddress playerIP;            // geleerd uit de statusberichten van de speler

// ---------------- Objecten / status ----------------
Arduino_DataBus *bus = new Arduino_ESP32SPI(LCD_DC, LCD_CS, LCD_SCK, LCD_MOSI, GFX_NOT_DEFINED);
Arduino_GFX *gfx = new Arduino_ST7789(bus, LCD_RST, 0, true, 240, 280, 0, 20, 0, 20);

StatusMsg st = {};
bool haveStatus = false;
uint32_t lastStatusMs = 0;
uint16_t seq = 0;
uint32_t lastTouch = 0;
uint8_t blCur = 0;
bool uiDirty = true;
int lastMinute = -1;
bool lastLinkOk = false;

// =========================================================================
//  Verbinding met de speler (UDP)
// =========================================================================
void sendCmd(uint8_t cmd, uint8_t arg = 0) {
  PanelMsg m = { TR_MAGIC, MSG_PANEL, cmd, arg, ++seq };
  IPAddress to = (playerIP[0] != 0) ? playerIP : WiFi.broadcastIP();
  for (int i = 0; i < 2; i++) {                 // 2x versturen; speler negeert dubbele
    udp.beginPacket(to, UDP_PORT);
    udp.write((uint8_t*)&m, sizeof(m));
    udp.endPacket();
    delay(5);
  }
}

bool linkOk() { return haveStatus && millis() - lastStatusMs < 6000; }
bool isActive() { return st.state == 1 || st.state == 2 || st.state == 3; }

void pollStatus() {
  int len;
  while ((len = udp.parsePacket()) > 0) {
    StatusMsg m;
    if (len != sizeof(m)) { udp.flush(); continue; }
    udp.read((uint8_t*)&m, sizeof(m));
    if (m.magic != TR_MAGIC || m.type != MSG_STATUS) continue;
    playerIP = udp.remoteIP();
    if (!haveStatus || memcmp(&m, &st, sizeof(m)) != 0) uiDirty = true;
    st = m; haveStatus = true; lastStatusMs = millis();
  }
  bool ok = linkOk();
  if (ok != lastLinkOk) { lastLinkOk = ok; uiDirty = true; }

  static uint32_t lastHello = 0;               // levensteken + status opvragen
  if (millis() - lastHello > 5000) { lastHello = millis(); sendCmd(CMD_HELLO); }
}

// =========================================================================
//  Scherm
// =========================================================================
const int ROW_Y0 = 48, ROW_H = 42, ROW_X = 12, ROW_W = 216;

void printCentered(const String& s, int y) {
  int16_t x1, y1; uint16_t w, h;
  gfx->getTextBounds(s.c_str(), 0, y, &x1, &y1, &w, &h);
  gfx->setCursor((240 - w) / 2, y);
  gfx->print(s);
}

void drawMessage(const String& a, const String& b) {
  gfx->fillScreen(C_BG);
  gfx->setFont(u8g2_font_helvB14_tf); gfx->setTextColor(C_GOLD); printCentered(a, 120);
  gfx->setFont(u8g2_font_helvR10_tf); gfx->setTextColor(C_TEXT); printCentered(b, 150);
}

void drawUI() {
  uiDirty = false;
  if (!haveStatus) { drawMessage("Toiletradio", "radio zoeken..."); return; }

  gfx->fillScreen(C_BG);

  // Klok
  gfx->setFont(u8g2_font_helvB18_tf);
  gfx->setTextColor(C_TEXT);
  struct tm t; char buf[6] = "--:--";
  if (getLocalTime(&t, 0)) strftime(buf, sizeof(buf), "%H:%M", &t);
  printCentered(buf, 36);

  // Mute-icoon
  if (isActive() || st.muted) {
    int ix = 190, iy = 14;
    uint16_t c = st.muted ? C_DIM : C_GOLD;
    gfx->fillRect(ix, iy + 7, 6, 10, c);
    gfx->fillTriangle(ix + 5, iy + 12, ix + 14, iy + 2, ix + 14, iy + 22, c);
    if (st.muted) {
      gfx->drawLine(ix + 18, iy + 7, ix + 26, iy + 17, C_TEXT);
      gfx->drawLine(ix + 18, iy + 17, ix + 26, iy + 7, C_TEXT);
    } else {
      gfx->drawLine(ix + 18, iy + 9, ix + 18, iy + 15, c);
      gfx->drawLine(ix + 22, iy + 6, ix + 22, iy + 18, c);
    }
  }

  // Zenders
  bool playing = st.state == 2 || st.state == 3 || st.state == 4;
  for (int i = 0; i < 5; i++) {
    int y = ROW_Y0 + i * ROW_H;
    bool sel = (i == st.station);
    bool on = sel && playing;
    gfx->fillRoundRect(ROW_X, y + 3, ROW_W, ROW_H - 6, 10, on ? C_GOLD : C_CARD);
    if (sel && !on) gfx->drawRoundRect(ROW_X, y + 3, ROW_W, ROW_H - 6, 10, C_GOLD);

    gfx->setFont(u8g2_font_helvR12_tf);
    gfx->setTextColor(on ? C_BG : (sel ? C_GOLD : C_TEXT));
    gfx->setCursor(ROW_X + 14, y + ROW_H / 2 + 6);
    char name[20]; memcpy(name, st.names[i], 19); name[19] = 0;
    gfx->print(name);

    if (on) {
      int bx = ROW_X + ROW_W - 30, by = y + ROW_H / 2 + 8;
      gfx->fillRect(bx,      by - 10, 4, 10, C_BG);
      gfx->fillRect(bx + 7,  by - 16, 4, 16, C_BG);
      gfx->fillRect(bx + 14, by - 7,  4, 7,  C_BG);
    }
  }

  // Statusregel
  String s = "";
  if (!linkOk()) s = "geen verbinding met radio";
  else if (st.state == 1) s = "verbinden...";
  else if (st.quiet) s = "stille uren";
  gfx->setFont(u8g2_font_helvR08_tf);
  gfx->setTextColor(C_DIM);
  printCentered(s, 272);
}

void updateBacklight() {
  static uint32_t t = 0;
  if (millis() - t < 15) return;
  t = millis();
  uint16_t scrT = haveStatus ? st.scrT : 30;
  bool on = (haveStatus && (st.state != 0 || st.someone)) || (millis() - lastTouch < scrT * 1000UL);
  uint8_t day = haveStatus ? st.bri : 180, night = haveStatus ? st.briN : 25;
  uint8_t target = on ? ((haveStatus && st.quiet) ? night : day) : 0;
  if (blCur < target)      blCur = min<int>(target, blCur + 5);
  else if (blCur > target) blCur = max<int>(target, blCur - 3);
  ledcWrite(LCD_BL, blCur);
}

// =========================================================================
//  Touch (CST816T)
// =========================================================================
void touchInit() {
  pinMode(TP_RST, OUTPUT);
  digitalWrite(TP_RST, LOW);  delay(10);
  digitalWrite(TP_RST, HIGH); delay(60);
  Wire.begin(TP_SDA, TP_SCL, 400000);
  Wire.beginTransmission(TP_ADDR);
  Wire.write(0xFE); Wire.write(0x01);            // auto-slaap uit
  Wire.endTransmission();
}

bool readTouch(int& x, int& y) {
  Wire.beginTransmission(TP_ADDR);
  Wire.write(0x02);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((uint8_t)TP_ADDR, (uint8_t)5) != 5) return false;
  uint8_t f = Wire.read(), xh = Wire.read(), xl = Wire.read(), yh = Wire.read(), yl = Wire.read();
  if (f == 0) return false;
  x = ((xh & 0x0F) << 8) | xl;
  y = ((yh & 0x0F) << 8) | yl;
  return true;
}

void handleTap(int x, int y) {
  bool wasDark = blCur < 10;
  lastTouch = millis();
  sendCmd(CMD_TOUCH);                            // telt als aanwezigheid
  if (wasDark || !linkOk()) { uiDirty = true; return; }

  if (y < ROW_Y0 && x > 170) { sendCmd(CMD_MUTE); return; }
  if (y < ROW_Y0 || y >= ROW_Y0 + 5 * ROW_H) return;
  sendCmd(CMD_SELECT, (y - ROW_Y0) / ROW_H);
}

void updateTouch() {
  static uint32_t t = 0; static bool down = false;
  if (millis() - t < 25) return;
  t = millis();
  int x, y;
  bool now = readTouch(x, y);
  if (now && !down) handleTap(x, y);
  down = now;
}

// =========================================================================
//  Setup / loop
// =========================================================================
void setup() {
  Serial.begin(115200);
  ledcAttach(LCD_BL, 5000, 8);
  ledcWrite(LCD_BL, 0);
  gfx->begin();
  gfx->setUTF8Print(true);
  touchInit();

  drawMessage("Toiletradio", "wifi verbinden...");
  ledcWrite(LCD_BL, 180); blCur = 180;

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFiManager wm;
  wm.setConfigPortalTimeout(300);
  wm.setAPCallback([](WiFiManager*) { drawMessage("Wifi instellen", "verbind met Toiletpaneel-Setup"); });
  if (!wm.autoConnect("Toiletpaneel-Setup")) ESP.restart();
  WiFi.setSleep(false);                          // snel reageren op statusberichten

  udp.begin(UDP_PORT);
  configTzTime(TZ_INFO, "pool.ntp.org", "time.google.com");

  ArduinoOTA.setHostname(HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASS);
  ArduinoOTA.onStart([]() { drawMessage("Update", "even geduld..."); });
  ArduinoOTA.begin();

  lastTouch = millis();
  sendCmd(CMD_HELLO);
  uiDirty = true;
}

void loop() {
  ArduinoOTA.handle();
  pollStatus();
  updateTouch();
  updateBacklight();

  int m = -1; struct tm t;
  if (getLocalTime(&t, 0)) m = t.tm_hour * 60 + t.tm_min;
  if (m != lastMinute) { lastMinute = m; uiDirty = true; }
  if (uiDirty) drawUI();
}
