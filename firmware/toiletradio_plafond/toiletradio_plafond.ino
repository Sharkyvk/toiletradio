/*****************************************************************************
  TOILETRADIO – PLAFONDSPELER  v2.1
  De eigenlijke radio: haalt de stream op, stuurt de versterker(s), leest de
  sensoren en bewaakt de stille uren. Het muurpaneel is een draadloze
  afstandsbediening (UDP via je wifi).

  v2.1: beheer via Bluetooth (BLE, met pincode) + wifi-dashboard als reserve.
        Paneelverbinding van ESP-NOW naar UDP (nodig omdat wifi en Bluetooth
        samen de antenne delen; UDP via de router is dan betrouwbaarder).

  BOARD   : ESP32-S3-DevKitC-1 N16R8 (16 MB flash, 8 MB PSRAM)
            (elk ESP32-S3 board met PSRAM werkt; pas dan de pinnen aan)
  CORE    : Arduino-ESP32 3.x

  Arduino IDE -> Tools:
    Board            : "ESP32S3 Dev Module"
    Flash Size       : 16MB
    Partition Scheme : "16M Flash (3MB APP/9.9MB FATFS)"
    PSRAM            : "OPI PSRAM"
    USB CDC On Boot  : "Enabled"

  Libraries:
    - ESP32-audioI2S (schreibfaul1, v3.x) -> GitHub ZIP
    - WiFiManager (tzapu)
    - NimBLE-Arduino (h2zero, v2.x)
    - ArduinoJson (Benoit Blanchon, v7.x)

  BEDRADING
  ---------------------------------------------------------------------------
   GPIO4   -> MAX98357A BCLK   (bij 2 versterkers: parallel op beide)
   GPIO5   -> MAX98357A LRC    (idem)
   GPIO6   -> MAX98357A DIN    (idem)
   GPIO7   -> MAX98357A SD     (idem; HIGH = aan)
   GPIO15  <- LD2410C OUT      (HIGH = iemand aanwezig)
   GPIO16  <- optocoupler-module uitgang (LOW = lamp aan), ingang op de
              schakeldraad van de lamp + nul
   5V/GND  -> IRM-10-5 voeding (vaste fase + nul via zekering)
   Speakers: 8 ohm per versterker

  BEDIENING MUURPANEEL (via UDP, poort 4210)
   - tik zender          -> kiezen/afspelen, tik spelende zender -> stop
   - tik luidspreker     -> mute aan/uit
   - elke aanraking telt als aanwezigheid

  BEHEER
   - Bluetooth: open beheer.html (Android: Chrome, iPhone: Bluefy),
     koppelen met pincode (standaard 123456 -> wijzigen!)
   - Reserve: http://toiletradio.local   (admin / toilet -> wijzigen!)
  Eerste start: wifi instellen via Bluetooth (beheer.html) of via het
  wifi-netwerk "Toiletradio-Setup"
*****************************************************************************/

#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <Preferences.h>
#include <WiFiManager.h>
#include <WiFiUdp.h>
#include <NimBLEDevice.h>
#include <ArduinoJson.h>
#include "Audio.h"
#include <time.h>

// ---------------- Pinnen ----------------
#define PIN_I2S_BCLK 4
#define PIN_I2S_LRC  5
#define PIN_I2S_DOUT 6
#define PIN_AMP_SD   7
#define PIN_RADAR    15
#define PIN_LIGHT    16

const char* HOSTNAME = "toiletradio";
const char* TZ_INFO  = "CET-1CEST,M3.5.0,M10.5.0/3";

// =========================================================================
//  Paneelprotocol via UDP  (MOET identiek zijn in plafond- en paneelfirmware)
// =========================================================================
#define TR_MAGIC 0x54524432UL     // "TRD2"
enum : uint8_t { MSG_PANEL = 1, MSG_STATUS = 2 };
enum : uint8_t { CMD_TOUCH = 1, CMD_SELECT = 2, CMD_STOP = 3, CMD_MUTE = 4, CMD_HELLO = 5 };

struct __attribute__((packed)) PanelMsg {
  uint32_t magic;
  uint8_t  type;      // MSG_PANEL
  uint8_t  cmd;
  uint8_t  arg;
  uint16_t seq;
};

struct __attribute__((packed)) StatusMsg {
  uint32_t magic;
  uint8_t  type;      // MSG_STATUS
  uint8_t  state;     // 0 uit, 1 verbinden, 2 fade-in, 3 speelt, 4 fade-uit
  uint8_t  station;
  uint8_t  muted;
  uint8_t  quiet;
  uint8_t  someone;   // licht aan of aanwezigheid
  uint8_t  bri, briN;
  uint16_t scrT;
  char     names[5][19];
};

#define UDP_PORT 4210
WiFiUDP udp;

// =========================================================================
//  Instellingen
// =========================================================================
Audio audio;
WebServer server(80);
Preferences prefs;

struct Station { String n; String u; };
Station stations[5];

struct Settings {
  uint8_t  station  = 0;
  uint8_t  volume   = 8;          // 0..21
  uint8_t  mode     = 2;          // 0 licht, 1 aanwezigheid, 2 licht + aanwezigheid
  uint16_t offDelay = 60;
  uint16_t fadeMs   = 3000;
  uint16_t qs       = 23 * 60 + 30;
  uint16_t qe       = 6 * 60;
  bool     enabled  = true;
  uint8_t  bri      = 180;        // paneel overdag
  uint8_t  briN     = 25;         // paneel stille uren
  uint16_t scrT     = 30;         // paneel aan na aanraking (s)
  String   pass     = "toilet";     // wifi-dashboard
  uint32_t pin      = 123456;       // Bluetooth-koppelcode
} cfg;

enum State { ST_IDLE, ST_CONNECTING, ST_FADEIN, ST_PLAYING, ST_FADEOUT };
const char* STATE_TXT[] = { "uit", "verbinden", "fade-in", "speelt", "fade-uit" };
State state = ST_IDLE;

bool forcedOn = false, manualOn = false, suppressed = false, muted = false;
float curVol = 0; int lastSetVol = -1;
bool fading = false; float fadeFrom = 0, fadeTo = 0; uint32_t fadeStart = 0;
uint32_t connectStart = 0;
uint32_t lastPresence = 0;
bool statusDirty = true;

void setDefaultStations() {
  stations[0] = { "NPO Radio 2",  "https://icecast.omroep.nl/radio2-bb-mp3" };
  stations[1] = { "Sky Radio",    "https://playerservices.streamtheworld.com/api/livestream-redirect/SKYRADIO.mp3" };
  stations[2] = { "Qmusic",       "https://stream.qmusic.nl/qmusic/mp3" };
  stations[3] = { "100% NL",      "https://stream.100p.nl/100pctnl.mp3" };
  stations[4] = { "NPO Klassiek", "https://icecast.omroep.nl/radio4-bb-mp3" };
}

void loadSettings() {
  setDefaultStations();
  prefs.begin("radio", true);
  cfg.station  = prefs.getUChar("st", cfg.station);
  cfg.volume   = prefs.getUChar("vol", cfg.volume);
  cfg.mode     = prefs.getUChar("mode", cfg.mode);
  cfg.offDelay = prefs.getUShort("off", cfg.offDelay);
  cfg.fadeMs   = prefs.getUShort("fade", cfg.fadeMs);
  cfg.qs       = prefs.getUShort("qs", cfg.qs);
  cfg.qe       = prefs.getUShort("qe", cfg.qe);
  cfg.enabled  = prefs.getBool("en", cfg.enabled);
  cfg.bri      = prefs.getUChar("bri", cfg.bri);
  cfg.briN     = prefs.getUChar("briN", cfg.briN);
  cfg.scrT     = prefs.getUShort("scrT", cfg.scrT);
  cfg.pass     = prefs.getString("pw", cfg.pass);
  cfg.pin      = prefs.getULong("pin", cfg.pin);
  for (int i = 0; i < 5; i++) {
    stations[i].n = prefs.getString(("n" + String(i)).c_str(), stations[i].n);
    stations[i].u = prefs.getString(("u" + String(i)).c_str(), stations[i].u);
  }
  prefs.end();
  if (cfg.station > 4) cfg.station = 0;
}

void saveSettings() {
  prefs.begin("radio", false);
  prefs.putUChar("st", cfg.station);
  prefs.putUChar("vol", cfg.volume);
  prefs.putUChar("mode", cfg.mode);
  prefs.putUShort("off", cfg.offDelay);
  prefs.putUShort("fade", cfg.fadeMs);
  prefs.putUShort("qs", cfg.qs);
  prefs.putUShort("qe", cfg.qe);
  prefs.putBool("en", cfg.enabled);
  prefs.putUChar("bri", cfg.bri);
  prefs.putUChar("briN", cfg.briN);
  prefs.putUShort("scrT", cfg.scrT);
  prefs.putString("pw", cfg.pass);
  prefs.putULong("pin", cfg.pin);
  for (int i = 0; i < 5; i++) {
    prefs.putString(("n" + String(i)).c_str(), stations[i].n);
    prefs.putString(("u" + String(i)).c_str(), stations[i].u);
  }
  prefs.end();
}

// =========================================================================
//  Tijd en sensoren
// =========================================================================
bool timeValid() { return time(nullptr) > 1700000000; }

int minutesNow() {
  struct tm t;
  if (!getLocalTime(&t, 0)) return -1;
  return t.tm_hour * 60 + t.tm_min;
}

bool inQuiet() {
  int m = minutesNow();
  if (m < 0 || cfg.qs == cfg.qe) return false;
  if (cfg.qs > cfg.qe) return (m >= cfg.qs || m < cfg.qe);
  return (m >= cfg.qs && m < cfg.qe);
}

volatile uint32_t lastLightPulse = 0;
void IRAM_ATTR onLightPulse() { lastLightPulse = millis(); }
bool lightOn() { return digitalRead(PIN_LIGHT) == LOW || (millis() - lastLightPulse < 150); }

bool presenceRecent() { return lastPresence && (millis() - lastPresence < cfg.offDelay * 1000UL); }

bool sensorWant() {
  switch (cfg.mode) {
    case 0:  return lightOn();
    case 1:  return presenceRecent();
    default: return lightOn() && presenceRecent();
  }
}

// =========================================================================
//  Audio
// =========================================================================
bool isActive() { return state == ST_CONNECTING || state == ST_FADEIN || state == ST_PLAYING; }

void setVol(float v) {
  curVol = v;
  int iv = muted ? 0 : (int)roundf(v);
  if (iv != lastSetVol) { audio.setVolume(iv); lastSetVol = iv; }
}

void startFade(float to) { fadeFrom = curVol; fadeTo = to; fadeStart = millis(); fading = true; }

void connectStream() {
  setVol(0);
  fading = false;
  connectStart = millis();
  state = ST_CONNECTING;
  statusDirty = true;
  audio.connecttohost(stations[cfg.station].u.c_str());
}

void goIdle() {
  audio.stopSong();
  digitalWrite(PIN_AMP_SD, LOW);
  state = ST_IDLE;
  muted = false;
  statusDirty = true;
}

void startPlay() {
  if (state == ST_FADEOUT) { state = ST_FADEIN; startFade(cfg.volume); statusDirty = true; return; }
  digitalWrite(PIN_AMP_SD, HIGH);
  connectStream();
}

void stopPlay() {
  if (state == ST_CONNECTING) goIdle();
  else { state = ST_FADEOUT; startFade(0); statusDirty = true; }
}

void toggleMute() {
  if (!isActive() && !muted) return;
  muted = !muted;
  lastSetVol = -1;
  setVol(curVol);
  statusDirty = true;
}

void updateAudio() {
  if (state == ST_CONNECTING) {
    if (audio.isRunning()) { state = ST_FADEIN; startFade(cfg.volume); statusDirty = true; }
    else if (millis() - connectStart > 8000) connectStream();
  } else if (state == ST_PLAYING && !audio.isRunning()) {
    connectStream();
  }
  if (fading) {
    uint32_t d = millis() - fadeStart;
    float k = cfg.fadeMs ? min(1.0f, d / (float)cfg.fadeMs) : 1.0f;
    setVol(fadeFrom + (fadeTo - fadeFrom) * k);
    if (k >= 1.0f) {
      fading = false;
      if (state == ST_FADEIN) { state = ST_PLAYING; statusDirty = true; }
      else if (state == ST_FADEOUT) goIdle();
    }
  }
}

// =========================================================================
//  Beslislogica
// =========================================================================
bool lastSomeone = false;

void updateLogic() {
  if (digitalRead(PIN_RADAR) == HIGH) lastPresence = millis();

  bool someone = lightOn() || presenceRecent();
  if (someone != lastSomeone) { lastSomeone = someone; statusDirty = true; }
  if (!someone) { suppressed = false; manualOn = false; }

  bool active = isActive();
  bool want;
  if (forcedOn)                        want = true;
  else if (suppressed || !cfg.enabled) want = false;
  else if (manualOn)                   want = true;
  else if (active)                     want = sensorWant();
  else                                 want = sensorWant() && timeValid() && !inQuiet();

  if (want && !active) startPlay();
  else if (!want && active) stopPlay();
}

// =========================================================================
//  Muurpaneel via UDP
// =========================================================================
uint32_t lastPanelSeen = 0;
IPAddress panelIP;

void applyPanelMsg(const PanelMsg& m) {
  static uint16_t lastSeq = 0xFFFF;
  if (m.cmd == CMD_HELLO) { statusDirty = true; return; }   // paneel leeft / vraagt status
  lastPresence = millis();                                 // elke aanraking = aanwezig
  if (m.cmd == CMD_TOUCH) return;
  if (m.seq == lastSeq) return;                            // dubbel verzonden commando
  lastSeq = m.seq;

  if (m.cmd == CMD_SELECT && m.arg < 5) {
    if (m.arg == cfg.station && isActive()) {              // spelende zender = stop
      forcedOn = false; manualOn = false; suppressed = true;
    } else {
      bool change = (m.arg != cfg.station);
      cfg.station = m.arg;
      suppressed = false; manualOn = true;
      if (change) { prefs.begin("radio", false); prefs.putUChar("st", cfg.station); prefs.end(); }
      if (change && isActive()) connectStream();
    }
  } else if (m.cmd == CMD_STOP) {
    forcedOn = false; manualOn = false; suppressed = true;
  } else if (m.cmd == CMD_MUTE) {
    toggleMute();
  }
  statusDirty = true;
}

void handlePanel() {
  int len;
  while ((len = udp.parsePacket()) > 0) {
    PanelMsg m;
    if (len != sizeof(m)) { udp.flush(); continue; }
    udp.read((uint8_t*)&m, sizeof(m));
    if (m.magic != TR_MAGIC || m.type != MSG_PANEL) continue;
    panelIP = udp.remoteIP();
    lastPanelSeen = millis();
    applyPanelMsg(m);
  }
}

void sendStatus() {
  static uint32_t last = 0;
  if (WiFi.status() != WL_CONNECTED) return;
  if (!statusDirty && millis() - last < 1000) return;
  last = millis(); statusDirty = false;

  StatusMsg s = {};
  s.magic = TR_MAGIC; s.type = MSG_STATUS;
  s.state = state; s.station = cfg.station; s.muted = muted;
  s.quiet = timeValid() && inQuiet();
  s.someone = lastSomeone;
  s.bri = cfg.bri; s.briN = cfg.briN; s.scrT = cfg.scrT;
  for (int i = 0; i < 5; i++) strlcpy(s.names[i], stations[i].n.c_str(), sizeof(s.names[i]));

  udp.beginPacket(WiFi.broadcastIP(), UDP_PORT);           // paneel zoekt de speler zo
  udp.write((uint8_t*)&s, sizeof(s));
  udp.endPacket();
  if (panelIP[0] != 0) {                                   // en rechtstreeks, als hij bekend is
    udp.beginPacket(panelIP, UDP_PORT);
    udp.write((uint8_t*)&s, sizeof(s));
    udp.endPacket();
  }
}

// =========================================================================
//  Gedeelde helpers (dashboard en Bluetooth)
// =========================================================================
String js(const String& s) {
  String o;
  for (char c : s) {
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if ((uint8_t)c >= 0x20) o += c;
  }
  return o;
}
String jb(bool b) { return b ? "true" : "false"; }

String timeStr() {
  char t[8] = "--:--";
  struct tm tm;
  if (getLocalTime(&tm, 0)) strftime(t, sizeof(t), "%H:%M", &tm);
  return String(t);
}

long panelAge() { return lastPanelSeen ? (long)((millis() - lastPanelSeen) / 1000) : -1; }

void afterChange(uint8_t oldStation, const String& oldUrl) {
  if (isActive() && (cfg.station != oldStation || stations[cfg.station].u != oldUrl)) connectStream();
  else if (state == ST_PLAYING) setVol(cfg.volume);
  statusDirty = true;
}

void cmdPlay() { forcedOn = true; suppressed = false; statusDirty = true; }
void cmdStop() { forcedOn = false; manualOn = false; suppressed = true; statusDirty = true; }

// =========================================================================
//  Bluetooth-beheer (BLE) – alleen beheerder, koppelen met pincode
// =========================================================================
#define BLE_SVC    "7e1a0001-5c3b-4b8e-9a51-2f6d0c7a9b01"
#define BLE_STATUS "7e1a0002-5c3b-4b8e-9a51-2f6d0c7a9b01"
#define BLE_CONFIG "7e1a0003-5c3b-4b8e-9a51-2f6d0c7a9b01"
#define BLE_CMD    "7e1a0004-5c3b-4b8e-9a51-2f6d0c7a9b01"
#define BLE_WIFI   "7e1a0005-5c3b-4b8e-9a51-2f6d0c7a9b01"
const char* BLE_ST[5] = {
  "7e1a0006-5c3b-4b8e-9a51-2f6d0c7a9b01", "7e1a0007-5c3b-4b8e-9a51-2f6d0c7a9b01",
  "7e1a0008-5c3b-4b8e-9a51-2f6d0c7a9b01", "7e1a0009-5c3b-4b8e-9a51-2f6d0c7a9b01",
  "7e1a000a-5c3b-4b8e-9a51-2f6d0c7a9b01" };

enum : uint8_t { JOB_CONFIG = 0, JOB_CMD = 1, JOB_WIFI = 2, JOB_ST0 = 10 };
struct BleJob { uint8_t kind; char data[513]; };
QueueHandle_t bleQueue;

NimBLEServer* bleServer = nullptr;
NimBLECharacteristic *chStatus, *chConfig, *chCmd, *chWifi, *chSt[5];

// Schrijfacties komen binnen in de Bluetooth-taak -> via een wachtrij naar loop()
class WriteCB : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& info) override {
    BleJob j = {};
    if (c == chConfig)    j.kind = JOB_CONFIG;
    else if (c == chCmd)  j.kind = JOB_CMD;
    else if (c == chWifi) j.kind = JOB_WIFI;
    else {
      int idx = -1;
      for (int i = 0; i < 5; i++) if (c == chSt[i]) idx = i;
      if (idx < 0) return;
      j.kind = JOB_ST0 + idx;
    }
    std::string v = c->getValue();
    strlcpy(j.data, v.c_str(), sizeof(j.data));
    xQueueSend(bleQueue, &j, 0);
  }
} writeCB;

String statusJson() {
  String j = "{";
  j += "\"s\":\"" + String(STATE_TXT[state]) + "\"";
  j += ",\"st\":" + String(cfg.station);
  j += ",\"m\":" + jb(muted);
  j += ",\"l\":" + jb(lightOn());
  j += ",\"p\":" + jb(digitalRead(PIN_RADAR) == HIGH);
  j += ",\"q\":" + jb(inQuiet());
  j += ",\"t\":\"" + timeStr() + "\"";
  j += ",\"w\":" + jb(WiFi.status() == WL_CONNECTED);
  j += ",\"ssid\":\"" + js(WiFi.SSID()) + "\"";
  j += ",\"ip\":\"" + WiFi.localIP().toString() + "\"";
  j += ",\"r\":" + String((int)WiFi.RSSI());
  j += ",\"pa\":" + String(panelAge());
  j += "}";
  return j;
}

String configJson() {
  String j = "{";
  j += "\"station\":" + String(cfg.station);
  j += ",\"volume\":" + String(cfg.volume);
  j += ",\"mode\":" + String(cfg.mode);
  j += ",\"offDelay\":" + String(cfg.offDelay);
  j += ",\"fadeMs\":" + String(cfg.fadeMs);
  j += ",\"qs\":" + String(cfg.qs) + ",\"qe\":" + String(cfg.qe);
  j += ",\"enabled\":" + jb(cfg.enabled);
  j += ",\"bri\":" + String(cfg.bri) + ",\"briN\":" + String(cfg.briN) + ",\"scrT\":" + String(cfg.scrT);
  j += "}";
  return j;
}

String stationJson(int i) {
  return "{\"n\":\"" + js(stations[i].n) + "\",\"u\":\"" + js(stations[i].u) + "\"}";
}

void bleSet(NimBLECharacteristic* c, const String& s) { c->setValue((const uint8_t*)s.c_str(), s.length()); }

void bleRefreshAll() {
  if (!bleServer) return;
  bleSet(chStatus, statusJson());
  bleSet(chConfig, configJson());
  for (int i = 0; i < 5; i++) bleSet(chSt[i], stationJson(i));
}

void bleInit() {
  bleQueue = xQueueCreate(4, sizeof(BleJob));
  NimBLEDevice::init("Toiletradio");
  NimBLEDevice::setSecurityAuth(true, true, true);         // koppelen, MITM, secure connections
  NimBLEDevice::setSecurityPasskey(cfg.pin);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);   // telefoon vraagt om de pincode
  NimBLEDevice::setMTU(517);

  const uint32_t R = NIMBLE_PROPERTY::READ  | NIMBLE_PROPERTY::READ_ENC  | NIMBLE_PROPERTY::READ_AUTHEN;
  const uint32_t W = NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC | NIMBLE_PROPERTY::WRITE_AUTHEN;

  bleServer = NimBLEDevice::createServer();
  NimBLEService* svc = bleServer->createService(BLE_SVC);
  chStatus = svc->createCharacteristic(BLE_STATUS, R | NIMBLE_PROPERTY::NOTIFY, 512);
  chConfig = svc->createCharacteristic(BLE_CONFIG, R | W, 512);
  chCmd    = svc->createCharacteristic(BLE_CMD, W, 128);
  chWifi   = svc->createCharacteristic(BLE_WIFI, W, 256);
  for (int i = 0; i < 5; i++) chSt[i] = svc->createCharacteristic(BLE_ST[i], R | W, 512);

  chConfig->setCallbacks(&writeCB);
  chCmd->setCallbacks(&writeCB);
  chWifi->setCallbacks(&writeCB);
  for (int i = 0; i < 5; i++) chSt[i]->setCallbacks(&writeCB);
  svc->start();

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  NimBLEAdvertisementData ad;
  ad.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
  ad.setCompleteServices(NimBLEUUID(BLE_SVC));
  adv->setAdvertisementData(ad);
  NimBLEAdvertisementData sr;
  sr.setName("Toiletradio");
  adv->setScanResponseData(sr);
  adv->start();

  bleRefreshAll();
}

void applyConfigJson(const char* txt) {
  JsonDocument d;
  if (deserializeJson(d, txt)) return;
  uint8_t oldStation = cfg.station;
  String oldUrl = stations[cfg.station].u;
  if (d["station"].is<int>())  cfg.station  = constrain(d["station"].as<int>(), 0, 4);
  if (d["volume"].is<int>())   cfg.volume   = constrain(d["volume"].as<int>(), 0, 21);
  if (d["mode"].is<int>())     cfg.mode     = constrain(d["mode"].as<int>(), 0, 2);
  if (d["offDelay"].is<int>()) cfg.offDelay = constrain(d["offDelay"].as<int>(), 5, 3600);
  if (d["fadeMs"].is<int>())   cfg.fadeMs   = constrain(d["fadeMs"].as<int>(), 0, 15000);
  if (d["qs"].is<int>())       cfg.qs       = constrain(d["qs"].as<int>(), 0, 1439);
  if (d["qe"].is<int>())       cfg.qe       = constrain(d["qe"].as<int>(), 0, 1439);
  if (d["enabled"].is<bool>()) cfg.enabled  = d["enabled"].as<bool>();
  if (d["bri"].is<int>())      cfg.bri      = constrain(d["bri"].as<int>(), 5, 255);
  if (d["briN"].is<int>())     cfg.briN     = constrain(d["briN"].as<int>(), 0, 255);
  if (d["scrT"].is<int>())     cfg.scrT     = constrain(d["scrT"].as<int>(), 5, 600);
  saveSettings();
  afterChange(oldStation, oldUrl);
}

void applyStationJson(int i, const char* txt) {
  JsonDocument d;
  if (deserializeJson(d, txt)) return;
  uint8_t oldStation = cfg.station;
  String oldUrl = stations[cfg.station].u;
  if (d["n"].is<const char*>()) stations[i].n = String(d["n"].as<const char*>()).substring(0, 18);
  if (d["u"].is<const char*>()) stations[i].u = d["u"].as<const char*>();
  saveSettings();
  afterChange(oldStation, oldUrl);
}

void applyCommand(String c) {
  c.trim();
  if (c == "play") cmdPlay();
  else if (c == "stop") cmdStop();
  else if (c == "mute") toggleMute();
  else if (c == "unpair") NimBLEDevice::deleteAllBonds();
  else if (c == "restart") { delay(200); ESP.restart(); }
  else if (c.startsWith("pin:")) {
    long p = c.substring(4).toInt();
    if (p >= 100000 && p <= 999999) {
      cfg.pin = p; saveSettings();
      NimBLEDevice::setSecurityPasskey(cfg.pin);
      NimBLEDevice::deleteAllBonds();                  // opnieuw koppelen met nieuwe code
    }
  } else if (c.startsWith("webpass:")) {
    String p = c.substring(8);
    if (p.length() >= 4) { cfg.pass = p; saveSettings(); }
  }
}

void applyWifiJson(const char* txt) {
  JsonDocument d;
  if (deserializeJson(d, txt)) return;
  String ssid = d["ssid"] | "";
  String pw   = d["pass"] | "";
  if (!ssid.length()) return;
  WiFi.disconnect();
  WiFi.begin(ssid.c_str(), pw.c_str());                   // wordt bewaard voor volgende start
}

void handleBle() {
  BleJob j;
  bool any = false;
  while (xQueueReceive(bleQueue, &j, 0) == pdTRUE) {
    any = true;
    if (j.kind == JOB_CMD)          applyCommand(String(j.data));
    else if (j.kind == JOB_WIFI)    applyWifiJson(j.data);
    else if (j.kind == JOB_CONFIG)  applyConfigJson(j.data);
    else if (j.kind >= JOB_ST0 && j.kind < JOB_ST0 + 5) applyStationJson(j.kind - JOB_ST0, j.data);
  }
  if (any) bleRefreshAll();

  static uint32_t last = 0;                               // status elke seconde verversen
  if (millis() - last > 1000) {
    last = millis();
    bleSet(chStatus, statusJson());
    if (bleServer->getConnectedCount() > 0) chStatus->notify();
  }
}

// =========================================================================
//  Dashboard
// =========================================================================
const char INDEX_HTML[] PROGMEM = R"HTML(<!doctype html><html lang=nl><head><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1"><title>Toiletradio</title>
<style>
body{font-family:system-ui,sans-serif;background:#111317;color:#e8e6e1;margin:0 auto;padding:16px;max-width:520px}
h1{font-weight:300;letter-spacing:.25em;color:#c8a96a;font-size:1.4em}
h3{font-weight:400;color:#c8a96a;margin:0 0 6px}
.card{background:#1e2127;border-radius:14px;padding:14px;margin:12px 0}
label{display:block;margin:10px 0 4px;color:#a9a59c;font-size:.88em}
input,select{width:100%;box-sizing:border-box;padding:9px;border-radius:8px;border:1px solid #363a42;background:#15171b;color:#e8e6e1;font-size:1em}
input[type=checkbox]{width:auto}
button{padding:10px 16px;border:0;border-radius:8px;background:#c8a96a;color:#111317;font-weight:600;font-size:1em;margin:4px 6px 0 0}
button.g{background:#363a42;color:#e8e6e1}
.row{display:flex;gap:10px}.row>div{flex:1}
#st{line-height:1.7}
</style></head><body>
<h1>TOILETRADIO</h1>
<div class=card id=st>Laden…</div>
<div class=card>
<button onclick="cmd('play')">▶ Nu afspelen</button><button class=g onclick="cmd('stop')">■ Stop</button><button class=g id=mb onclick="cmd('mute')">🔇 Mute</button>
<label>Zender</label><select id=station onchange=save()></select>
<label>Volume <span id=vv></span></label>
<input type=range id=volume min=0 max=21 oninput="vv.textContent=this.value" onchange=save()>
</div>
<div class=card><h3>Automatisch</h3>
<label><input type=checkbox id=enabled> Automatisch aanzetten actief</label>
<label>Aan op</label><select id=mode>
<option value=0>Licht aan</option><option value=1>Aanwezigheid (radar)</option>
<option value=2>Licht aan + aanwezigheid</option></select>
<label>Uit na zoveel seconden zonder aanwezigheid</label><input type=number id=offDelay min=5 max=3600>
<label>Fade-in / fade-uit (ms)</label><input type=number id=fadeMs min=0 max=15000 step=250>
<div class=row><div><label>Stil vanaf</label><input type=time id=qs></div>
<div><label>Stil tot</label><input type=time id=qe></div></div>
</div>
<div class=card><h3>Muurpaneel</h3>
<div class=row><div><label>Helderheid dag</label><input type=number id=bri min=5 max=255></div>
<div><label>Helderheid nacht</label><input type=number id=briN min=0 max=255></div></div>
<label>Scherm aan na aanraking (s)</label><input type=number id=scrT min=5 max=600>
</div>
<div class=card id=stations></div>
<div class=card><label>Nieuw wachtwoord (leeg = ongewijzigd)</label><input type=password id=pass>
<button onclick=save()>Opslaan</button></div>
<script>
const $=id=>document.getElementById(id);
const hm=m=>String(Math.floor(m/60)).padStart(2,'0')+':'+String(m%60).padStart(2,'0');
const mins=v=>{const p=v.split(':');return (+p[0])*60+(+p[1])};
const esc=s=>String(s).replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));
let S=null;
async function load(full){
 try{S=await (await fetch('/api/state')).json()}catch(e){$('st').textContent='Geen verbinding';return}
 $('mb').textContent=S.muted?'🔊 Geluid aan':'🔇 Mute';
 $('st').innerHTML=`<b>${S.stateText}${S.muted?' (gedempt)':''}</b> · ${esc(S.stations[S.station].n)}<br>Licht: ${S.light?'aan':'uit'} · Aanwezig: ${S.presence?'ja':'nee'}<br>Tijd: ${S.time}${S.quiet?' · stille uren':''} · wifi ${S.rssi} dBm<br>Muurpaneel: ${S.panelAge<0?'nog niet gezien':S.panelAge<10?'verbonden':'laatst '+S.panelAge+' s geleden'}`;
 if(!full)return;
 $('station').innerHTML=S.stations.map((s,i)=>`<option value=${i}>${esc(s.n)}</option>`).join('');
 $('station').value=S.station;$('volume').value=S.volume;$('vv').textContent=S.volume;
 $('mode').value=S.mode;$('offDelay').value=S.offDelay;$('fadeMs').value=S.fadeMs;
 $('qs').value=hm(S.qs);$('qe').value=hm(S.qe);$('enabled').checked=S.enabled;
 $('bri').value=S.bri;$('briN').value=S.briN;$('scrT').value=S.scrT;
 $('stations').innerHTML='<h3>Zenders</h3>'+S.stations.map((s,i)=>`<label>Zender ${i+1}</label><input id=n${i} maxlength=18 value="${esc(s.n)}"><input id=u${i} value="${esc(s.u)}" placeholder="https://stream-url" style="margin-top:6px">`).join('');
}
async function save(){
 const f=new URLSearchParams();
 ['station','volume','mode','offDelay','fadeMs','bri','briN','scrT'].forEach(k=>f.append(k,$(k).value));
 f.append('qs',mins($('qs').value));f.append('qe',mins($('qe').value));
 f.append('enabled',$('enabled').checked?1:0);
 for(let i=0;i<5;i++){f.append('n'+i,$('n'+i).value);f.append('u'+i,$('u'+i).value)}
 if($('pass').value)f.append('pass',$('pass').value);
 await fetch('/api/set',{method:'POST',body:f});$('pass').value='';load(true);
}
async function cmd(c){await fetch('/api/'+c,{method:'POST'});load(false)}
load(true);setInterval(()=>load(false),3000);
</script></body></html>)HTML";

bool auth() {
  if (server.authenticate("admin", cfg.pass.c_str())) return true;
  server.requestAuthentication();
  return false;
}

void handleState() {
  if (!auth()) return;
  String j = "{";
  j += "\"stateText\":\"" + String(STATE_TXT[state]) + "\",";
  j += "\"station\":" + String(cfg.station) + ",";
  j += "\"volume\":" + String(cfg.volume) + ",";
  j += "\"muted\":" + String(muted ? "true" : "false") + ",";
  j += "\"mode\":" + String(cfg.mode) + ",";
  j += "\"offDelay\":" + String(cfg.offDelay) + ",";
  j += "\"fadeMs\":" + String(cfg.fadeMs) + ",";
  j += "\"qs\":" + String(cfg.qs) + ",\"qe\":" + String(cfg.qe) + ",";
  j += "\"enabled\":" + String(cfg.enabled ? "true" : "false") + ",";
  j += "\"bri\":" + String(cfg.bri) + ",\"briN\":" + String(cfg.briN) + ",\"scrT\":" + String(cfg.scrT) + ",";
  j += "\"light\":" + String(lightOn() ? "true" : "false") + ",";
  j += "\"presence\":" + String(digitalRead(PIN_RADAR) == HIGH ? "true" : "false") + ",";
  j += "\"quiet\":" + String(inQuiet() ? "true" : "false") + ",";
  j += "\"time\":\"" + timeStr() + "\",";
  j += "\"rssi\":" + String(WiFi.RSSI()) + ",";
  j += "\"panelAge\":" + String(panelAge()) + ",";
  j += "\"stations\":[";
  for (int i = 0; i < 5; i++) {
    j += "{\"n\":\"" + js(stations[i].n) + "\",\"u\":\"" + js(stations[i].u) + "\"}";
    if (i < 4) j += ",";
  }
  j += "]}";
  server.send(200, "application/json", j);
}

void handleSet() {
  if (!auth()) return;
  uint8_t oldStation = cfg.station;
  String oldUrl = stations[cfg.station].u;

  if (server.hasArg("station"))  cfg.station  = constrain(server.arg("station").toInt(), 0, 4);
  if (server.hasArg("volume"))   cfg.volume   = constrain(server.arg("volume").toInt(), 0, 21);
  if (server.hasArg("mode"))     cfg.mode     = constrain(server.arg("mode").toInt(), 0, 2);
  if (server.hasArg("offDelay")) cfg.offDelay = constrain(server.arg("offDelay").toInt(), 5, 3600);
  if (server.hasArg("fadeMs"))   cfg.fadeMs   = constrain(server.arg("fadeMs").toInt(), 0, 15000);
  if (server.hasArg("qs"))       cfg.qs       = constrain(server.arg("qs").toInt(), 0, 1439);
  if (server.hasArg("qe"))       cfg.qe       = constrain(server.arg("qe").toInt(), 0, 1439);
  if (server.hasArg("enabled"))  cfg.enabled  = server.arg("enabled") == "1";
  if (server.hasArg("bri"))      cfg.bri      = constrain(server.arg("bri").toInt(), 5, 255);
  if (server.hasArg("briN"))     cfg.briN     = constrain(server.arg("briN").toInt(), 0, 255);
  if (server.hasArg("scrT"))     cfg.scrT     = constrain(server.arg("scrT").toInt(), 5, 600);
  if (server.hasArg("pass") && server.arg("pass").length() >= 4) cfg.pass = server.arg("pass");
  for (int i = 0; i < 5; i++) {
    String kn = "n" + String(i), ku = "u" + String(i);
    if (server.hasArg(kn)) stations[i].n = server.arg(kn).substring(0, 18);
    if (server.hasArg(ku)) stations[i].u = server.arg(ku);
  }
  saveSettings();
  afterChange(oldStation, oldUrl);
  bleRefreshAll();
  server.send(200, "text/plain", "ok");
}

void setupWeb() {
  server.on("/", HTTP_GET, []() { if (auth()) server.send_P(200, "text/html", INDEX_HTML); });
  server.on("/api/state", HTTP_GET, handleState);
  server.on("/api/set", HTTP_POST, handleSet);
  server.on("/api/play", HTTP_POST, []() {
    if (!auth()) return;
    cmdPlay();
    server.send(200, "text/plain", "ok");
  });
  server.on("/api/stop", HTTP_POST, []() {
    if (!auth()) return;
    cmdStop();
    server.send(200, "text/plain", "ok");
  });
  server.on("/api/mute", HTTP_POST, []() {
    if (!auth()) return;
    toggleMute();
    server.send(200, "text/plain", "ok");
  });
  server.begin();
}

// =========================================================================
//  Setup / loop
// =========================================================================
WiFiManager wm;
bool netStarted = false;

void setup() {
  Serial.begin(115200);
  pinMode(PIN_AMP_SD, OUTPUT); digitalWrite(PIN_AMP_SD, LOW);
  pinMode(PIN_RADAR, INPUT_PULLDOWN);
  pinMode(PIN_LIGHT, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_LIGHT), onLightPulse, FALLING);

  loadSettings();

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  // LET OP: geen WiFi.setSleep(false) - met Bluetooth aan moet modem-sleep aan blijven

  bleInit();                                // Bluetooth-beheer werkt ook zonder wifi

  wm.setConfigPortalBlocking(false);        // reserve: wifi-portal "Toiletradio-Setup"
  wm.setConfigPortalTimeout(0);
  wm.autoConnect("Toiletradio-Setup");

  audio.setPinout(PIN_I2S_BCLK, PIN_I2S_LRC, PIN_I2S_DOUT);
  audio.forceMono(true);   // SD direct op 3,3 V = linkerkanaal -> mono mixen
  audio.setVolume(0);

  setupWeb();
}

void startNetwork() {                       // eenmalig zodra wifi verbonden is
  netStarted = true;
  if (wm.getConfigPortalActive()) wm.stopConfigPortal();
  udp.begin(UDP_PORT);
  configTzTime(TZ_INFO, "pool.ntp.org", "time.google.com");
  MDNS.begin(HOSTNAME);
  ArduinoOTA.setHostname(HOSTNAME);
  ArduinoOTA.setPassword(cfg.pass.c_str());
  ArduinoOTA.onStart([]() { audio.stopSong(); digitalWrite(PIN_AMP_SD, LOW); });
  ArduinoOTA.begin();
}

void loop() {
  audio.loop();
  wm.process();
  if (!netStarted && WiFi.status() == WL_CONNECTED) startNetwork();
  if (netStarted) { server.handleClient(); ArduinoOTA.handle(); handlePanel(); }

  handleBle();
  updateLogic();
  updateAudio();
  sendStatus();
}
