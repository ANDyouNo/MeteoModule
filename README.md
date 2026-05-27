**English** | [Русский](README.ru.md)

# MeteoModule — ESP32-C3 HomeKit Weather Station

Two firmware variants for a DIY home weather station based on the ESP32-C3, with Apple HomeKit integration. The device displays sensor readings on an OLED screen and reports them to the Home app.

---

## Versions

| | v1 | v2 |
|---|---|---|
| **Folder** | `esp32c3_aht20_bmp280_ags10` | `esp32c3_ens160_aht21` |
| **Temperature / Humidity** | AHT20 | AHT21 |
| **Pressure** | BMP280 | — |
| **Air quality** | AGS10 (TVOC, optional) | ENS160 (TVOC + eCO₂) |
| **HomeKit services** | Temperature, Humidity, (Air Quality) | Temperature, Humidity, Air Quality, CO₂ |
| **Screens** | Temp+Humidity, Pressure(+TVOC), Pairing | Temp+Humidity, Air (TVOC+eCO₂), Pairing |
| **Button** | Touch GPIO4 | GPIO3 |
| **I2C** | SDA=GPIO8, SCL=GPIO9 | SDA=GPIO4, SCL=GPIO5 |
| **3D-printed enclosure** | [MakerWorld — MeteoModule v1](#) | [MakerWorld — MeteoModule v2](#) |

---

## MeteoModule v1 — `esp32c3_aht20_bmp280_ags10`

### Components

- ESP32-C3 Dev Module
- SSD1306 OLED 128×64 (I2C, 0x3C)
- AHT20 — temperature & humidity sensor (I2C)
- BMP280 — barometric pressure sensor (I2C, 0x76 or 0x77)
- AGS10 — TVOC sensor (I2C, 0x1A) — **optional**
- Touch button on GPIO4

### Wiring

```
ESP32-C3        Peripheral
─────────       ──────────────────────────────
GPIO8  (SDA) →  SDA of all I2C devices
GPIO9  (SCL) →  SCL of all I2C devices
GPIO4        →  Button (HIGH when pressed)
3.3V         →  VCC (OLED, AHT20, BMP280, AGS10)
GND          →  GND
```

All devices share one I2C bus. Bus speed: 100 kHz.

### Libraries (Arduino Library Manager)

- `U8g2` (olikraus)
- `Adafruit AHTX0`
- `Adafruit BMP280 Library`
- `Adafruit Unified Sensor`
- `HomeSpan` (Gregg Berman)

### Configuration

All settings are in the config block at the top of `esp32c3_aht20_bmp280_ags10.ino`:

```cpp
// Enable AGS10 TVOC sensor (false = build without TVOC)
#define ENABLE_AGS10        false

// Calibration offsets (added to raw sensor readings)
#define TEMP_OFFSET         0.0f    // °C, e.g. -1.5
#define HUMIDITY_OFFSET     +20.0f  // %, e.g. +5.0

// HomeKit pairing code (8 digits, no dashes)
#define HK_PAIRING_CODE     "46637726"
#define HK_PAIRING_DISPLAY  "466-37-726"

// I2C pins
#define PIN_SDA             8
#define PIN_SCL             9

// Display schedule (NTP-based auto on/off)
#define SCREEN_SCHEDULE_ENABLE   true
#define SCREEN_ON_HOUR           7      // turn display ON  at 07:00
#define SCREEN_OFF_HOUR          23     // turn display OFF at 23:00

// NTP servers and timezone
#define NTP_SERVER1              "pool.ntp.org"
#define GMT_OFFSET_SEC           0      // UTC+3 = 10800
#define DAYLIGHT_OFFSET_SEC      0
```

### Screens & button controls

| Action | Result |
|---|---|
| 1 tap | Cycle screens / dismiss pairing screen |
| 3 taps | Toggle display on / off |
| Hold 10 s | HomeKit factory reset + reboot |

**Screens:** Pairing → Temp+Humidity ↔ Pressure (+TVOC if AGS10 enabled)

---

## MeteoModule v2 — `esp32c3_ens160_aht21`

### Components

- ESP32-C3 Dev Module
- SSD1306 OLED 128×64 (I2C, 0x3C)
- ENS160+AHT21 combo module — TVOC, eCO₂, temperature & humidity
  - ENS160: I2C 0x53 (ADD pin → GND)
  - AHT21: I2C 0x38
  - CS pin → HIGH or leave floating (selects I2C mode)
- Button on GPIO3

### Wiring

```
ESP32-C3        Peripheral
─────────       ──────────────────────────────
GPIO4  (SDA) →  SDA (OLED + ENS160/AHT21)
GPIO5  (SCL) →  SCL (OLED + ENS160/AHT21)
GPIO3        →  Button (HIGH when pressed)
3.3V         →  VCC
GND          →  GND
```

I2C bus speed: 100 kHz.

### Libraries (Arduino Library Manager)

- `U8g2` (olikraus)
- `HomeSpan` (Gregg Berman)

> AHT21 and ENS160 drivers are built into the firmware — no additional libraries required.

### Configuration

All settings are at the top of `esp32c3_ens160_aht21.ino`:

```cpp
// Calibration offsets
#define TEMP_OFFSET       -0.75f   // °C (compensates for board self-heating)
#define HUMIDITY_OFFSET    0.0f    // %

// Set to false for an AHT21-only build (no air quality / CO₂)
#define ENABLE_ENS160        true

// Display schedule (NTP-based auto on/off)
#define SCREEN_SCHEDULE_ENABLE   true
#define SCREEN_ON_HOUR           7      // turn display ON  at 07:00
#define SCREEN_OFF_HOUR          23     // turn display OFF at 23:00

// NTP servers and timezone
#define NTP_SERVER1              "pool.ntp.org"
#define GMT_OFFSET_SEC           0      // UTC+3 = 10800
#define DAYLIGHT_OFFSET_SEC      0

// Pins
#define PIN_SDA     4
#define PIN_SCL     5
#define PIN_BUTTON  3
```

### Screens & button controls

| Action | Result |
|---|---|
| 1 tap | Cycle screens |
| 3 taps | Toggle display on / off |

**Screens:** Pairing → Temp+Humidity ↔ Air (TVOC + eCO₂)

> The ENS160 requires a warm-up period. Until warm-up completes, the Air screen shows `wait...`. Watch for `*** WARM-UP COMPLETE ***` in the Serial Monitor.

---

## Common features

**Apple HomeKit** — the device connects as a Bridge accessory via the HomeSpan library.

- Pairing code: `466-37-726`
- WiFi is configured via Serial Monitor (115200 baud): type `W` and follow the prompts
- HomeKit values update every 10 seconds

**Display schedule** — the screen turns on and off automatically based on NTP time. A button press overrides the schedule manually until the next scheduled transition.

**Calibration offsets** are set via `#define` and are added to raw sensor readings. The `HUMIDITY_OFFSET +20.0` in v1 compensates for a specific sensor unit's characteristics — adjust to match your own.

---

## Flashing with Arduino IDE

1. Add ESP32 board support in Arduino IDE (Boards Manager URL: `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`)
2. Select board: **ESP32C3 Dev Module**
3. Install the required libraries (see Libraries section above)
4. Open the `.ino` file for the desired version
5. Set your configuration values in the config block
6. Upload

After the first flash, open Serial Monitor (115200 baud), type `W`, and configure WiFi.

---

## Repository structure

```
firmwares/
├── esp32c3_aht20_bmp280_ags10/   # MeteoModule v1
│   ├── esp32c3_aht20_bmp280_ags10.ino
│   ├── DisplayManager.h
│   ├── SensorManager.h
│   ├── TouchButton.h
│   └── HomeKitAccessory.h
└── esp32c3_ens160_aht21/         # MeteoModule v2
    └── esp32c3_ens160_aht21.ino
```
