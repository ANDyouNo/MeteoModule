/*
 * MeteoModule — ESP32-C3 Weather Station + Apple HomeKit
 * v2.2 — ENS160 toggle + NTP-scheduled display on/off
 *
 * Hardware:
 *   - ESP32-C3
 *   - SSD1306 128x64 OLED (I2C, addr 0x3C)
 *   - BMHM ENS160 + AHT21 combo sensor (I2C)
 *       ENS160 addr 0x53 (ADD→GND), AHT21 addr 0x38
 *       CS pin → HIGH or float for I2C mode
 *   - Touch button on GPIO 3 (HIGH when pressed)
 *
 * Wiring (I2C bus shared):
 *   SDA → GPIO 4,  SCL → GPIO 5
 *
 * Controls:
 *   Single press  — cycle screens (Pairing → Main ↔ Air)
 *   Triple press  — toggle display off / on
 *
 * The display also turns on/off automatically on a network-time
 * schedule (SCREEN_SCHEDULE_* below). The button overrides the
 * schedule until the next scheduled switch point.
 *
 * Build options (config block below the includes):
 *   ENABLE_ENS160          — false = AHT21-only build
 *   SCREEN_SCHEDULE_ENABLE — false = no time-based on/off
 *
 * HomeKit services:
 *   TemperatureSensor, HumiditySensor,
 *   AirQualitySensor (VOCDensity), CarbonDioxideSensor (eCO2)
 *     — Air Quality + CO2 only built when ENABLE_ENS160 = true
 *
 * WiFi: configured via Serial (HomeSpan CLI: type 'W')
 * Pairing code: 466-37-726
 *
 * Libraries (Arduino Library Manager):
 *   - HomeSpan  (by Gregg Berman)
 *   - U8g2     (by olikraus)
 */

#include "HomeSpan.h"
#include <U8g2lib.h>
#include <Wire.h>
#include <time.h>

// ===================== Calibration Offsets ====================
#define TEMP_OFFSET       -0.75f    // compensate ENS160 board heat
#define HUMIDITY_OFFSET    0.0f    // e.g. +5.0 to add 5%

// ===================== Feature Toggles =======================
// Build an AHT21-only firmware by setting this to false. When
// disabled, the ENS160 is never initialised, the Air screen is
// hidden, and the HomeKit Air Quality / CO2 accessories are not
// created. (Mirrors ENABLE_AGS10 in the AGS10 firmware.)
#define ENABLE_ENS160        true

// ===================== Display Schedule ======================
// Automatic display on/off driven by a network clock (NTP).
// The button still works at any time and acts as a manual
// override until the next scheduled switch point.
//
// NTP_SERVER1 can be your router's LAN IP — many routers act as
// an NTP server (e.g. "192.168.1.1") — or a public internet pool.
#define SCREEN_SCHEDULE_ENABLE   true
#define SCREEN_ON_HOUR           7      // turn display ON  at 07:00
#define SCREEN_ON_MINUTE         0
#define SCREEN_OFF_HOUR          23     // turn display OFF at 23:00
#define SCREEN_OFF_MINUTE        0

#define NTP_SERVER1              "pool.ntp.org"
#define NTP_SERVER2              "time.nist.gov"
#define GMT_OFFSET_SEC           0      // UTC offset, seconds (UTC+3 = 10800)
#define DAYLIGHT_OFFSET_SEC      0      // extra DST offset, seconds
#define SCHEDULE_CHECK_INTERVAL  15000  // re-evaluate schedule every 15 s

// ===================== Pin Configuration =====================
#define PIN_SDA     4
#define PIN_SCL     5
#define PIN_BUTTON  3

// ===================== I2C Addresses =========================
#define AHT21_ADDR   0x38
#define ENS160_ADDR  0x53

// ===================== Timing (ms) ===========================
#define SENSOR_INTERVAL     2000
#define DISPLAY_INTERVAL    500
#define HOMEKIT_INTERVAL    10000
#define DEBOUNCE_MS         50
#define CLICK_GAP_MS        400
#define ENS_LOG_INTERVAL    60000   // diagnostic log every 60s

// ===================== Screen States =========================
enum Screen { SCR_PAIRING, SCR_MAIN, SCR_AIR };

// ===================== Widget Layout =========================
struct Widget {
  int x, y, w, h;
};

static const Widget WIDGET_L = { 3,  3, 58, 58 };
static const Widget WIDGET_R = { 67, 3, 58, 58 };

// ===================== OLED ==================================
U8G2_SSD1306_128X64_NONAME_F_HW_I2C display(U8G2_R0, U8X8_PIN_NONE);

// ===================== State =================================
Screen   curScreen      = SCR_PAIRING;
bool     displayOn      = true;
bool     wifiConnected  = false;

// Sensor data
float    temperature    = 0.0f;
float    humidity       = 0.0f;
uint16_t tvoc           = 0;
uint16_t eco2           = 0;
uint8_t  aqiIndex       = 0;
bool     ahtOk          = false;
bool     ensOk          = false;
bool     ensWarming     = true;    // true until first valid data from ENS160
bool     ensEverReady   = false;   // logged once when sensor produces first data

// Timing
uint32_t lastSensorRead    = 0;
uint32_t lastDisplayDraw   = 0;
uint32_t lastEnsLog        = 0;
uint32_t lastScheduleCheck = 0;
bool     needRedraw        = true;

// Display schedule
bool     timeSyncStarted   = false;  // true once NTP configTime() issued
int8_t   lastSchedState    = -1;     // -1 = unknown, 0 = off, 1 = on

// Button
bool     btnPrev          = false;
uint32_t btnDownTime      = 0;
uint8_t  clickCount       = 0;
uint32_t lastClickTime    = 0;

// HomeKit characteristic pointers
SpanCharacteristic *hkTemp       = nullptr;
SpanCharacteristic *hkHumid      = nullptr;
SpanCharacteristic *hkAQ         = nullptr;
SpanCharacteristic *hkVOC        = nullptr;
SpanCharacteristic *hkCO2Det     = nullptr;
SpanCharacteristic *hkCO2Level   = nullptr;

// =============================================================
//  Helper: format uptime as "HHh MMm SSs"
// =============================================================
void printUptime() {
  uint32_t sec = millis() / 1000;
  uint32_t h = sec / 3600;
  uint32_t m = (sec % 3600) / 60;
  uint32_t s = sec % 60;
  Serial.printf("[%02uh %02um %02us] ", h, m, s);
}

// =============================================================
//                     AHT21 Minimal Driver
// =============================================================

class AHT21 {
public:
  bool begin() {
    Wire.beginTransmission(AHT21_ADDR);
    Wire.write(0xBE);
    Wire.write(0x08);
    Wire.write(0x00);
    if (Wire.endTransmission() != 0) return false;
    delay(10);
    return true;
  }

  bool read(float &t, float &h) {
    Wire.beginTransmission(AHT21_ADDR);
    Wire.write(0xAC);
    Wire.write(0x33);
    Wire.write(0x00);
    if (Wire.endTransmission() != 0) return false;
    delay(80);

    Wire.requestFrom((uint8_t)AHT21_ADDR, (uint8_t)7);
    if (Wire.available() < 7) return false;

    uint8_t buf[7];
    for (int i = 0; i < 7; i++) buf[i] = Wire.read();

    if (buf[0] & 0x80) return false;

    uint32_t rawH = ((uint32_t)buf[1] << 12) |
                    ((uint32_t)buf[2] << 4)  |
                    ((uint32_t)buf[3] >> 4);
    uint32_t rawT = (((uint32_t)buf[3] & 0x0F) << 16) |
                    ((uint32_t)buf[4] << 8) |
                    (uint32_t)buf[5];

    h = (float)rawH / 1048576.0f * 100.0f;
    t = (float)rawT / 1048576.0f * 200.0f - 50.0f;
    return true;
  }
};

// =============================================================
//                    ENS160 Minimal Driver
// =============================================================

class ENS160 {
public:
  bool begin() {
    // 1) Hard reset
    writeReg(0x10, 0xF4);
    delay(200);

    // 2) Verify part ID
    uint16_t id = readReg16(0x00);
    Serial.printf("[ENS160] Part ID: 0x%04X\n", id);
    if (id != 0x0160) {
      Serial.println("[ENS160] ERROR: unexpected part ID!");
      return false;
    }

    // 3) Go to Idle
    writeReg(0x10, 0x01);
    delay(100);

    // 4) Read firmware version
    writeReg(0x12, 0x0E);   // GET_APPVER
    delay(100);
    uint8_t fwMaj = readReg8(0x4C);
    uint8_t fwMin = readReg8(0x4D);
    uint8_t fwRel = readReg8(0x4E);
    Serial.printf("[ENS160] Firmware: %u.%u.%u\n", fwMaj, fwMin, fwRel);

    // 5) Clear GPR
    writeReg(0x12, 0xCC);
    delay(100);
    writeReg(0x12, 0x00);   // NOP
    delay(100);

    // 6) Set default compensation before starting
    setCompensation(25.0f, 50.0f);
    delay(50);

    // 7) Enter Standard mode
    writeReg(0x10, 0x02);
    delay(200);

    // 8) Set compensation again in standard mode
    setCompensation(25.0f, 50.0f);
    delay(100);

    // 9) Report initial state
    uint8_t mode   = readReg8(0x10);
    uint8_t status = readReg8(0x20);
    Serial.printf("[ENS160] OPMODE=0x%02X  STATUS=0x%02X\n", mode, status);
    Serial.printf("[ENS160] STATUS bits: STATAS=%u  ERR=%u  NEWDAT=%u  NEWGPR=%u\n",
                  status & 0x03, (status >> 2) & 0x01 ? 0 : 0,
                  (status >> 2) & 0x01, (status >> 3) & 0x01);

    return true;
  }

  void setCompensation(float t, float h) {
    uint16_t tVal = (uint16_t)((t + 273.15f) * 64.0f);
    uint16_t hVal = (uint16_t)(h * 512.0f);
    writeReg16(0x13, tVal);
    writeReg16(0x15, hVal);
  }

  // Returns true if new data was available and read successfully
  bool readData(uint8_t &aqi, uint16_t &tvocVal, uint16_t &eco2Val) {
    uint8_t status = readReg8(0x20);

    // Bit 2 = NEWDAT (new data available)
    if (!(status & 0x04)) return false;

    aqi     = readReg8(0x21) & 0x07;
    tvocVal = readReg16(0x22);
    eco2Val = readReg16(0x24);
    return true;
  }

  // Read STATUS register for diagnostics
  uint8_t readStatus() {
    return readReg8(0x20);
  }

  uint8_t readOpmode() {
    return readReg8(0x10);
  }

  // Read current TVOC/eCO2 registers directly (even if NEWDAT not set)
  void readRawValues(uint16_t &tvocVal, uint16_t &eco2Val) {
    tvocVal = readReg16(0x22);
    eco2Val = readReg16(0x24);
  }

private:
  void writeReg(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(ENS160_ADDR);
    Wire.write(reg);
    Wire.write(val);
    Wire.endTransmission();
  }

  void writeReg16(uint8_t reg, uint16_t val) {
    Wire.beginTransmission(ENS160_ADDR);
    Wire.write(reg);
    Wire.write(val & 0xFF);
    Wire.write((val >> 8) & 0xFF);
    Wire.endTransmission();
  }

  uint8_t readReg8(uint8_t reg) {
    Wire.beginTransmission(ENS160_ADDR);
    Wire.write(reg);
    Wire.endTransmission(false);
    Wire.requestFrom((uint8_t)ENS160_ADDR, (uint8_t)1);
    return Wire.available() ? Wire.read() : 0;
  }

  uint16_t readReg16(uint8_t reg) {
    Wire.beginTransmission(ENS160_ADDR);
    Wire.write(reg);
    Wire.endTransmission(false);
    Wire.requestFrom((uint8_t)ENS160_ADDR, (uint8_t)2);
    if (Wire.available() < 2) return 0;
    uint16_t lo = Wire.read();
    uint16_t hi = Wire.read();
    return (hi << 8) | lo;
  }
};

// Sensor instances
AHT21  aht;
ENS160 ens;

// =============================================================
//                   HomeSpan Service Structs
// =============================================================

struct SvcTemperature : Service::TemperatureSensor {
  SvcTemperature() : Service::TemperatureSensor() {
    hkTemp = new Characteristic::CurrentTemperature(20.0);
    hkTemp->setRange(-40, 85);
  }
  void loop() override {
    if (hkTemp->timeVal() > HOMEKIT_INTERVAL && ahtOk) {
      hkTemp->setVal(temperature);
    }
  }
};

struct SvcHumidity : Service::HumiditySensor {
  SvcHumidity() : Service::HumiditySensor() {
    hkHumid = new Characteristic::CurrentRelativeHumidity(50.0);
  }
  void loop() override {
    if (hkHumid->timeVal() > HOMEKIT_INTERVAL && ahtOk) {
      hkHumid->setVal(humidity);
    }
  }
};

struct SvcAirQuality : Service::AirQualitySensor {
  SvcAirQuality() : Service::AirQualitySensor() {
    hkAQ  = new Characteristic::AirQuality(0);
    hkVOC = new Characteristic::VOCDensity(0);
  }
  void loop() override {
    if (hkAQ->timeVal() > HOMEKIT_INTERVAL && ensOk && !ensWarming) {
      hkAQ->setVal(aqiIndex);
      float vocUgm3 = tvoc * 4.0f;
      if (vocUgm3 > 1000) vocUgm3 = 1000;
      hkVOC->setVal(vocUgm3);
    }
  }
};

struct SvcCO2 : Service::CarbonDioxideSensor {
  SvcCO2() : Service::CarbonDioxideSensor() {
    hkCO2Det   = new Characteristic::CarbonDioxideDetected(0);
    hkCO2Level = new Characteristic::CarbonDioxideLevel(400);
  }
  void loop() override {
    if (hkCO2Det->timeVal() > HOMEKIT_INTERVAL && ensOk && !ensWarming) {
      hkCO2Det->setVal(eco2 > 1000 ? 1 : 0);
      hkCO2Level->setVal((float)eco2);
    }
  }
};

// =============================================================
//               Icon Drawing Functions (vector)
// =============================================================

void drawIconTemp(int cx, int cy) {
  display.drawRBox(cx - 2, cy - 7, 5, 11, 1);
  display.drawDisc(cx, cy + 6, 4);
  display.setDrawColor(0);
  display.drawBox(cx - 1, cy - 5, 3, 9);
  display.drawDisc(cx, cy + 6, 2);
  display.setDrawColor(1);
  display.drawBox(cx - 1, cy - 1, 3, 5);
  display.drawDisc(cx, cy + 6, 2);
  display.drawHLine(cx + 3, cy - 4, 2);
  display.drawHLine(cx + 3, cy - 1, 2);
  display.drawHLine(cx + 3, cy + 2, 2);
}

void drawIconHumid(int cx, int cy) {
  display.drawDisc(cx, cy + 3, 5);
  display.drawTriangle(cx - 5, cy + 3, cx + 5, cy + 3, cx, cy - 7);
  display.setDrawColor(0);
  display.drawDisc(cx - 2, cy + 1, 1);
  display.setDrawColor(1);
}

void drawIconTVOC(int cx, int cy) {
  for (int row = -4; row <= 4; row += 4) {
    for (int x = -6; x <= 6; x++) {
      int yOff = (x % 3 == 0) ? -1 : ((x % 3 == 1) ? 0 : 1);
      display.drawPixel(cx + x, cy + row + yOff);
      display.drawPixel(cx + x, cy + row + yOff + 1);
    }
  }
}

void drawIconCO2(int cx, int cy) {
  display.drawDisc(cx - 3, cy - 1, 4);
  display.drawDisc(cx + 3, cy - 1, 3);
  display.drawDisc(cx, cy - 4, 3);
  display.drawBox(cx - 6, cy, 13, 4);
  display.drawRBox(cx - 7, cy + 1, 15, 4, 2);
  display.setDrawColor(0);
  display.setFont(u8g2_font_micro_tr);
  int w = display.getStrWidth("CO2");
  display.drawStr(cx - w / 2, cy + 3, "CO2");
  display.setDrawColor(1);
}

// =============================================================
//                    Widget Drawing
// =============================================================

void drawWidget(const Widget &wd, void (*iconFn)(int, int), const char *value) {
  display.drawRFrame(wd.x, wd.y, wd.w, wd.h, 6);

  int iconCx = wd.x + wd.w / 2;
  int iconCy = wd.y + 15;
  iconFn(iconCx, iconCy);

  display.drawHLine(wd.x + 4, wd.y + 28, wd.w - 8);

  display.setFont(u8g2_font_helvB10_te);
  int tw = display.getStrWidth(value);
  int tx = wd.x + (wd.w - tw) / 2;
  int ty = wd.y + 46;
  display.drawStr(tx, ty, value);
}

// =============================================================
//                     Screen Renderers
// =============================================================

void drawScreenMain() {
  char bufT[16], bufH[16];
  dtostrf(temperature, 3, 1, bufT);
  strcat(bufT, "\xB0" "C");
  snprintf(bufH, sizeof(bufH), "%d%%", constrain((int)round(humidity), 0, 100));

  drawWidget(WIDGET_L, drawIconTemp,  bufT);
  drawWidget(WIDGET_R, drawIconHumid, bufH);
}

void drawScreenAir() {
  char bufV[16], bufC[16];

  if (ensWarming) {
    strcpy(bufV, "wait...");
    strcpy(bufC, "wait...");
  } else {
    snprintf(bufV, sizeof(bufV), "%u ppb", tvoc);
    snprintf(bufC, sizeof(bufC), "%u ppm", eco2);
  }

  drawWidget(WIDGET_L, drawIconTVOC, bufV);
  drawWidget(WIDGET_R, drawIconCO2,  bufC);
}

void drawScreenPairing() {
  display.setFont(u8g2_font_helvR08_te);
  const char *title = "HomeKit";
  int tw = display.getStrWidth(title);
  display.drawStr(64 - tw / 2, 12, title);

  display.setFont(u8g2_font_helvB14_tn);
  const char *code = "466-37-726";
  tw = display.getStrWidth(code);
  display.drawStr(64 - tw / 2, 36, code);

  display.setFont(u8g2_font_helvR08_te);
  const char *status = wifiConnected ? "WiFi: OK" : "WiFi: Serial 'W'";
  tw = display.getStrWidth(status);
  display.drawStr(64 - tw / 2, 54, status);

  display.drawHLine(20, 16, 88);
}

// =============================================================
//                  Display Update Logic
// =============================================================

void updateDisplay() {
  if (!displayOn) {
    display.clearBuffer();
    display.sendBuffer();
    return;
  }

  display.clearBuffer();

  switch (curScreen) {
    case SCR_MAIN:    drawScreenMain();    break;
    case SCR_AIR:     drawScreenAir();     break;
    case SCR_PAIRING: drawScreenPairing(); break;
  }

  display.sendBuffer();
  needRedraw = false;
}

// =============================================================
//                     Sensor Reading
// =============================================================

void readSensors() {
  // ── AHT21 ──
  float rawT, rawH;
  if (aht.read(rawT, rawH)) {
    // Apply offsets for display & HomeKit
    temperature = rawT + TEMP_OFFSET;
    humidity    = constrain(rawH + HUMIDITY_OFFSET, 0.0f, 100.0f);
    ahtOk       = true;

    // Feed RAW (un-offset) values to ENS160 for compensation
    #if ENABLE_ENS160
    if (ensOk) {
      ens.setCompensation(rawT, rawH);
    }
    #endif
  }

  // ── ENS160 ──
  #if ENABLE_ENS160
  if (ensOk) {
    uint8_t  aqi;
    uint16_t tv, ec;
    if (ens.readData(aqi, tv, ec)) {
      aqiIndex = aqi;
      tvoc     = tv;
      eco2     = ec;

      // First valid data — sensor is out of warm-up!
      if (ensWarming) {
        ensWarming  = false;
        ensEverReady = true;
        printUptime();
        Serial.printf("[ENS160] *** WARM-UP COMPLETE *** "
                      "AQI=%u  TVOC=%u ppb  eCO2=%u ppm\n",
                      aqi, tv, ec);
      }
    }
  }
  #endif

  needRedraw = true;
}

// =============================================================
//               ENS160 Periodic Diagnostics
// =============================================================

void logEnsDiagnostics() {
  if (!ensOk) return;

  uint32_t now = millis();
  if (now - lastEnsLog < ENS_LOG_INTERVAL) return;
  lastEnsLog = now;

  uint8_t  status = ens.readStatus();
  uint8_t  mode   = ens.readOpmode();
  uint16_t rawTvoc, rawEco2;
  ens.readRawValues(rawTvoc, rawEco2);

  printUptime();
  Serial.printf("[ENS160] STATUS=0x%02X  OPMODE=0x%02X  "
                "TVOC=%u  eCO2=%u  warming=%s\n",
                status, mode, rawTvoc, rawEco2,
                ensWarming ? "YES" : "NO");
}

// =============================================================
//                      Button Handler
// =============================================================

void handleButton() {
  bool pressed = digitalRead(PIN_BUTTON) == HIGH;
  uint32_t now = millis();

  if (pressed && !btnPrev && (now - btnDownTime > DEBOUNCE_MS)) {
    btnDownTime = now;
    clickCount++;
    lastClickTime = now;
  }
  btnPrev = pressed;

  if (clickCount > 0 && (now - lastClickTime > CLICK_GAP_MS)) {
    if (clickCount >= 3) {
      displayOn = !displayOn;
      if (displayOn) curScreen = SCR_MAIN;
      needRedraw = true;
    } else if (clickCount == 1) {
      if (!displayOn) {
        displayOn  = true;
        curScreen  = SCR_MAIN;
      } else {
        switch (curScreen) {
          case SCR_PAIRING:
            curScreen = SCR_MAIN;
            break;
          case SCR_MAIN:
            #if ENABLE_ENS160
            curScreen = SCR_AIR;   // only reachable when ENS160 is built in
            #endif
            break;
          case SCR_AIR:
            curScreen = SCR_MAIN;
            break;
        }
      }
      needRedraw = true;
    }
    clickCount = 0;
  }
}

// =============================================================
//                  Network Time & Schedule
// =============================================================

// Issue the NTP configuration once, as soon as WiFi is up.
void initTimeSync() {
  if (timeSyncStarted) return;
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER1, NTP_SERVER2);
  timeSyncStarted = true;
  Serial.println("[TIME] NTP sync requested.");
}

// True if the display should be ON for the given local time.
// Handles a window that wraps past midnight (e.g. 22:00 → 07:00).
bool scheduleWantsOn(const struct tm &t) {
  int cur = t.tm_hour * 60 + t.tm_min;
  int on  = SCREEN_ON_HOUR  * 60 + SCREEN_ON_MINUTE;
  int off = SCREEN_OFF_HOUR * 60 + SCREEN_OFF_MINUTE;
  if (on == off) return true;                     // window disabled → always on
  if (on <  off) return (cur >= on && cur < off); // same-day window
  return (cur >= on || cur < off);                // window wraps past midnight
}

// Re-evaluate the schedule. Only acts when a switch point is
// crossed, so a manual button press stays in effect until the
// next scheduled on/off transition.
void applySchedule() {
  struct tm t;
  if (!getLocalTime(&t, 50)) return;              // time not synced yet

  bool wantOn = scheduleWantsOn(t);
  if ((int8_t)wantOn == lastSchedState) return;   // no switch point crossed

  lastSchedState = wantOn;
  displayOn      = wantOn;
  needRedraw     = true;

  printUptime();
  Serial.printf("[SCHEDULE] %02d:%02d -> display %s\n",
                t.tm_hour, t.tm_min, displayOn ? "ON" : "OFF");
}

// =============================================================
//                          SETUP
// =============================================================

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n========================================");
  Serial.println("  MeteoModule v2.1 — 48h conditioning");
  Serial.println("========================================");

  pinMode(PIN_BUTTON, INPUT);

  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(100000);

  // OLED
  display.begin();
  display.setContrast(180);
  display.clearBuffer();
  display.setFont(u8g2_font_helvR08_te);
  display.drawStr(14, 35, "MeteoModule v2.1");
  display.sendBuffer();

  // AHT21
  ahtOk = aht.begin();
  Serial.printf("[AHT21]  %s\n", ahtOk ? "OK" : "FAIL");

  // ENS160
  #if ENABLE_ENS160
  ensOk = ens.begin();
  Serial.printf("[ENS160] %s\n", ensOk ? "OK — waiting for warm-up exit..." : "FAIL");
  if (ensOk) {
    Serial.println("[ENS160] Will log status every 60s.");
    Serial.println("[ENS160] Look for '*** WARM-UP COMPLETE ***' message.");
  }
  #else
  Serial.println("[ENS160] disabled (ENABLE_ENS160 = false)");
  #endif

  delay(500);

  // HomeSpan
  homeSpan.setLogLevel(1);
  homeSpan.setPairingCode("46637726");
  homeSpan.setQRID("MTEO");

  homeSpan.begin(Category::Bridges, "MeteoModule");

  new SpanAccessory();
    new Service::AccessoryInformation();
      new Characteristic::Identify();
      new Characteristic::Name("MeteoModule Bridge");

  new SpanAccessory();
    new Service::AccessoryInformation();
      new Characteristic::Identify();
      new Characteristic::Name("Temperature");
    new SvcTemperature();

  new SpanAccessory();
    new Service::AccessoryInformation();
      new Characteristic::Identify();
      new Characteristic::Name("Humidity");
    new SvcHumidity();

  #if ENABLE_ENS160
  new SpanAccessory();
    new Service::AccessoryInformation();
      new Characteristic::Identify();
      new Characteristic::Name("Air Quality");
    new SvcAirQuality();

  new SpanAccessory();
    new Service::AccessoryInformation();
      new Characteristic::Identify();
      new Characteristic::Name("CO2 Sensor");
    new SvcCO2();
  #endif

  Serial.println("[HomeSpan] Ready. Type 'W' in Serial to configure WiFi.");
  Serial.println("[HomeSpan] Pairing code: 466-37-726");

  curScreen = SCR_PAIRING;
  needRedraw = true;

  Serial.println("\n[READY] 48h test started. Do NOT power off.\n");
}

// =============================================================
//                           LOOP
// =============================================================

void loop() {
  homeSpan.poll();

  uint32_t now = millis();

  // WiFi state tracking
  bool wf = WiFi.status() == WL_CONNECTED;
  if (wf != wifiConnected) {
    wifiConnected = wf;
    needRedraw = true;
    if (wifiConnected) initTimeSync();   // start NTP once online
  }

  // Read sensors every 2s
  if (now - lastSensorRead >= SENSOR_INTERVAL) {
    lastSensorRead = now;
    readSensors();
  }

  #if ENABLE_ENS160
  // ENS160 diagnostics every 60s
  logEnsDiagnostics();
  #endif

  #if SCREEN_SCHEDULE_ENABLE
  // Time-based display on/off (button overrides until next switch point)
  if (now - lastScheduleCheck >= SCHEDULE_CHECK_INTERVAL) {
    lastScheduleCheck = now;
    applySchedule();
  }
  #endif

  // Button
  handleButton();

  // Display refresh
  if (needRedraw || (now - lastDisplayDraw >= DISPLAY_INTERVAL)) {
    lastDisplayDraw = now;
    updateDisplay();
  }
}
