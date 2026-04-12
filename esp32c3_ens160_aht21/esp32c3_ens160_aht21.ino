/*
 * MeteoModule — ESP32-C3 Weather Station + Apple HomeKit
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
 *   Triple press   — toggle display off / on
 *
 * HomeKit services exposed:
 *   TemperatureSensor, HumiditySensor,
 *   AirQualitySensor (VOCDensity), CarbonDioxideSensor (eCO2)
 *
 * WiFi is configured via Serial (HomeSpan CLI: type 'W').
 * Pairing code: 466-37-726
 *
 * Libraries required (install via Arduino Library Manager):
 *   - HomeSpan            (by Gregg Berman)
 *   - U8g2                (by oliver / olikraus)
 */

#include "HomeSpan.h"
#include <U8g2lib.h>
#include <Wire.h>

// ===================== Pin Configuration =====================
#define PIN_SDA     4
#define PIN_SCL     5
#define PIN_BUTTON  3

// ===================== I2C Addresses =========================
#define AHT21_ADDR   0x38
#define ENS160_ADDR  0x53   // ADD pin LOW; use 0x52 if ADD→HIGH

// ===================== Timing (ms) ===========================
#define SENSOR_INTERVAL     2000
#define DISPLAY_INTERVAL    300
#define HOMEKIT_INTERVAL    10000
#define DEBOUNCE_MS         50
#define CLICK_GAP_MS        400     // max gap between clicks for multi-click
#define ENS160_WARMUP_MS    180000  // 3 min initial warm-up

// ===================== Screen States =========================
enum Screen { SCR_PAIRING, SCR_MAIN, SCR_AIR };

// ===================== Widget Layout =========================
struct Widget {
  int x, y, w, h;
};

const Widget WIDGET_L = { 3,  3, 58, 58 };
const Widget WIDGET_R = { 67, 3, 58, 58 };

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
uint16_t eco2           = 400;
uint8_t  aqiIndex       = 0;       // ENS160 AQI 1-5
bool     ahtOk          = false;
bool     ensOk          = false;
bool     ensWarming     = true;

// Timing
uint32_t lastSensorRead   = 0;
uint32_t lastDisplayDraw  = 0;
uint32_t lastHKUpdate     = 0;
uint32_t ensStartTime     = 0;
bool     needRedraw       = true;

// Button
bool     btnPrev          = false;
uint32_t btnDownTime      = 0;
uint8_t  clickCount       = 0;
uint32_t lastClickTime    = 0;

// ===================== HomeKit Characteristic Pointers ========
SpanCharacteristic *hkTemp       = nullptr;
SpanCharacteristic *hkHumid      = nullptr;
SpanCharacteristic *hkAQ         = nullptr;
SpanCharacteristic *hkVOC        = nullptr;
SpanCharacteristic *hkCO2Det     = nullptr;
SpanCharacteristic *hkCO2Level   = nullptr;

// =============================================================
//                     AHT21 Minimal Driver
// =============================================================

class AHT21 {
public:
  bool begin() {
    Wire.beginTransmission(AHT21_ADDR);
    Wire.write(0xBE);  // init / calibrate
    Wire.write(0x08);
    Wire.write(0x00);
    if (Wire.endTransmission() != 0) return false;
    delay(10);
    return true;
  }

  bool read(float &t, float &h) {
    // Trigger measurement
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

    if (buf[0] & 0x80) return false; // busy

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
    // Read part ID
    uint16_t id = readReg16(0x00);
    if (id != 0x0160) {
      Serial.printf("[ENS160] Bad part ID: 0x%04X\n", id);
      return false;
    }
    // Set standard operating mode
    writeReg(0x10, 0x02);
    delay(50);
    return true;
  }

  // Feed temperature (C) and humidity (%) for compensation
  void setCompensation(float t, float h) {
    uint16_t tVal = (uint16_t)((t + 273.15f) * 64.0f);
    uint16_t hVal = (uint16_t)(h * 512.0f);
    writeReg16(0x13, tVal);
    writeReg16(0x15, hVal);
  }

  bool readData(uint8_t &aqi, uint16_t &tvocVal, uint16_t &eco2Val) {
    uint8_t status = readReg8(0x20);
    if (!(status & 0x04)) return false; // no new data

    aqi     = readReg8(0x21) & 0x07;
    tvocVal = readReg16(0x22);
    eco2Val = readReg16(0x24);
    return true;
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
      float vocUgm3 = tvoc * 4.0f;        // rough ppb→ug/m3 conversion
      if (vocUgm3 > 1000) vocUgm3 = 1000; // HAP cap
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

// Thermometer — centre at (cx, cy), fits ~14x14 px
void drawIconTemp(int cx, int cy) {
  // Stem
  display.drawRBox(cx - 2, cy - 7, 5, 11, 1);
  // Bulb
  display.drawDisc(cx, cy + 6, 4);
  // Inner cut-out (mercury channel)
  display.setDrawColor(0);
  display.drawBox(cx - 1, cy - 5, 3, 9);
  display.drawDisc(cx, cy + 6, 2);
  display.setDrawColor(1);
  // Mercury fill
  display.drawBox(cx - 1, cy - 1, 3, 5);
  display.drawDisc(cx, cy + 6, 2);
  // Scale ticks
  display.drawHLine(cx + 3, cy - 4, 2);
  display.drawHLine(cx + 3, cy - 1, 2);
  display.drawHLine(cx + 3, cy + 2, 2);
}

// Water droplet — centre at (cx, cy)
void drawIconHumid(int cx, int cy) {
  display.drawDisc(cx, cy + 3, 5);
  display.drawTriangle(cx - 5, cy + 3, cx + 5, cy + 3, cx, cy - 7);
  // Highlight
  display.setDrawColor(0);
  display.drawDisc(cx - 2, cy + 1, 1);
  display.setDrawColor(1);
}

// TVOC — three wavy lines (air/vapour)
void drawIconTVOC(int cx, int cy) {
  for (int row = -4; row <= 4; row += 4) {
    for (int x = -6; x <= 6; x++) {
      int yOff = (x % 3 == 0) ? -1 : ((x % 3 == 1) ? 0 : 1);
      display.drawPixel(cx + x, cy + row + yOff);
      display.drawPixel(cx + x, cy + row + yOff + 1);
    }
  }
}

// CO2 — text label with a small cloud shape
void drawIconCO2(int cx, int cy) {
  // Cloud outline
  display.drawDisc(cx - 3, cy - 1, 4);
  display.drawDisc(cx + 3, cy - 1, 3);
  display.drawDisc(cx, cy - 4, 3);
  display.drawBox(cx - 6, cy, 13, 4);
  display.drawRBox(cx - 7, cy + 1, 15, 4, 2);
  // "CO2" text on cloud
  display.setDrawColor(0);
  display.setFont(u8g2_font_micro_tr); // tiny 3px font
  int w = display.getStrWidth("CO2");
  display.drawStr(cx - w / 2, cy + 3, "CO2");
  display.setDrawColor(1);
}

// =============================================================
//                    Widget Drawing
// =============================================================
// Widget: rounded rect, top half = icon, divider line, bottom half = value

void drawWidget(const Widget &wd, void (*iconFn)(int, int), const char *value) {
  // Rounded frame
  display.drawRFrame(wd.x, wd.y, wd.w, wd.h, 6);

  // Icon centred in top portion
  int iconCx = wd.x + wd.w / 2;
  int iconCy = wd.y + 15;
  iconFn(iconCx, iconCy);

  // Divider line
  display.drawHLine(wd.x + 4, wd.y + 28, wd.w - 8);

  // Value text centred in bottom portion
  display.setFont(u8g2_font_helvB10_te);  // 10px bold, extended (has degree sign)
  int tw = display.getStrWidth(value);
  int tx = wd.x + (wd.w - tw) / 2;
  int ty = wd.y + 46;
  display.drawStr(tx, ty, value);
}

// =============================================================
//                     Screen Renderers
// =============================================================

void drawScreenMain() {
  char bufT[12], bufH[12];
  dtostrf(temperature, 3, 1, bufT);
  strcat(bufT, "\xB0" "C");            // degree sign + C
  snprintf(bufH, sizeof(bufH), "%d%%", (int)round(humidity));

  drawWidget(WIDGET_L, drawIconTemp,  bufT);
  drawWidget(WIDGET_R, drawIconHumid, bufH);
}

void drawScreenAir() {
  char bufV[12], bufC[12];

  if (ensWarming) {
    strcpy(bufV, "---");
    strcpy(bufC, "---");
  } else {
    snprintf(bufV, sizeof(bufV), "%u ppb", tvoc);
    snprintf(bufC, sizeof(bufC), "%u ppm", eco2);
  }

  drawWidget(WIDGET_L, drawIconTVOC, bufV);
  drawWidget(WIDGET_R, drawIconCO2,  bufC);
}

void drawScreenPairing() {
  // Title
  display.setFont(u8g2_font_helvR08_te);
  const char *title = "HomeKit";
  int tw = display.getStrWidth(title);
  display.drawStr(64 - tw / 2, 12, title);

  // Pairing code large
  display.setFont(u8g2_font_helvB14_tn);  // bold numbers
  const char *code = "466-37-726";
  tw = display.getStrWidth(code);
  display.drawStr(64 - tw / 2, 36, code);

  // WiFi status
  display.setFont(u8g2_font_helvR08_te);
  const char *status = wifiConnected ? "WiFi: OK" : "WiFi: Serial 'W'";
  tw = display.getStrWidth(status);
  display.drawStr(64 - tw / 2, 54, status);

  // Decorative line
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
  // AHT21
  float t, h;
  if (aht.read(t, h)) {
    temperature = t;
    humidity    = h;
    ahtOk       = true;
    // Feed compensation to ENS160
    if (ensOk) ens.setCompensation(t, h);
  }

  // ENS160
  if (ensOk) {
    uint8_t  aqi;
    uint16_t tv, ec;
    if (ens.readData(aqi, tv, ec)) {
      aqiIndex = aqi;
      tvoc     = tv;
      eco2     = ec;
    }
    // Check warm-up
    if (ensWarming && (millis() - ensStartTime > ENS160_WARMUP_MS)) {
      ensWarming = false;
    }
  }

  needRedraw = true;
}

// =============================================================
//                      Button Handler
// =============================================================
// Touch button: HIGH when pressed.
// Detects single-click and triple-click via click counting
// within a time window.

void handleButton() {
  bool pressed = digitalRead(PIN_BUTTON) == HIGH;
  uint32_t now = millis();

  // Rising edge detection with debounce
  if (pressed && !btnPrev && (now - btnDownTime > DEBOUNCE_MS)) {
    btnDownTime = now;
    clickCount++;
    lastClickTime = now;
  }
  btnPrev = pressed;

  // Evaluate after click gap window expires
  if (clickCount > 0 && (now - lastClickTime > CLICK_GAP_MS)) {
    if (clickCount >= 3) {
      // ── Triple click: toggle display ──
      displayOn = !displayOn;
      if (displayOn) {
        curScreen = SCR_MAIN;
      }
      needRedraw = true;
    } else if (clickCount == 1) {
      // ── Single click ──
      if (!displayOn) {
        displayOn  = true;
        curScreen  = SCR_MAIN;
      } else {
        switch (curScreen) {
          case SCR_PAIRING: curScreen = SCR_MAIN; break;
          case SCR_MAIN:    curScreen = SCR_AIR;  break;
          case SCR_AIR:     curScreen = SCR_MAIN; break;
        }
      }
      needRedraw = true;
    }
    // (double-click is ignored / treated as two singles — only last evaluated)
    clickCount = 0;
  }
}

// =============================================================
//                          SETUP
// =============================================================

void setup() {
  Serial.begin(115200);
  Serial.println("\n[MeteoModule] Starting...");

  // Button
  pinMode(PIN_BUTTON, INPUT);

  // I2C
  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(100000);  // 100 kHz — safe for all devices

  // OLED
  display.begin();
  display.setContrast(180);
  display.clearBuffer();
  display.setFont(u8g2_font_helvR08_te);
  display.drawStr(20, 35, "MeteoModule v1.0");
  display.sendBuffer();

  // AHT21
  ahtOk = aht.begin();
  Serial.printf("[AHT21]  %s\n", ahtOk ? "OK" : "FAIL");

  // ENS160
  ensOk = ens.begin();
  ensStartTime = millis();
  Serial.printf("[ENS160] %s\n", ensOk ? "OK (warming up 3 min)" : "FAIL");

  delay(500);

  // ── HomeSpan ──
  homeSpan.setLogLevel(1);
  homeSpan.setPairingCode("46637726");
  homeSpan.setQRID("MTEO");
  homeSpan.begin(Category::Bridges, "MeteoModule");

  // Bridge accessory (required first)
  new SpanAccessory();
    new Service::AccessoryInformation();
      new Characteristic::Identify();
      new Characteristic::Name("MeteoModule Bridge");

  // Temperature
  new SpanAccessory();
    new Service::AccessoryInformation();
      new Characteristic::Identify();
      new Characteristic::Name("Temperature");
    new SvcTemperature();

  // Humidity
  new SpanAccessory();
    new Service::AccessoryInformation();
      new Characteristic::Identify();
      new Characteristic::Name("Humidity");
    new SvcHumidity();

  // Air Quality (TVOC)
  new SpanAccessory();
    new Service::AccessoryInformation();
      new Characteristic::Identify();
      new Characteristic::Name("Air Quality");
    new SvcAirQuality();

  // CO2
  new SpanAccessory();
    new Service::AccessoryInformation();
      new Characteristic::Identify();
      new Characteristic::Name("CO2 Sensor");
    new SvcCO2();

  Serial.println("[HomeSpan] Ready. Type 'W' in Serial to configure WiFi.");
  Serial.println("[HomeSpan] Pairing code: 466-37-726");

  curScreen = SCR_PAIRING;
  needRedraw = true;
}

// =============================================================
//                           LOOP
// =============================================================

void loop() {
  // HomeSpan processing (WiFi, HAP, serial CLI)
  homeSpan.poll();

  uint32_t now = millis();

  // Track WiFi state
  bool wf = WiFi.status() == WL_CONNECTED;
  if (wf != wifiConnected) {
    wifiConnected = wf;
    needRedraw = true;
  }

  // Read sensors periodically
  if (now - lastSensorRead >= SENSOR_INTERVAL) {
    lastSensorRead = now;
    readSensors();
  }

  // Button
  handleButton();

  // Refresh display when needed or periodically
  if (needRedraw || (now - lastDisplayDraw >= DISPLAY_INTERVAL)) {
    lastDisplayDraw = now;
    updateDisplay();
  }
}
