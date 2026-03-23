#pragma once

#include <Arduino.h>

#include "epd_driver.h"
#include "weather_types.h"

class DisplayRenderer {
public:
    bool begin();

    void renderDashboard(const CityState &city, uint8_t cityIndex, uint8_t cityCount, bool forceFullRefresh);
    void renderStatusMessage(const String &title, const String &detail, bool forceFullRefresh);

private:
    void clearFramebuffer();
    void pushToDisplay(bool forceFullRefresh);

    void drawText(const GFXfont *font, int32_t x, int32_t y, const String &text);
    void drawSectionFrames();
    void drawWeatherIcon(int16_t weatherId, bool isDaylight, int32_t centerX, int32_t centerY, int32_t size, bool suppressMoon = false, bool forceSmallStyle = false);
    void drawTrendChart(int32_t panelX,
                        int32_t panelY,
                        int32_t panelW,
                        int32_t panelH,
                        const String &title,
                        const int16_t *values,
                        size_t count,
                        int16_t axisMin,
                        int16_t axisMax,
                        const String &unitSuffix,
                        const String *xLabels = nullptr);

    String formatLocalTime(int64_t utcEpoch, int32_t offsetSeconds) const;
    String formatLocalDay(int64_t utcEpoch, int32_t offsetSeconds) const;

    uint8_t *_framebuffer = nullptr;
};


