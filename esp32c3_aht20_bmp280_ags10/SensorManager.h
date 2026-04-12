#pragma once

#include <Wire.h>
#include <Adafruit_AHTX0.h>
#include <Adafruit_BMP280.h>

// AGS10 I2C address
#define AGS10_ADDR  0x1A

class SensorManager {
public:
    float    temperature = 0.0f;
    float    humidity    = 0.0f;
    float    pressure    = 0.0f;   // hPa
    uint32_t tvoc        = 0;      // ppb (AGS10)
    bool     ahtOk       = false;
    bool     bmpOk       = false;
    bool     agsOk       = false;

    bool begin() {
        bool ok = true;

        // AHT20
        if (!aht.begin()) {
            Serial.println("[SENSOR] AHT20 not found!");
            ok = false;
        } else {
            ahtOk = true;
            Serial.println("[AHT20]  OK");
        }

        // BMP280 — try both addresses
        if (!bmp.begin(0x76)) {
            if (!bmp.begin(0x77)) {
                Serial.println("[SENSOR] BMP280 not found!");
                ok = false;
            } else { bmpOk = true; }
        } else { bmpOk = true; }

        if (bmpOk) {
            bmp.setSampling(Adafruit_BMP280::MODE_NORMAL,
                            Adafruit_BMP280::SAMPLING_X2,
                            Adafruit_BMP280::SAMPLING_X16,
                            Adafruit_BMP280::FILTER_X16,
                            Adafruit_BMP280::STANDBY_MS_500);
            Serial.println("[BMP280] OK");
        }

        // AGS10 (optional)
        #if ENABLE_AGS10
        agsOk = ags10_init();
        Serial.printf("[AGS10]  %s\n", agsOk ? "OK" : "not found");
        #endif

        return ok;
    }

    void update(float tempOffset, float humOffset) {
        // AHT20
        if (ahtOk) {
            sensors_event_t humEvent, tempEvent;
            aht.getEvent(&humEvent, &tempEvent);
            temperature = tempEvent.temperature;
            humidity    = humEvent.relative_humidity;
        }

        // BMP280
        if (bmpOk) {
            pressure = bmp.readPressure() / 100.0f;
        }

        // AGS10
        #if ENABLE_AGS10
        if (agsOk) {
            uint32_t raw;
            if (ags10_readTVOC(raw)) {
                tvoc = raw;
            }
        }
        #endif
    }

private:
    Adafruit_AHTX0  aht;
    Adafruit_BMP280  bmp;

    // ── AGS10 minimal I2C driver ──
    // Register 0x00: read 5 bytes → [status, tvoc_hi, tvoc_mid, tvoc_lo, crc]

    bool ags10_init() {
        Wire.beginTransmission(AGS10_ADDR);
        if (Wire.endTransmission() != 0) return false;
        // Read firmware version as a connectivity check
        Wire.beginTransmission(AGS10_ADDR);
        Wire.write(0x11);  // version register
        if (Wire.endTransmission() != 0) return false;
        Wire.requestFrom((uint8_t)AGS10_ADDR, (uint8_t)5);
        if (Wire.available() < 5) return false;
        // Just drain the bytes — we only need to confirm communication
        for (int i = 0; i < 5; i++) Wire.read();
        return true;
    }

    bool ags10_readTVOC(uint32_t &ppb) {
        Wire.beginTransmission(AGS10_ADDR);
        Wire.write(0x00);
        if (Wire.endTransmission() != 0) return false;

        Wire.requestFrom((uint8_t)AGS10_ADDR, (uint8_t)5);
        if (Wire.available() < 5) return false;

        uint8_t status = Wire.read();
        uint8_t hi     = Wire.read();
        uint8_t mid    = Wire.read();
        uint8_t lo     = Wire.read();
        uint8_t crc    = Wire.read();
        (void)crc;  // CRC check omitted for simplicity

        // Bit 0 of status: 1 = sensor still warming up
        if (status & 0x01) {
            ppb = 0;
            return true; // valid but warming up
        }

        ppb = ((uint32_t)hi << 16) | ((uint32_t)mid << 8) | lo;
        return true;
    }
};
