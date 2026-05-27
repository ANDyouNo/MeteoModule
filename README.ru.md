[English](README.md) | **Русский**

# MeteoModule — ESP32-C3 HomeKit Weather Station

Два варианта прошивки для домашней метеостанции на базе ESP32-C3 с интеграцией в Apple HomeKit. Устройство отображает данные на OLED-дисплее и передаёт их в приложение Дом (Home).

---

## Версии

| | v1 | v2 |
|---|---|---|
| **Папка** | `esp32c3_aht20_bmp280_ags10` | `esp32c3_ens160_aht21` |
| **Температура / Влажность** | AHT20 | AHT21 |
| **Давление** | BMP280 | — |
| **Качество воздуха** | AGS10 (TVOC, опционально) | ENS160 (TVOC + eCO2) |
| **HomeKit сервисы** | Температура, Влажность, (Качество воздуха) | Температура, Влажность, Качество воздуха, CO₂ |
| **Экраны** | Темп+Влажность, Давление(+TVOC), Паринг | Темп+Влажность, Воздух (TVOC+eCO₂), Паринг |
| **Кнопка** | Touch GPIO4 | GPIO3 |
| **I2C** | SDA=GPIO8, SCL=GPIO9 | SDA=GPIO4, SCL=GPIO5 |
| **3D-модель корпуса** | [MakerWorld — MeteoModule v1](#) | [MakerWorld — MeteoModule v2](#) |

---

## MeteoModule v1 — `esp32c3_aht20_bmp280_ags10`

### Компоненты

- ESP32-C3 Dev Module
- SSD1306 OLED 128×64 (I2C, 0x3C)
- AHT20 — датчик температуры и влажности (I2C)
- BMP280 — датчик атмосферного давления (I2C, 0x76 или 0x77)
- AGS10 — датчик TVOC (I2C, 0x1A) — **опционально**
- Кнопка / тач-пин на GPIO4

### Подключение

```
ESP32-C3        Периферия
─────────       ──────────────────────────────
GPIO8  (SDA) →  SDA всех I2C устройств
GPIO9  (SCL) →  SCL всех I2C устройств
GPIO4        →  Кнопка (HIGH при нажатии)
3.3V         →  VCC (OLED, AHT20, BMP280, AGS10)
GND          →  GND
```

Все устройства висят на одной шине I2C. Частота шины: 100 кГц.

### Библиотеки (Arduino Library Manager)

- `U8g2` (olikraus)
- `Adafruit AHTX0`
- `Adafruit BMP280 Library`
- `Adafruit Unified Sensor`
- `HomeSpan` (Gregg Berman)

### Конфигурация

Все настройки находятся в блоке в начале файла `esp32c3_aht20_bmp280_ags10.ino`:

```cpp
// Включить датчик AGS10 TVOC (false — прошивка без TVOC)
#define ENABLE_AGS10        false

// Калибровочные поправки (прибавляются к сырым показаниям)
#define TEMP_OFFSET         0.0f    // °C, например -1.5
#define HUMIDITY_OFFSET     +20.0f  // %, например +5.0

// Код сопряжения HomeKit (8 цифр без дефисов)
#define HK_PAIRING_CODE     "46637726"
#define HK_PAIRING_DISPLAY  "466-37-726"

// Пины I2C
#define PIN_SDA             8
#define PIN_SCL             9

// Расписание экрана (по NTP)
#define SCREEN_SCHEDULE_ENABLE   true
#define SCREEN_ON_HOUR           7      // включить в 07:00
#define SCREEN_OFF_HOUR          23     // выключить в 23:00

// NTP-серверы и часовой пояс
#define NTP_SERVER1              "pool.ntp.org"
#define GMT_OFFSET_SEC           0      // UTC+3 = 10800
#define DAYLIGHT_OFFSET_SEC      0
```

### Экраны и управление кнопкой

| Действие | Результат |
|---|---|
| 1 нажатие | Переключение экрана / скрыть экран паринга |
| 3 нажатия | Включить / выключить дисплей |
| Удержание 10 с | Factory Reset HomeKit + перезагрузка |

**Экраны:** Паринг → Температура+Влажность ↔ Давление (+TVOC если включён AGS10)

---

## MeteoModule v2 — `esp32c3_ens160_aht21`

### Компоненты

- ESP32-C3 Dev Module
- SSD1306 OLED 128×64 (I2C, 0x3C)
- ENS160+AHT21 combo-модуль — датчик TVOC, eCO₂, температуры и влажности
  - ENS160: I2C 0x53 (пин ADD→GND)
  - AHT21: I2C 0x38
  - CS пин → HIGH или оставить в воздухе (режим I2C)
- Кнопка на GPIO3

### Подключение

```
ESP32-C3        Периферия
─────────       ──────────────────────────────
GPIO4  (SDA) →  SDA (OLED + ENS160/AHT21)
GPIO5  (SCL) →  SCL (OLED + ENS160/AHT21)
GPIO3        →  Кнопка (HIGH при нажатии)
3.3V         →  VCC
GND          →  GND
```

Частота шины I2C: 100 кГц.

### Библиотеки (Arduino Library Manager)

- `U8g2` (olikraus)
- `HomeSpan` (Gregg Berman)

> Драйверы AHT21 и ENS160 встроены в прошивку и сторонних библиотек не требуют.

### Конфигурация

Все настройки — в начале файла `esp32c3_ens160_aht21.ino`:

```cpp
// Калибровочные поправки
#define TEMP_OFFSET       -0.75f   // °C (компенсация нагрева платы)
#define HUMIDITY_OFFSET    0.0f    // %

// Отключить ENS160 (false — прошивка только с AHT21, без воздуха и CO₂)
#define ENABLE_ENS160        true

// Расписание экрана (по NTP)
#define SCREEN_SCHEDULE_ENABLE   true
#define SCREEN_ON_HOUR           7      // включить в 07:00
#define SCREEN_OFF_HOUR          23     // выключить в 23:00

// NTP и часовой пояс
#define NTP_SERVER1              "pool.ntp.org"
#define GMT_OFFSET_SEC           0      // UTC+3 = 10800
#define DAYLIGHT_OFFSET_SEC      0

// Пины
#define PIN_SDA     4
#define PIN_SCL     5
#define PIN_BUTTON  3
```

### Экраны и управление кнопкой

| Действие | Результат |
|---|---|
| 1 нажатие | Переключение экрана |
| 3 нажатия | Включить / выключить дисплей |

**Экраны:** Паринг → Температура+Влажность ↔ Воздух (TVOC + eCO₂)

> ENS160 требует прогрева. До завершения прогрева на экране воздуха отображается `wait...`. В Serial-мониторе появится сообщение `*** WARM-UP COMPLETE ***`.

---

## Общие возможности

**Apple HomeKit** — устройство подключается как Bridge через библиотеку HomeSpan.

- Код сопряжения: `466-37-726`
- WiFi настраивается через Serial-монитор (115200 бод): введите `W` и следуйте инструкциям
- Данные обновляются в HomeKit каждые 10 секунд

**Расписание дисплея** — автоматическое включение/выключение экрана по NTP-времени. Кнопка в любой момент перекрывает расписание вручную до следующей точки переключения.

**Калибровочные поправки** задаются в `#define` и прибавляются к сырым показаниям сенсора. Значение `HUMIDITY_OFFSET +20.0` в v1 компенсирует специфику конкретного экземпляра датчика — подберите под свой.

---

## Прошивка через Arduino IDE

1. Установить поддержку ESP32 в Arduino IDE (через Boards Manager: `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`)
2. Выбрать плату: **ESP32C3 Dev Module**
3. Установить библиотеки (см. раздел Libraries выше)
4. Открыть `.ino` файл нужной версии
5. Задать настройки в блоке конфигурации
6. Загрузить прошивку

После первой загрузки открыть Serial Monitor (115200), ввести `W` и настроить WiFi.

---

## Структура репозитория

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
