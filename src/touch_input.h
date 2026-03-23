#pragma once

#include <Arduino.h>

#include <TouchDrvGT911.hpp>

#include "epd_driver.h"
#include "touch.h"

enum class TouchAction {
    None,
    PrevCity,
    NextCity,
};

namespace TouchPins {
#if defined(CONFIG_IDF_TARGET_ESP32S3)
static constexpr int kTouchScl = 17;
static constexpr int kTouchSda = 18;
static constexpr int kTouchInt = 47;
#else
static constexpr int kTouchScl = 14;
static constexpr int kTouchSda = 15;
static constexpr int kTouchInt = 13;
#endif
} // namespace TouchPins

namespace TouchZones {
static constexpr int kFooterTop = 486;
static constexpr int kFooterBottom = EPD_HEIGHT - 1;

static constexpr int kPrevXMin = 20;
static constexpr int kPrevXMax = 220;

static constexpr int kNextXMin = EPD_WIDTH - 220;
static constexpr int kNextXMax = EPD_WIDTH - 20;
} // namespace TouchZones

class TouchInput {
public:
    bool begin();
    TouchAction pollTouchAction(uint32_t debounceMs);

private:
    TouchDrvGT911 _gt911;
    TouchClass _legacyTouch;
    bool _useGt911 = false;
    bool _useLegacyTouch = false;
    bool _ready = false;
    bool _touchHeld = false;
    uint32_t _lastTouchMs = 0;
};
