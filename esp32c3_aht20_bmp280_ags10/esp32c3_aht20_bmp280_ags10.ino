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
 * The display also turns on/off automatically on a network-time
 * schedule (SCREEN_SCHEDULE_* in the config block). The button
 * overrides the schedule until the next scheduled switch point.
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

// ── Display schedule (automatic on/off via network NTP clock) ──
// The button still works at any time and overrides the schedule
// until the next scheduled switch point.
// NTP_SERVER1 may be your router's LAN IP (many routers act as an
// NTP server, e.g. "192.168.1.1") or a public internet pool.
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

// ── Includes (AFTER config defines so headers see them) ──
#include <Wire.h>
#include <HomeSpan.h>
#include <time.h>

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

// Display schedule state
unsigned long lastScheduleCheck = 0;
bool          timeSyncStarted   = false;  // true once NTP configTime() issued
int8_t        lastSchedState    = -1;     // -1 = unknown, 0 = off, 1 = on

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
//  Network time & display schedule
// ──────────────────────────────────────────────

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
    refreshDisplay();                               // draws screen or turns off

    Serial.printf("[SCHEDULE] %02d:%02d -> display %s\n",
                  t.tm_hour, t.tm_min, displayOn ? "ON" : "OFF");
}

// ──────────────────────────────────────────────
void loop() {
    homeSpan.poll();

    unsigned long now = millis();

    // Track WiFi
    bool wf = WiFi.status() == WL_CONNECTED;
    if (wf != wifiConnected) {
        wifiConnected = wf;
        if (wifiConnected) initTimeSync();   // start NTP once online
        if (currentScreen == SCR_PAIRING) refreshDisplay();
    }

    #if SCREEN_SCHEDULE_ENABLE
    // Time-based display on/off (button overrides until next switch point)
    if (now - lastScheduleCheck >= SCHEDULE_CHECK_INTERVAL) {
        lastScheduleCheck = now;
        applySchedule();
    }
    #endif

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
