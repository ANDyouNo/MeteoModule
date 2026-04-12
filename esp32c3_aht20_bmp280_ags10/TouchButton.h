#pragma once

#include <Arduino.h>

#define TOUCH_PIN        4
#define LONG_PRESS_MS    10000
#define MULTI_TAP_MS     400
#define DEBOUNCE_MS      50

enum ButtonEvent {
    BTN_NONE = 0,
    BTN_SINGLE,
    BTN_TRIPLE,
    BTN_LONG_PRESS
};

class TouchButton {
public:
    void begin() {
        pinMode(TOUCH_PIN, INPUT);
        lastState = LOW;
        pressStart = 0;
        tapCount = 0;
        lastTapTime = 0;
        longFired = false;
    }

    ButtonEvent update() {
        bool current = digitalRead(TOUCH_PIN);
        unsigned long now = millis();
        ButtonEvent evt = BTN_NONE;

        // Rising edge — finger touched
        if (current == HIGH && lastState == LOW) {
            if (now - lastEdgeTime > DEBOUNCE_MS) {
                pressStart = now;
                longFired = false;
            }
            lastEdgeTime = now;
        }

        // Long press while held
        if (current == HIGH && pressStart > 0 && !longFired) {
            if (now - pressStart >= LONG_PRESS_MS) {
                longFired = true;
                tapCount = 0;
                lastState = current;
                return BTN_LONG_PRESS;
            }
        }

        // Falling edge — finger released
        if (current == LOW && lastState == HIGH) {
            if (now - lastEdgeTime > DEBOUNCE_MS && !longFired) {
                tapCount++;
                lastTapTime = now;
            }
            lastEdgeTime = now;
        }

        // Evaluate taps after window
        if (tapCount > 0 && (now - lastTapTime > MULTI_TAP_MS) && current == LOW) {
            if (tapCount >= 3) {
                evt = BTN_TRIPLE;
            } else if (tapCount == 1) {
                evt = BTN_SINGLE;
            }
            tapCount = 0;
        }

        lastState = current;
        return evt;
    }

private:
    bool lastState;
    unsigned long pressStart;
    unsigned long lastEdgeTime = 0;
    unsigned long lastTapTime;
    int tapCount;
    bool longFired;
};
