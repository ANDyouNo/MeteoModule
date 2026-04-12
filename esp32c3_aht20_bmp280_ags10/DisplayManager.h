#pragma once

#include <U8g2lib.h>

// ══════════════════════════════════════════════════════════
//  Widget-style display matching MeteoModule UI
//  Rounded frames, icon on top, divider line, value below
// ══════════════════════════════════════════════════════════

// Widget geometry
struct Widget {
    int x, y, w, h;
};

// Two side-by-side widgets: 3px margins, 6px gap
static const Widget WD_LEFT  = { 3,  3, 58, 58 };
static const Widget WD_RIGHT = { 67, 3, 58, 58 };

// Single full-width widget (for pressure-only screen)
static const Widget WD_FULL  = { 16, 3, 96, 58 };

class DisplayManager {
public:
    U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2;

    DisplayManager() : u8g2(U8G2_R0, U8X8_PIN_NONE) {}

    void begin() {
        u8g2.begin();
        u8g2.setContrast(180);
    }

    void turnOff() {
        u8g2.clearBuffer();
        u8g2.sendBuffer();
    }

    void drawSplash(const char *text) {
        u8g2.clearBuffer();
        u8g2.setFont(u8g2_font_helvR08_te);
        int w = u8g2.getStrWidth(text);
        u8g2.drawStr(64 - w / 2, 36, text);
        u8g2.sendBuffer();
    }

    // ─────────────────────────────────────────
    //  Screen 1: Temperature + Humidity
    // ─────────────────────────────────────────
    void drawTempHumScreen(float tempC, float humidity) {
        u8g2.clearBuffer();

        char bufT[12], bufH[12];
        dtostrf(tempC, 3, 1, bufT);
        strcat(bufT, "\xB0" "C");
        snprintf(bufH, sizeof(bufH), "%d%%", constrain((int)round(humidity), 0, 100));

        drawWidget(WD_LEFT,  drawIconTemp,  bufT);
        drawWidget(WD_RIGHT, drawIconHumid, bufH);

        u8g2.sendBuffer();
    }

    // ─────────────────────────────────────────
    //  Screen 2a: Pressure (full width, no TVOC)
    // ─────────────────────────────────────────
    void drawPressureFullScreen(float pressureHpa) {
        u8g2.clearBuffer();

        char buf[16];
        snprintf(buf, sizeof(buf), "%d hPa", (int)round(pressureHpa));

        drawWidget(WD_FULL, drawIconPressure, buf);

        u8g2.sendBuffer();
    }

    // ─────────────────────────────────────────
    //  Screen 2b: Pressure + TVOC (two widgets)
    // ─────────────────────────────────────────
    void drawPressureTvocScreen(float pressureHpa, uint32_t tvocPpb) {
        u8g2.clearBuffer();

        char bufP[16], bufV[16];
        snprintf(bufP, sizeof(bufP), "%d hPa", (int)round(pressureHpa));
        snprintf(bufV, sizeof(bufV), "%lu ppb", (unsigned long)tvocPpb);

        drawWidget(WD_LEFT,  drawIconPressure, bufP);
        drawWidget(WD_RIGHT, drawIconTVOC,     bufV);

        u8g2.sendBuffer();
    }

    // ─────────────────────────────────────────
    //  Pairing screen
    // ─────────────────────────────────────────
    void drawPairingScreen(const char *code, bool wifiOk) {
        u8g2.clearBuffer();

        // Title
        u8g2.setFont(u8g2_font_helvR08_te);
        const char *title = "HomeKit";
        int tw = u8g2.getStrWidth(title);
        u8g2.drawStr(64 - tw / 2, 12, title);

        // Divider
        u8g2.drawHLine(20, 16, 88);

        // Pairing code (large)
        u8g2.setFont(u8g2_font_helvB14_tn);
        tw = u8g2.getStrWidth(code);
        u8g2.drawStr(64 - tw / 2, 36, code);

        // WiFi status
        u8g2.setFont(u8g2_font_helvR08_te);
        const char *status = wifiOk ? "WiFi: OK" : "WiFi: Serial 'W'";
        tw = u8g2.getStrWidth(status);
        u8g2.drawStr(64 - tw / 2, 54, status);

        u8g2.sendBuffer();
    }

private:
    // ═══════════════════════════════════════
    //  Generic widget renderer
    // ═══════════════════════════════════════
    void drawWidget(const Widget &wd, void (*iconFn)(U8G2_SSD1306_128X64_NONAME_F_HW_I2C&, int, int),
                    const char *value)
    {
        // Rounded frame
        u8g2.drawRFrame(wd.x, wd.y, wd.w, wd.h, 6);

        // Icon centered in top portion
        int iconCx = wd.x + wd.w / 2;
        int iconCy = wd.y + 15;
        iconFn(u8g2, iconCx, iconCy);

        // Divider line
        u8g2.drawHLine(wd.x + 4, wd.y + 28, wd.w - 8);

        // Value text centered in bottom portion
        u8g2.setFont(u8g2_font_helvB10_te);
        int vw = u8g2.getStrWidth(value);
        int vx = wd.x + (wd.w - vw) / 2;
        int vy = wd.y + 46;
        u8g2.drawStr(vx, vy, value);
    }

    // ═══════════════════════════════════════
    //  Icon drawing functions (vector-based)
    // ═══════════════════════════════════════

    // Thermometer
    static void drawIconTemp(U8G2_SSD1306_128X64_NONAME_F_HW_I2C &d, int cx, int cy) {
        d.drawRBox(cx - 2, cy - 7, 5, 11, 1);
        d.drawDisc(cx, cy + 6, 4);
        d.setDrawColor(0);
        d.drawBox(cx - 1, cy - 5, 3, 9);
        d.drawDisc(cx, cy + 6, 2);
        d.setDrawColor(1);
        d.drawBox(cx - 1, cy - 1, 3, 5);
        d.drawDisc(cx, cy + 6, 2);
        d.drawHLine(cx + 3, cy - 4, 2);
        d.drawHLine(cx + 3, cy - 1, 2);
        d.drawHLine(cx + 3, cy + 2, 2);
    }

    // Water droplet
    static void drawIconHumid(U8G2_SSD1306_128X64_NONAME_F_HW_I2C &d, int cx, int cy) {
        d.drawDisc(cx, cy + 3, 5);
        d.drawTriangle(cx - 5, cy + 3, cx + 5, cy + 3, cx, cy - 7);
        d.setDrawColor(0);
        d.drawDisc(cx - 2, cy + 1, 1);
        d.setDrawColor(1);
    }

    // Pressure — three arrows pointing down
    static void drawIconPressure(U8G2_SSD1306_128X64_NONAME_F_HW_I2C &d, int cx, int cy) {
        for (int i = -6; i <= 6; i += 6) {
            int ax = cx + i;
            // Arrow shaft
            d.drawVLine(ax, cy - 6, 9);
            // Arrow head
            d.drawLine(ax - 3, cy + 1, ax, cy + 5);
            d.drawLine(ax + 3, cy + 1, ax, cy + 5);
        }
    }

    // TVOC — wavy vapour lines
    static void drawIconTVOC(U8G2_SSD1306_128X64_NONAME_F_HW_I2C &d, int cx, int cy) {
        for (int row = -4; row <= 4; row += 4) {
            for (int x = -6; x <= 6; x++) {
                int yOff = (x % 3 == 0) ? -1 : ((x % 3 == 1) ? 0 : 1);
                d.drawPixel(cx + x, cy + row + yOff);
                d.drawPixel(cx + x, cy + row + yOff + 1);
            }
        }
    }
};
