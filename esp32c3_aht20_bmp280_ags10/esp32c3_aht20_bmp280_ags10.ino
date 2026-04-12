/*
 * ESP32-C3 HomeKit Weather Station v2
 * ────────────────────────────────────
 * Board:     ESP32C3 Dev Module
 * Hardware:  SSD1306 OLED 128×64 (I2C), AHT20+BMP280, Touch GPIO4
 *            Optional: AGS10 TVOC sensor (I2C)
 *
 * Libraries (Arduino Library Manager):
 *   U8g2, Adafruit AHTX0, Adafruit BMP280 Library,
 *   Adafruit Unified Sensor, HomeSpan
 *
 * Screens:
 *   1 — Temp + Humidity  (widget style)
 *   2 — Pressure + TVOC  (or pressure full-width if AGS10 disabled)
 *   3 — HomeKit pairing code
 *
 * Button (GPIO4):
 *   1 tap       → cycle screens / dismiss pairing screen
 *   3 taps      → toggle display on/off
 *   Hold 10 sec → HomeKit factory reset + reboot
 *
 * WiFi: configured via Serial (HomeSpan CLI, type 'W')
 * Pairing code: 466-37-726
 */

// ╔══════════════════════════════════════════════════════╗
// ║     CONFIGURATION — must be BEFORE #includes         ║
// ╚══════════════════════════════════════════════════════╝

// Set to true to enable AGS10 TVOC sensor
#define ENABLE_AGS10        false

// Calibration offsets (add to raw reading)
#define TEMP_OFFSET         0.0f    // e.g. -1.5 to subtract 1.5°C
#define HUMIDITY_OFFSET     +20.0f    // e.g. +5.0 to add 5%

// HomeKit pairing code (8 digits)
#define HK_PAIRING_CODE     "46637726"
#define HK_PAIRING_DISPLAY  "466-37-726"

// I2C pins
#define PIN_SDA             8
#define PIN_SCL             9

// ── Includes (AFTER config defines so headers see them) ──
#include <Wire.h>
#include <HomeSpan.h>

#include "DisplayManager.h"
#include "SensorManager.h"
#include "TouchButton.h"
#include "HomeKitAccessory.h"

// ── Global sensor values for HomeKit ──
float g_hk_temperature = 0;
float g_hk_humidity    = 0;
float g_hk_tvoc        = 0;

// ── Objects ──
DisplayManager  oled;
SensorManager   sensors;
TouchButton     button;

// ── State ──
enum AppScreen { SCR_TEMP_HUM, SCR_PRESSURE, SCR_PAIRING };

AppScreen     currentScreen    = SCR_PAIRING;
bool          displayOn        = true;
bool          wifiConnected    = false;
bool          pairingDismissed = false;
unsigned long lastSensorRead   = 0;
const unsigned long SENSOR_INTERVAL = 2000;

// ──────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("\n=== ESP32-C3 HomeKit Weather Station v2 ===");

    // I2C
    Wire.begin(PIN_SDA, PIN_SCL);
    Wire.setClock(100000);

    // OLED (U8g2)
    oled.begin();
    oled.drawSplash("Weather Station v2");
    delay(800);

    // Sensors
    sensors.begin();
    sensors.update(TEMP_OFFSET, HUMIDITY_OFFSET);

    // Touch button
    button.begin();

    // ── HomeSpan ──
    homeSpan.setLogLevel(1);
    homeSpan.setSketchVersion("2.0.0");
    homeSpan.setHostNameSuffix("");
    homeSpan.setPairingCode(HK_PAIRING_CODE);
    homeSpan.setQRID("WXST");

    homeSpan.begin(Category::Bridges, "Weather Station");

    // Bridge accessory
    new SpanAccessory();
        new Service::AccessoryInformation();
            new Characteristic::Identify();
            new Characteristic::Name("Weather Station");
            new Characteristic::Manufacturer("DIY");
            new Characteristic::Model("ESP32C3-WX");
            new Characteristic::SerialNumber("001");
            new Characteristic::FirmwareRevision("2.0.0");

    // Temperature
    new SpanAccessory();
        new Service::AccessoryInformation();
            new Characteristic::Identify();
            new Characteristic::Name("Temperature");
        new HK_TempSensor();

    // Humidity
    new SpanAccessory();
        new Service::AccessoryInformation();
            new Characteristic::Identify();
            new Characteristic::Name("Humidity");
        new HK_HumSensor();

    // TVOC (only if AGS10 enabled)
    #if ENABLE_AGS10
    new SpanAccessory();
        new Service::AccessoryInformation();
            new Characteristic::Identify();
            new Characteristic::Name("Air Quality");
        new HK_AirQualitySensor();
    #endif

    // Show pairing screen first
    currentScreen = SCR_PAIRING;
    refreshDisplay();

    Serial.println("[HomeSpan] Ready. Type 'W' in Serial to configure WiFi.");
    Serial.printf("[HomeSpan] Pairing code: %s\n", HK_PAIRING_DISPLAY);
}

// ──────────────────────────────────────────────
void refreshDisplay() {
    if (!displayOn) {
        oled.turnOff();
        return;
    }

    switch (currentScreen) {
        case SCR_TEMP_HUM:
            oled.drawTempHumScreen(
                sensors.temperature + TEMP_OFFSET,
                sensors.humidity + HUMIDITY_OFFSET
            );
            break;
        case SCR_PRESSURE:
            #if ENABLE_AGS10
                oled.drawPressureTvocScreen(sensors.pressure, sensors.tvoc);
            #else
                oled.drawPressureFullScreen(sensors.pressure);
            #endif
            break;
        case SCR_PAIRING:
            oled.drawPairingScreen(HK_PAIRING_DISPLAY, wifiConnected);
            break;
    }
}

// ──────────────────────────────────────────────
void loop() {
    homeSpan.poll();

    unsigned long now = millis();

    // Track WiFi
    bool wf = WiFi.status() == WL_CONNECTED;
    if (wf != wifiConnected) {
        wifiConnected = wf;
        if (currentScreen == SCR_PAIRING) refreshDisplay();
    }

    // Sensor reading
    if (now - lastSensorRead >= SENSOR_INTERVAL) {
        lastSensorRead = now;
        sensors.update(TEMP_OFFSET, HUMIDITY_OFFSET);

        g_hk_temperature = sensors.temperature + TEMP_OFFSET;
        g_hk_humidity    = constrain(sensors.humidity + HUMIDITY_OFFSET, 0, 100);
        #if ENABLE_AGS10
        g_hk_tvoc        = sensors.tvoc;
        #endif

        // Refresh live data screens
        if (displayOn && currentScreen != SCR_PAIRING) {
            refreshDisplay();
        }
    }

    // Button
    ButtonEvent evt = button.update();

    switch (evt) {
        case BTN_SINGLE:
            if (!displayOn) {
                displayOn = true;
                currentScreen = SCR_TEMP_HUM;
            } else if (currentScreen == SCR_PAIRING) {
                pairingDismissed = true;
                currentScreen = SCR_TEMP_HUM;
            } else {
                currentScreen = (currentScreen == SCR_TEMP_HUM)
                    ? SCR_PRESSURE : SCR_TEMP_HUM;
            }
            refreshDisplay();
            break;

        case BTN_TRIPLE:
            if (displayOn) {
                displayOn = false;
                oled.turnOff();
            } else {
                displayOn = true;
                currentScreen = SCR_TEMP_HUM;
                refreshDisplay();
            }
            break;

        case BTN_LONG_PRESS:
            Serial.println("[HK] Factory reset triggered!");
            oled.drawSplash("Factory Reset...");
            delay(2000);
            homeSpan.processSerialCommand("F");
            ESP.restart();
            break;

        default:
            break;
    }
}
