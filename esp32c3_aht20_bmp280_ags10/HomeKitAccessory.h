#pragma once

#include <HomeSpan.h>

// Sensor data updated from main loop
extern float g_hk_temperature;
extern float g_hk_humidity;
extern float g_hk_tvoc;

// ── Temperature Sensor ──
struct HK_TempSensor : Service::TemperatureSensor {
    SpanCharacteristic *temp;

    HK_TempSensor() : Service::TemperatureSensor() {
        temp = new Characteristic::CurrentTemperature(20.0);
        temp->setRange(-40, 125);
    }

    void loop() override {
        if (temp->timeVal() > 5000) {
            temp->setVal(g_hk_temperature);
        }
    }
};

// ── Humidity Sensor ──
struct HK_HumSensor : Service::HumiditySensor {
    SpanCharacteristic *hum;

    HK_HumSensor() : Service::HumiditySensor() {
        hum = new Characteristic::CurrentRelativeHumidity(50.0);
    }

    void loop() override {
        if (hum->timeVal() > 5000) {
            hum->setVal(g_hk_humidity);
        }
    }
};

// ── Air Quality Sensor (TVOC from AGS10) ──
struct HK_AirQualitySensor : Service::AirQualitySensor {
    SpanCharacteristic *aq;
    SpanCharacteristic *voc;

    HK_AirQualitySensor() : Service::AirQualitySensor() {
        aq  = new Characteristic::AirQuality(0);       // 0=Unknown
        voc = new Characteristic::VOCDensity(0);        // ug/m3
    }

    void loop() override {
        if (aq->timeVal() > 5000) {
            // Map TVOC ppb to HomeKit AirQuality index (0-5)
            uint8_t quality = 0; // Unknown
            float tvocPpb = g_hk_tvoc;
            if (tvocPpb >= 0) {
                if      (tvocPpb < 65)   quality = 1; // Excellent
                else if (tvocPpb < 220)  quality = 2; // Good
                else if (tvocPpb < 660)  quality = 3; // Fair
                else if (tvocPpb < 2200) quality = 4; // Inferior
                else                     quality = 5; // Poor
            }
            aq->setVal(quality);

            // VOCDensity in ug/m3 (rough: ppb * 4 for typical VOC mix)
            float vocUgm3 = tvocPpb * 4.0f;
            if (vocUgm3 > 1000) vocUgm3 = 1000; // HAP cap
            voc->setVal(vocUgm3);
        }
    }
};
