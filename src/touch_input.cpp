#include "touch_input.h"

#include <Wire.h>
#include <esp_sleep.h>

namespace {

bool probeTouchAddress(uint8_t address)
{
    Wire.beginTransmission(address);
    return Wire.endTransmission() == 0;
}

bool readGt911Point(TouchDrvGT911 &touch, int32_t &x, int32_t &y)
{
    int16_t rawX = 0;
    int16_t rawY = 0;
    if (touch.getPoint(&rawX, &rawY, 1) == 0) {
        return false;
    }

    x = rawX;
    y = rawY;
    return true;
}

bool readLegacyPoint(TouchClass &touch, int32_t &x, int32_t &y)
{
    if (!digitalRead(TouchPins::kTouchInt)) {
        return false;
    }

    if (touch.scanPoint() == 0) {
        return false;
    }

    uint16_t rawX = 0;
    uint16_t rawY = 0;
    touch.getPoint(rawX, rawY, 0);
    x = rawX;
    y = rawY;
    return true;
}

TouchAction detectStrictFooterAction(int32_t x, int32_t y)
{
    if (x < 0 || y < 0 || x >= EPD_WIDTH || y >= EPD_HEIGHT) {
        return TouchAction::None;
    }

    if (y < TouchZones::kFooterTop || y > TouchZones::kFooterBottom) {
        return TouchAction::None;
    }

    if (x >= TouchZones::kPrevXMin && x <= TouchZones::kPrevXMax) {
        return TouchAction::PrevCity;
    }

    if (x >= TouchZones::kNextXMin && x <= TouchZones::kNextXMax) {
        return TouchAction::NextCity;
    }

    return TouchAction::None;
}

TouchAction detectRelaxedFooterAction(int32_t x, int32_t y)
{
    if (x < 0 || y < 0 || x >= EPD_WIDTH || y >= EPD_HEIGHT) {
        return TouchAction::None;
    }

    // Some panel/controller combos report footer touches slightly shifted upward.
    static constexpr int kRelaxedFooterTop = 430;
    static constexpr int kCenterDeadbandHalfWidth = 48;

    if (y < kRelaxedFooterTop || y >= EPD_HEIGHT) {
        return TouchAction::None;
    }

    const int32_t centerX = EPD_WIDTH / 2;
    if (x <= (centerX - kCenterDeadbandHalfWidth)) {
        return TouchAction::PrevCity;
    }

    if (x >= (centerX + kCenterDeadbandHalfWidth)) {
        return TouchAction::NextCity;
    }

    return TouchAction::None;
}

TouchAction detectActionFromPoint(int32_t x, int32_t y)
{
    TouchAction action = detectStrictFooterAction(x, y);
    if (action != TouchAction::None) {
        return action;
    }

    return detectRelaxedFooterAction(x, y);
}

TouchAction detectActionWithTransforms(int32_t rawX, int32_t rawY)
{
    const int32_t w = EPD_WIDTH;
    const int32_t h = EPD_HEIGHT;

    // 1) Direct mapping first.
    TouchAction action = detectActionFromPoint(rawX, rawY);
    if (action != TouchAction::None) {
        return action;
    }

    // 2) Try mirrored axes.
    action = detectActionFromPoint(rawX, (h - 1) - rawY);
    if (action != TouchAction::None) {
        return action;
    }

    action = detectActionFromPoint((w - 1) - rawX, rawY);
    if (action != TouchAction::None) {
        return action;
    }

    action = detectActionFromPoint((w - 1) - rawX, (h - 1) - rawY);
    if (action != TouchAction::None) {
        return action;
    }

    // 3) Try swapped X/Y orientation variants.
    action = detectActionFromPoint(rawY, rawX);
    if (action != TouchAction::None) {
        return action;
    }

    action = detectActionFromPoint(rawY, (h - 1) - rawX);
    if (action != TouchAction::None) {
        return action;
    }

    action = detectActionFromPoint((w - 1) - rawY, rawX);
    if (action != TouchAction::None) {
        return action;
    }

    return detectActionFromPoint((w - 1) - rawY, (h - 1) - rawX);
}

} // namespace

bool TouchInput::begin()
{
    _useGt911 = false;
    _useLegacyTouch = false;
    _ready = false;
    _touchHeld = false;
    _lastTouchMs = 0;

    // The controller may not be ready immediately after deep-sleep wake.
    if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_UNDEFINED) {
        delay(1000);
    }

    // Wake touch IC before probing I2C.
    pinMode(TouchPins::kTouchInt, OUTPUT);
    digitalWrite(TouchPins::kTouchInt, HIGH);
    delay(8);

    Wire.begin(TouchPins::kTouchSda, TouchPins::kTouchScl);

    pinMode(TouchPins::kTouchInt, INPUT_PULLUP);

    uint8_t gt911Address = 0;
    if (probeTouchAddress(0x14)) {
        gt911Address = 0x14;
    } else if (probeTouchAddress(0x5D)) {
        gt911Address = 0x5D;
    }

    if (gt911Address != 0) {
        _gt911.setPins(-1, TouchPins::kTouchInt);
        _useGt911 = _gt911.begin(Wire, gt911Address, TouchPins::kTouchSda, TouchPins::kTouchScl);

        if (_useGt911) {
            _gt911.setMaxCoordinates(EPD_WIDTH, EPD_HEIGHT);
            _gt911.setSwapXY(true);
            _gt911.setMirrorXY(false, true);
            _ready = true;
            Serial.printf("[touch] GT911 online at 0x%02X\n", gt911Address);
            return true;
        }
    }

    _useLegacyTouch = _legacyTouch.begin(Wire);
    if (_useLegacyTouch) {
        _ready = true;
        Serial.println("[touch] Legacy touch online at 0x5A");
        return true;
    }

    Serial.println("[touch] No touch controller detected");
    return false;
}

TouchAction TouchInput::pollTouchAction(uint32_t debounceMs)
{
    if (!_ready) {
        return TouchAction::None;
    }

    int32_t rawX = 0;
    int32_t rawY = 0;
    const char *source = nullptr;
    bool hasTouch = false;

    if (_useGt911) {
        hasTouch = readGt911Point(_gt911, rawX, rawY);
        if (hasTouch) {
            source = "gt911";
        }
    } else if (_useLegacyTouch) {
        hasTouch = readLegacyPoint(_legacyTouch, rawX, rawY);
        if (hasTouch) {
            source = "legacy";
        }
    }

    if (!hasTouch) {
        _touchHeld = false;
        return TouchAction::None;
    }

    // One city change per physical press. User must release before next change.
    if (_touchHeld) {
        return TouchAction::None;
    }
    _touchHeld = true;

    const uint32_t now = millis();
    if ((now - _lastTouchMs) < debounceMs) {
        return TouchAction::None;
    }

    const TouchAction action = detectActionWithTransforms(rawX, rawY);
    if (action != TouchAction::None) {
        _lastTouchMs = now;
        Serial.printf("[touch] src=%s raw=(%ld,%ld) action=%d\n",
                      source,
                      static_cast<long>(rawX),
                      static_cast<long>(rawY),
                      static_cast<int>(action));
    }

    return action;
}
