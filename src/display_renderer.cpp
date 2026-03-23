#include "display_renderer.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "fonts/opensans10b.h"
#include "fonts/opensans12b.h"
#include "fonts/opensans18b.h"

namespace {

constexpr uint8_t WHITE = 0xFF;
constexpr uint8_t BLACK = 0x00;

constexpr int HEADER_BOX_Y = 10;
constexpr int HEADER_BOX_H = 72;

constexpr int HEADER_Y = 50;
constexpr int META_Y = 76;

constexpr int CURRENT_BOX_Y = 92;
constexpr int CURRENT_BOX_H = 90;

constexpr int HOURLY_BOX_Y = 188;
constexpr int HOURLY_BOX_H = 170;

constexpr int DAILY_BOX_Y = 366;
constexpr int DAILY_BOX_H = 132;

constexpr int FOOTER_BOX_Y = 502;
constexpr int FOOTER_BOX_H = 30;

constexpr int PREV_BTN_X = 20;
constexpr int PREV_BTN_W = 200;

constexpr int NEXT_BTN_X = EPD_WIDTH - 220;
constexpr int NEXT_BTN_W = 200;

int32_t textWidth(const GFXfont *font, const String &text)
{
    if (font == nullptr || text.isEmpty()) {
        return 0;
    }

    int32_t x = 0;
    int32_t y = 0;
    int32_t x1 = 0;
    int32_t y1 = 0;
    int32_t w = 0;
    int32_t h = 0;
    get_text_bounds(font, text.c_str(), &x, &y, &x1, &y1, &w, &h, nullptr);
    return w;
}

void trimLastUtf8Codepoint(String &text)
{
    if (text.isEmpty()) {
        return;
    }

    int idx = static_cast<int>(text.length()) - 1;
    while (idx > 0 && (static_cast<uint8_t>(text[idx]) & 0xC0) == 0x80) {
        --idx;
    }

    text.remove(idx);
}

String truncateToWidth(const GFXfont *font, const String &text, int32_t maxWidth)
{
    if (font == nullptr || maxWidth <= 0) {
        return "";
    }

    if (textWidth(font, text) <= maxWidth) {
        return text;
    }

    const String ellipsis = "...";
    const int32_t ellipsisWidth = textWidth(font, ellipsis);
    if (ellipsisWidth > maxWidth) {
        return "";
    }

    String trimmed = text;
    while (!trimmed.isEmpty()) {
        trimLastUtf8Codepoint(trimmed);
        if (textWidth(font, trimmed + ellipsis) <= maxWidth) {
            return trimmed + ellipsis;
        }
    }

    return ellipsis;
}

String formatLocalEpoch(int64_t utcEpoch, int32_t offsetSeconds, const char *format)
{
    if (utcEpoch <= 0) {
        return "--";
    }

    time_t shifted = static_cast<time_t>(utcEpoch + offsetSeconds);
    struct tm tmValue;
    gmtime_r(&shifted, &tmValue);

    char out[24] = {0};
    strftime(out, sizeof(out), format, &tmValue);
    return String(out);
}

String weatherLabelFromId(int16_t weatherId)
{
    if (weatherId == 800) {
        return "Clear sky";
    }
    if (weatherId == 801) {
        return "Few clouds";
    }
    if (weatherId == 802) {
        return "Scattered clouds";
    }
    if (weatherId == 803) {
        return "Broken clouds";
    }
    if (weatherId == 804) {
        return "Overcast clouds";
    }
    if (weatherId >= 200 && weatherId < 300) {
        return "Thunderstorm";
    }
    if (weatherId >= 300 && weatherId < 400) {
        return "Drizzle";
    }
    if (weatherId >= 500 && weatherId < 600) {
        return "Rain";
    }
    if (weatherId >= 600 && weatherId < 700) {
        return "Snow";
    }
    if (weatherId >= 700 && weatherId < 800) {
        return "Mist";
    }

    return "Weather";
}

bool isDaylightNow(const WeatherSnapshot &snapshot)
{
    if (snapshot.sunriseEpochUtc <= 0 || snapshot.sunsetEpochUtc <= 0) {
        return true;
    }

    return snapshot.nowEpochUtc >= snapshot.sunriseEpochUtc &&
           snapshot.nowEpochUtc < snapshot.sunsetEpochUtc;
}

bool isRainWeatherId(int16_t weatherId)
{
    return weatherId >= 200 && weatherId < 600;
}

bool isLikelyDaylightHour(int64_t utcEpoch, int32_t offsetSeconds)
{
    if (utcEpoch <= 0) {
        return true;
    }

    time_t shifted = static_cast<time_t>(utcEpoch + offsetSeconds);
    struct tm tmValue;
    gmtime_r(&shifted, &tmValue);
    return tmValue.tm_hour >= 7 && tmValue.tm_hour < 19;
}


void computeSeriesRange(const int16_t *values, size_t count, int16_t minSpan, int16_t &outMin, int16_t &outMax)
{
    outMin = 0;
    outMax = 1;
    if (values == nullptr || count == 0) {
        return;
    }

    outMin = values[0];
    outMax = values[0];
    for (size_t i = 1; i < count; ++i) {
        if (values[i] < outMin) {
            outMin = values[i];
        }
        if (values[i] > outMax) {
            outMax = values[i];
        }
    }

    const int16_t span = static_cast<int16_t>(outMax - outMin);
    if (span < minSpan) {
        const int16_t extra = static_cast<int16_t>(minSpan - span);
        const int16_t lowerPad = static_cast<int16_t>(extra / 2);
        const int16_t upperPad = static_cast<int16_t>(extra - lowerPad);
        outMin = static_cast<int16_t>(outMin - lowerPad);
        outMax = static_cast<int16_t>(outMax + upperPad);
    }

    if (outMin == outMax) {
        outMax = static_cast<int16_t>(outMin + 1);
    }
}

void drawDottedHorizontalLine(uint8_t *framebuffer, int32_t xStart, int32_t xEnd, int32_t y)
{
    if (framebuffer == nullptr || xEnd < xStart) {
        return;
    }

    for (int32_t x = xStart; x <= xEnd; x += 6) {
        int32_t segmentEnd = x + 2;
        if (segmentEnd > xEnd) {
            segmentEnd = xEnd;
        }
        epd_draw_line(x, y, segmentEnd, y, BLACK, framebuffer);
    }
}

String iconCodeFromWeatherId(int16_t weatherId, bool isDaylight)
{
    const String dn = isDaylight ? "d" : "n";

    if (weatherId == 800) {
        return "01" + dn;
    }
    if (weatherId == 801) {
        return "02" + dn;
    }
    if (weatherId == 802) {
        return "03" + dn;
    }
    if (weatherId == 803 || weatherId == 804) {
        return "04" + dn;
    }
    if (weatherId >= 200 && weatherId < 300) {
        return "11" + dn;
    }
    if (weatherId >= 300 && weatherId < 400) {
        return "09" + dn;
    }
    if (weatherId >= 500 && weatherId < 600) {
        if (weatherId <= 504) {
            return "10" + dn;
        }
        return "09" + dn;
    }
    if (weatherId >= 600 && weatherId < 700) {
        return "13" + dn;
    }
    if (weatherId >= 700 && weatherId < 800) {
        return "50" + dn;
    }

    return "01" + dn;
}

double normalizedLunarPhase(int64_t utcEpoch)
{
    constexpr double secondsPerLunarCycle = 29.530588853 * 86400.0;
    constexpr double referenceNewMoonUtc = 947182440.0; // 2000-01-06 18:14 UTC

    double phaseSeconds = fmod(static_cast<double>(utcEpoch) - referenceNewMoonUtc,
                               secondsPerLunarCycle);
    if (phaseSeconds < 0.0) {
        phaseSeconds += secondsPerLunarCycle;
    }

    return phaseSeconds / secondsPerLunarCycle;
}

String moonPhaseLabelFromEpoch(int64_t utcEpoch)
{
    int64_t epoch = utcEpoch;
    if (epoch <= 0) {
        epoch = static_cast<int64_t>(time(nullptr));
    }

    const double phase = normalizedLunarPhase(epoch);
    if (phase < 0.03 || phase >= 0.97) {
        return "New Moon";
    }
    if (phase < 0.22) {
        return "Waxing Crescent";
    }
    if (phase < 0.28) {
        return "First Quarter";
    }
    if (phase < 0.47) {
        return "Waxing Gibbous";
    }
    if (phase < 0.53) {
        return "Full Moon";
    }
    if (phase < 0.72) {
        return "Waning Gibbous";
    }
    if (phase < 0.78) {
        return "Last Quarter";
    }

    return "Waning Crescent";
}

void drawMoonPanelIcon(uint8_t *framebuffer,
                       int32_t centerX,
                       int32_t centerY,
                       int32_t radius,
                       int64_t nowEpochUtc)
{
    if (framebuffer == nullptr || radius < 8) {
        return;
    }

    int64_t epochUtc = nowEpochUtc;
    if (epochUtc <= 0) {
        epochUtc = static_cast<int64_t>(time(nullptr));
    }

    const double phase = normalizedLunarPhase(epochUtc);
    const bool waxing = phase <= 0.5;
    const double halfCycle = waxing ? phase * 2.0 : (phase - 0.5) * 2.0;

    epd_fill_circle(centerX, centerY, radius, BLACK, framebuffer);

    for (int32_t y = -radius; y <= radius; ++y) {
        const double yValue = static_cast<double>(y);
        const double xEdge = sqrt(static_cast<double>(radius * radius) - yValue * yValue);
        const double xBound = xEdge * (1.0 - 2.0 * halfCycle);

        int32_t xStart = 0;
        int32_t xEnd = 0;
        if (waxing) {
            xStart = static_cast<int32_t>(ceil(xBound));
            xEnd = static_cast<int32_t>(floor(xEdge));
        } else {
            xStart = static_cast<int32_t>(ceil(-xEdge));
            xEnd = static_cast<int32_t>(floor(xBound));
        }

        if (xStart <= xEnd) {
            epd_draw_line(centerX + xStart,
                          centerY + y,
                          centerX + xEnd,
                          centerY + y,
                          WHITE,
                          framebuffer);
        }
    }

    for (int degree = 0; degree < 360; degree += 8) {
        const float rad0 = static_cast<float>(degree) * PI / 180.0f;
        const float rad1 = static_cast<float>(degree + 8) * PI / 180.0f;

        const int32_t x0 = centerX + static_cast<int32_t>(cosf(rad0) * radius);
        const int32_t y0 = centerY + static_cast<int32_t>(sinf(rad0) * radius);
        const int32_t x1 = centerX + static_cast<int32_t>(cosf(rad1) * radius);
        const int32_t y1 = centerY + static_cast<int32_t>(sinf(rad1) * radius);
        epd_draw_line(x0, y0, x1, y1, BLACK, framebuffer);
    }
}

void drawCardMoonBadge(uint8_t *framebuffer, int32_t centerX, int32_t centerY, int32_t radius)
{
    if (framebuffer == nullptr || radius < 2) {
        return;
    }

    epd_fill_circle(centerX, centerY, radius, BLACK, framebuffer);
    epd_fill_circle(centerX + (radius / 2) + 1, centerY - 1, radius - 1, WHITE, framebuffer);
}
} // namespace

bool DisplayRenderer::begin()
{
    epd_init();

    _framebuffer = static_cast<uint8_t *>(ps_calloc(sizeof(uint8_t), EPD_WIDTH * EPD_HEIGHT / 2));
    if (_framebuffer == nullptr) {
        return false;
    }

    clearFramebuffer();
    return true;
}

void DisplayRenderer::renderDashboard(const CityState &city, uint8_t cityIndex, uint8_t cityCount, bool forceFullRefresh)
{
    clearFramebuffer();
    drawSectionFrames();

    const String staleState = city.snapshot.stale ? "STALE" : "LIVE";
    const int32_t staleWidth = textWidth(&OpenSans10B, staleState);
    const int32_t staleX = EPD_WIDTH - 20 - staleWidth;

    const String titleRaw = city.resolvedName.isEmpty() ? city.query : city.resolvedName;
    const String title = truncateToWidth(&OpenSans18B, titleRaw, staleX - 40);
    drawText(&OpenSans18B, 20, HEADER_Y, title);
    drawText(&OpenSans10B, staleX, HEADER_Y - 2, staleState);

    const String updated = "Updated: " + formatLocalTime(city.snapshot.fetchedAtEpoch, city.snapshot.timezoneOffsetSec);
    drawText(&OpenSans10B, 20, META_Y, truncateToWidth(&OpenSans10B, updated, EPD_WIDTH - 40));

    const int16_t hi = city.snapshot.daily[0].tempMaxC;
    const int16_t lo = city.snapshot.daily[0].tempMinC;

    const int currentLeftX = 20;
    const int currentLeftY = CURRENT_BOX_Y + 8;
    const int currentLeftW = 430;
    const int currentLeftH = CURRENT_BOX_H - 16;

    drawText(&OpenSans18B, currentLeftX + 14, CURRENT_BOX_Y + 34, String(city.snapshot.currentTempC) + "C");
    drawText(&OpenSans18B, currentLeftX + 180, CURRENT_BOX_Y + 34, String(city.snapshot.humidityPct) + "%");
    drawText(&OpenSans12B, currentLeftX + 14, CURRENT_BOX_Y + 60, String(hi) + "C | " + String(lo) + "C");

    const String condition = weatherLabelFromId(city.snapshot.currentWeatherId);
    drawText(&OpenSans12B, currentLeftX + 14, CURRENT_BOX_Y + 82,
             truncateToWidth(&OpenSans12B, condition, currentLeftW - 28));

    drawWeatherIcon(city.snapshot.currentWeatherId,
                    isDaylightNow(city.snapshot),
                    currentLeftX + currentLeftW - 80,
                    CURRENT_BOX_Y + 45,
                    58);

    const int currentRightX = currentLeftX + currentLeftW + 14;
    const int currentRightY = currentLeftY;
    const int currentRightW = EPD_WIDTH - currentRightX - 20;
    const int currentRightH = currentLeftH;

    const int leftPaneW = 220;
    const int dividerX = currentRightX + leftPaneW;
    const int moonCenterX = currentRightX + (leftPaneW / 2);
    const int moonCenterY = currentRightY + 24;

    drawMoonPanelIcon(_framebuffer,
                      moonCenterX,
                      moonCenterY,
                      24,
                      city.snapshot.nowEpochUtc);

    const String moonPhaseLabel = moonPhaseLabelFromEpoch(city.snapshot.nowEpochUtc);
    drawText(&OpenSans10B,
             currentRightX + (leftPaneW - textWidth(&OpenSans10B, moonPhaseLabel)) / 2,
             currentRightY + currentRightH - 8,
             moonPhaseLabel);

    const String sunrise = formatLocalTime(city.snapshot.sunriseEpochUtc, city.snapshot.timezoneOffsetSec);
    const String sunset = formatLocalTime(city.snapshot.sunsetEpochUtc, city.snapshot.timezoneOffsetSec);
    const int row1Y = currentRightY + 28;
    const int row2Y = currentRightY + 58;
    const int timesRightX = currentRightX + currentRightW - 16;
    const int sunriseX = timesRightX - textWidth(&OpenSans12B, sunrise);
    const int sunsetX = timesRightX - textWidth(&OpenSans12B, sunset);
    const int timeLeftX = sunriseX < sunsetX ? sunriseX : sunsetX;
    int labelsX = timeLeftX - 88;
    const int minLabelsX = dividerX + 14;
    if (labelsX < minLabelsX) {
        labelsX = minLabelsX;
    }

    epd_draw_line(labelsX,
                  currentRightY + (currentRightH / 2),
                  timesRightX,
                  currentRightY + (currentRightH / 2),
                  BLACK,
                  _framebuffer);

    drawText(&OpenSans10B, labelsX, row1Y, "Sunrise");
    drawText(&OpenSans12B, sunriseX, row1Y, sunrise);

    drawText(&OpenSans10B, labelsX, row2Y, "Sunset");
    drawText(&OpenSans12B, sunsetX, row2Y, sunset);

    drawText(&OpenSans12B, 20, HOURLY_BOX_Y + 24, "Next 24 hours");

    int rainStartIndex = -1;
    int rainStopIndex = -1;
    for (size_t i = 0; i < HOURLY_FORECAST_COUNT; ++i) {
        const bool rainy = isRainWeatherId(city.snapshot.hourly[i].weatherId);
        if (rainStartIndex < 0) {
            if (rainy) {
                rainStartIndex = static_cast<int>(i);
            }
            continue;
        }

        if (!rainy) {
            rainStopIndex = static_cast<int>(i);
            break;
        }
    }

    String rainSummary = "Rain: none expected";
    if (rainStartIndex >= 0) {
        const String rainStart = formatLocalTime(city.snapshot.hourly[rainStartIndex].timestampUtc,
                                                 city.snapshot.timezoneOffsetSec);
        if (rainStopIndex >= 0) {
            const String rainStop = formatLocalTime(city.snapshot.hourly[rainStopIndex].timestampUtc,
                                                    city.snapshot.timezoneOffsetSec);
            rainSummary = "Rain: " + rainStart + " - " + rainStop;
        } else {
            rainSummary = "Rain: starts " + rainStart + " (continues)";
        }
    }

    const String centeredRainSummary = truncateToWidth(&OpenSans10B, rainSummary, EPD_WIDTH - 280);
    drawText(&OpenSans10B,
             (EPD_WIDTH - textWidth(&OpenSans10B, centeredRainSummary)) / 2,
             HOURLY_BOX_Y + 24,
             centeredRainSummary);

    constexpr size_t kHourlyCardCount = 8;
    constexpr size_t kHoursPerCard = 3;
    const int cardsX = 20;
    const int cardsW = EPD_WIDTH - 40;
    const int cardGap = 6;
    const int cardW = (cardsW - (static_cast<int>(kHourlyCardCount) - 1) * cardGap) /
                      static_cast<int>(kHourlyCardCount);
    const int cardY = HOURLY_BOX_Y + 34;
    const int cardH = HOURLY_BOX_H - (cardY - HOURLY_BOX_Y) - 4;

    for (size_t card = 0; card < kHourlyCardCount; ++card) {
        const size_t startIndex = card * kHoursPerCard;
        if (startIndex >= HOURLY_FORECAST_COUNT) {
            break;
        }

        const int x = cardsX + static_cast<int>(card) * (cardW + cardGap);

        int16_t minTemp = city.snapshot.hourly[startIndex].temperatureC;
        int16_t maxTemp = city.snapshot.hourly[startIndex].temperatureC;
        uint8_t maxRain = city.snapshot.hourly[startIndex].rainProbPct;

        for (size_t j = 1; j < kHoursPerCard && (startIndex + j) < HOURLY_FORECAST_COUNT; ++j) {
            const HourlyForecast &sample = city.snapshot.hourly[startIndex + j];
            if (sample.temperatureC < minTemp) {
                minTemp = sample.temperatureC;
            }
            if (sample.temperatureC > maxTemp) {
                maxTemp = sample.temperatureC;
            }
            if (sample.rainProbPct > maxRain) {
                maxRain = sample.rainProbPct;
            }
        }

        const HourlyForecast &h = city.snapshot.hourly[startIndex];
        const bool cardIsDaylight = isLikelyDaylightHour(h.timestampUtc, city.snapshot.timezoneOffsetSec);

        const String hour = formatLocalTime(h.timestampUtc, city.snapshot.timezoneOffsetSec);
        const int32_t hourWidth = textWidth(&OpenSans10B, hour);
        const int32_t moonRadius = 4;
        const int32_t moonGap = 6;
        const int32_t moonWidth = cardIsDaylight ? 0 : (moonRadius * 2 + moonGap);
        const int32_t timeX = x + (cardW - hourWidth - moonWidth) / 2;

        drawText(&OpenSans10B, timeX, cardY + 14, hour);

        if (!cardIsDaylight) {
            const int32_t moonCenterX = timeX + hourWidth + moonGap + moonRadius;
            const int32_t moonCenterY = cardY + 10;
            drawCardMoonBadge(_framebuffer, moonCenterX, moonCenterY, moonRadius);
        }


        drawWeatherIcon(h.weatherId,
                        cardIsDaylight,
                        x + cardW / 2,
                        cardY + 60,
                        76,
                        !cardIsDaylight,
                        true);

        const String tempRange = String(maxTemp) + "/" + String(minTemp) + "C";
        drawText(&OpenSans10B,
                 x + (cardW - textWidth(&OpenSans10B, tempRange)) / 2,
                 cardY + cardH - 24,
                 tempRange);

        const String rainLine = "R" + String(maxRain) + "%";
        drawText(&OpenSans10B,
                 x + (cardW - textWidth(&OpenSans10B, rainLine)) / 2,
                 cardY + cardH - 4,
                 rainLine);
    }

    drawText(&OpenSans10B, 20, DAILY_BOX_Y + 18, "Next 7 days trends");

    int16_t tempTrend[DAILY_FORECAST_COUNT] = {0};
    int16_t humidityTrend[DAILY_FORECAST_COUNT] = {0};
    int16_t rainTrend[DAILY_FORECAST_COUNT] = {0};
    String dayLabels[DAILY_FORECAST_COUNT];

    for (size_t i = 0; i < DAILY_FORECAST_COUNT; ++i) {
        const DailyForecast &d = city.snapshot.daily[i];
        tempTrend[i] = static_cast<int16_t>((d.tempMaxC + d.tempMinC) / 2);

        uint8_t humidity = d.humidityPct;
        if (humidity == 0 && d.cloudPct > 0) {
            humidity = d.cloudPct;
        }
        humidityTrend[i] = static_cast<int16_t>(humidity);
        rainTrend[i] = static_cast<int16_t>(d.rainProbPct);

        String dayText = formatLocalDay(d.timestampUtc, city.snapshot.timezoneOffsetSec);
        char dayInitial = '-';
        for (size_t c = 0; c < dayText.length(); ++c) {
            char ch = dayText[c];
            const bool isUpper = (ch >= 'A' && ch <= 'Z');
            const bool isLower = (ch >= 'a' && ch <= 'z');
            if (!isUpper && !isLower) {
                continue;
            }
            if (isLower) {
                ch = static_cast<char>(ch - ('a' - 'A'));
            }
            dayInitial = ch;
            break;
        }
        dayLabels[i] = String(dayInitial);
    }

    int16_t tempAxisMin = 0;
    int16_t tempAxisMax = 1;
    computeSeriesRange(tempTrend, DAILY_FORECAST_COUNT, 6, tempAxisMin, tempAxisMax);

    const int32_t trendLeft = 20;
    const int32_t trendTop = DAILY_BOX_Y + 28;
    const int32_t trendWidth = EPD_WIDTH - 40;
    const int32_t trendHeight = DAILY_BOX_H - 32;
    const int32_t trendGap = 10;
    const int32_t trendPanelW = (trendWidth - trendGap * 2) / 3;

    drawTrendChart(trendLeft,
                   trendTop,
                   trendPanelW,
                   trendHeight,
                   "Temp (C)",
                   tempTrend,
                   DAILY_FORECAST_COUNT,
                   tempAxisMin,
                   tempAxisMax,
                   "C",
                   dayLabels);

    drawTrendChart(trendLeft + trendPanelW + trendGap,
                   trendTop,
                   trendPanelW,
                   trendHeight,
                   "Humidity (%)",
                   humidityTrend,
                   DAILY_FORECAST_COUNT,
                   0,
                   100,
                   "%",
                   dayLabels);

    drawTrendChart(trendLeft + (trendPanelW + trendGap) * 2,
                   trendTop,
                   trendPanelW,
                   trendHeight,
                   "Rain (%)",
                   rainTrend,
                   DAILY_FORECAST_COUNT,
                   0,
                   100,
                   "%",
                   dayLabels);

    drawText(&OpenSans12B, PREV_BTN_X + 56, FOOTER_BOX_Y + 22, "< Prev");
    drawText(&OpenSans12B, NEXT_BTN_X + 56, FOOTER_BOX_Y + 22, "Next >");

    drawText(&OpenSans12B, (EPD_WIDTH / 2) - 30, FOOTER_BOX_Y + 22,
             String(cityIndex + 1) + "/" + String(cityCount));

    pushToDisplay(forceFullRefresh);
}

void DisplayRenderer::renderStatusMessage(const String &title, const String &detail, bool forceFullRefresh)
{
    clearFramebuffer();
    epd_draw_rect(20, 20, EPD_WIDTH - 40, EPD_HEIGHT - 40, BLACK, _framebuffer);

    drawText(&OpenSans18B, 40, 120, title);
    drawText(&OpenSans12B, 40, 190, detail);

    (void)forceFullRefresh;
    pushToDisplay(true);
}

void DisplayRenderer::clearFramebuffer()
{
    if (_framebuffer != nullptr) {
        memset(_framebuffer, WHITE, EPD_WIDTH * EPD_HEIGHT / 2);
    }
}

void DisplayRenderer::pushToDisplay(bool forceFullRefresh)
{
    if (_framebuffer == nullptr) {
        return;
    }

    epd_poweron();
    if (forceFullRefresh) {
        epd_clear();
    }
    epd_draw_grayscale_image(epd_full_screen(), _framebuffer);
    epd_poweroff();
}

void DisplayRenderer::drawText(const GFXfont *font, int32_t x, int32_t y, const String &text)
{
    if (_framebuffer == nullptr) {
        return;
    }

    int32_t cx = x;
    int32_t cy = y;
    write_string(font, text.c_str(), &cx, &cy, _framebuffer);
}

void DisplayRenderer::drawTrendChart(int32_t panelX,
                                     int32_t panelY,
                                     int32_t panelW,
                                     int32_t panelH,
                                     const String &title,
                                     const int16_t *values,
                                     size_t count,
                                     int16_t axisMin,
                                     int16_t axisMax,
                                     const String &unitSuffix,
                                     const String *xLabels)
{
    if (_framebuffer == nullptr || values == nullptr || count == 0) {
        return;
    }

    if (panelW < 80 || panelH < 50) {
        return;
    }

    epd_draw_rect(panelX, panelY, panelW, panelH, BLACK, _framebuffer);

    drawText(&OpenSans10B,
             panelX + (panelW - textWidth(&OpenSans10B, title)) / 2,
             panelY + 9,
             title);

    const int32_t axisLabelW = 30;
    const int32_t plotX = panelX + axisLabelW;
    const int32_t plotY = panelY + 16;
    const int32_t plotW = panelW - axisLabelW - 6;
    const int32_t plotH = panelH - 32;

    if (plotW < 10 || plotH < 10) {
        return;
    }

    epd_draw_rect(plotX, plotY, plotW, plotH, BLACK, _framebuffer);
    for (int i = 1; i < 4; ++i) {
        const int32_t gridY = plotY + (plotH * i) / 4;
        drawDottedHorizontalLine(_framebuffer, plotX + 1, plotX + plotW - 2, gridY);
    }

    int16_t minValue = axisMin;
    int16_t maxValue = axisMax;
    if (maxValue <= minValue) {
        maxValue = static_cast<int16_t>(minValue + 1);
    }

    const int16_t midValue = static_cast<int16_t>(minValue + (maxValue - minValue) / 2);

    drawText(&OpenSans10B, panelX + 1, plotY + 7, String(maxValue) + unitSuffix);
    drawText(&OpenSans10B, panelX + 1, plotY + (plotH / 2) + 3, String(midValue) + unitSuffix);
    drawText(&OpenSans10B, panelX + 1, plotY + plotH - 4, String(minValue) + unitSuffix);

    const int32_t denom = static_cast<int32_t>(maxValue - minValue);
    int32_t prevX = 0;
    int32_t prevY = 0;
    bool havePrev = false;

    for (size_t i = 0; i < count; ++i) {
        int32_t value = values[i];
        if (value < minValue) {
            value = minValue;
        } else if (value > maxValue) {
            value = maxValue;
        }

        int32_t pointX = plotX + (plotW / 2);
        if (count > 1) {
            pointX = plotX + static_cast<int32_t>((static_cast<int32_t>(i) * (plotW - 1)) / static_cast<int32_t>(count - 1));
        }

        const int32_t scaled = (value - minValue) * (plotH - 1);
        const int32_t pointY = plotY + (plotH - 1) - ((denom > 0) ? (scaled / denom) : 0);

        if (havePrev) {
            epd_draw_line(prevX, prevY, pointX, pointY, BLACK, _framebuffer);
        }
        epd_draw_line(pointX, pointY, pointX, pointY, BLACK, _framebuffer);

        prevX = pointX;
        prevY = pointY;
        havePrev = true;
    }

    if (xLabels != nullptr) {
        const int32_t labelsY = panelY + panelH - 3;
        for (size_t i = 0; i < count; ++i) {
            int32_t labelX = plotX + (plotW / 2);
            if (count > 1) {
                labelX = plotX + static_cast<int32_t>((static_cast<int32_t>(i) * (plotW - 1)) / static_cast<int32_t>(count - 1));
            }

            String label = xLabels[i];
            if (label.isEmpty()) {
                label = "-";
            }

            const int32_t labelW = textWidth(&OpenSans10B, label);
            int32_t drawX = labelX - (labelW / 2);
            if (drawX < plotX) {
                drawX = plotX;
            }
            const int32_t maxX = plotX + plotW - labelW;
            if (drawX > maxX) {
                drawX = maxX;
            }

            drawText(&OpenSans10B, drawX, labelsY, label);
        }
    }
}

void DisplayRenderer::drawSectionFrames()
{
    // Intentionally left blank: section border lines are hidden.
}

void DisplayRenderer::drawWeatherIcon(int16_t weatherId, bool isDaylight, int32_t centerX, int32_t centerY, int32_t size, bool suppressMoon, bool forceSmallStyle)
{
    if (_framebuffer == nullptr) {
        return;
    }

    const float scaleFactor = (size > 0) ? (static_cast<float>(size) / 68.0f) : 1.0f;
    const bool largeIcon = true;
    const bool smallIcon = false;
    int small = static_cast<int>(10 * scaleFactor);
    int large = static_cast<int>(20 * scaleFactor);
    if (small < 6) {
        small = 6;
    }
    if (large < 12) {
        large = 12;
    }

    auto drawIconText = [&](const GFXfont *font, int32_t x, int32_t y, const String &text) {
        int32_t cx = x;
        int32_t cy = y;
        write_string(font, text.c_str(), &cx, &cy, _framebuffer);
    };

    auto drawAngledLine = [&](int x, int y, int x1, int y1, int lineSize) {
        const float dx = static_cast<float>(x - x1);
        const float dy = static_cast<float>(y - y1);
        const float denom = sqrtf(dx * dx + dy * dy);
        if (denom < 0.001f) {
            return;
        }

        const int px = static_cast<int>((lineSize / 2.0f) * dx / denom);
        const int py = static_cast<int>((lineSize / 2.0f) * dy / denom);

        epd_fill_triangle(x + px, y - py, x - px, y + py, x1 + px, y1 - py, BLACK, _framebuffer);
        epd_fill_triangle(x - px, y + py, x1 - px, y1 + py, x1 + px, y1 - py, BLACK, _framebuffer);
    };

    auto addCloud = [&](int x, int y, int scale, int lineSize) {
        epd_fill_circle(x - scale * 3, y, scale, BLACK, _framebuffer);
        epd_fill_circle(x + scale * 3, y, scale, BLACK, _framebuffer);
        epd_fill_circle(x - scale, y - scale, static_cast<int>(scale * 1.4f), BLACK, _framebuffer);
        epd_fill_circle(x + static_cast<int>(scale * 1.5f), y - static_cast<int>(scale * 1.3f),
                        static_cast<int>(scale * 1.75f), BLACK, _framebuffer);
        epd_fill_rect(x - scale * 3 - 1, y - scale, scale * 6, scale * 2 + 1, BLACK, _framebuffer);

        epd_fill_circle(x - scale * 3, y, scale - lineSize, WHITE, _framebuffer);
        epd_fill_circle(x + scale * 3, y, scale - lineSize, WHITE, _framebuffer);
        epd_fill_circle(x - scale, y - scale, static_cast<int>(scale * 1.4f) - lineSize, WHITE, _framebuffer);
        epd_fill_circle(x + static_cast<int>(scale * 1.5f), y - static_cast<int>(scale * 1.3f),
                        static_cast<int>(scale * 1.75f) - lineSize, WHITE, _framebuffer);
        epd_fill_rect(x - scale * 3 + 2, y - scale + lineSize - 1,
                      static_cast<int>(scale * 5.9f), scale * 2 - lineSize * 2 + 2, WHITE, _framebuffer);
    };

    auto addRain = [&](int x, int y, int, bool iconSize) {
        if (iconSize == smallIcon) {
            drawIconText(&OpenSans10B, x - static_cast<int>(25 * scaleFactor), y + static_cast<int>(12 * scaleFactor), "/////");
        } else {
            drawIconText(&OpenSans18B, x - static_cast<int>(60 * scaleFactor), y + static_cast<int>(25 * scaleFactor), "///////");
        }
    };

    auto addSnow = [&](int x, int y, int, bool iconSize) {
        if (iconSize == smallIcon) {
            drawIconText(&OpenSans10B, x - static_cast<int>(25 * scaleFactor), y + static_cast<int>(15 * scaleFactor), "* * *");
        } else {
            drawIconText(&OpenSans18B, x - static_cast<int>(60 * scaleFactor), y + static_cast<int>(30 * scaleFactor), "* * * *");
        }
    };

    auto addTstorm = [&](int x, int y, int scale) {
        y = y + scale / 2;
        for (int i = 1; i < 5; i++) {
            epd_draw_line(x - scale * 4 + static_cast<int>(scale * i * 1.5f), y + static_cast<int>(scale * 1.5f),
                          x - static_cast<int>(scale * 3.5f) + static_cast<int>(scale * i * 1.5f), y + scale,
                          BLACK, _framebuffer);
            epd_draw_line(x - scale * 4 + static_cast<int>(scale * i * 1.5f), y + static_cast<int>(scale * 1.5f) + 1,
                          x - static_cast<int>(scale * 3.5f) + static_cast<int>(scale * i * 1.5f), y + scale + 1,
                          BLACK, _framebuffer);
            epd_draw_line(x - static_cast<int>(scale * 3.5f) + static_cast<int>(scale * i * 1.4f), y + static_cast<int>(scale * 2.5f),
                          x - scale * 3 + static_cast<int>(scale * i * 1.5f), y + static_cast<int>(scale * 1.5f),
                          BLACK, _framebuffer);
        }
    };

    auto addSun = [&](int x, int y, int scale, bool) {
        const int lineSize = static_cast<int>(5 * scaleFactor);
        epd_fill_rect(x - scale * 2, y, scale * 4, lineSize, BLACK, _framebuffer);
        epd_fill_rect(x, y - scale * 2, lineSize, scale * 4, BLACK, _framebuffer);
        drawAngledLine(x + static_cast<int>(scale * 1.4f), y + static_cast<int>(scale * 1.4f),
                       x - static_cast<int>(scale * 1.4f), y - static_cast<int>(scale * 1.4f),
                       static_cast<int>(lineSize * 1.5f));
        drawAngledLine(x - static_cast<int>(scale * 1.4f), y + static_cast<int>(scale * 1.4f),
                       x + static_cast<int>(scale * 1.4f), y - static_cast<int>(scale * 1.4f),
                       static_cast<int>(lineSize * 1.5f));

        epd_fill_circle(x, y, static_cast<int>(scale * 1.3f), WHITE, _framebuffer);
        epd_fill_circle(x, y, scale, BLACK, _framebuffer);
        epd_fill_circle(x, y, scale - lineSize, WHITE, _framebuffer);
    };

    auto addFog = [&](int x, int y, int scale, int lineSize, bool iconSize) {
        if (iconSize == smallIcon) {
            lineSize = static_cast<int>(3 * scaleFactor);
        }
        epd_fill_rect(x - scale * 3, y + static_cast<int>(scale * 1.5f), scale * 6, lineSize, BLACK, _framebuffer);
        epd_fill_rect(x - scale * 3, y + static_cast<int>(scale * 2.0f), scale * 6, lineSize, BLACK, _framebuffer);
        epd_fill_rect(x - scale * 3, y + static_cast<int>(scale * 2.5f), scale * 6, lineSize, BLACK, _framebuffer);
    };

    auto addMoon = [&](int x, int y, bool iconSize) {
        int xOffset = static_cast<int>(65 * scaleFactor);
        int yOffset = static_cast<int>(12 * scaleFactor);
        if (iconSize == largeIcon) {
            xOffset = static_cast<int>(130 * scaleFactor);
            yOffset = static_cast<int>(-40 * scaleFactor);
        }

        epd_fill_circle(x - static_cast<int>(28 * scaleFactor) + xOffset,
                        y - static_cast<int>(37 * scaleFactor) + yOffset,
                        static_cast<int>(small * 1.0f), BLACK, _framebuffer);
        epd_fill_circle(x - static_cast<int>(16 * scaleFactor) + xOffset,
                        y - static_cast<int>(37 * scaleFactor) + yOffset,
                        static_cast<int>(small * 1.6f), WHITE, _framebuffer);
    };

    auto clearSky = [&](int x, int y, bool iconSize, const String &iconName) {
        int scale = small;
        const bool nightIcon = iconName.endsWith("n");
        if (nightIcon && !suppressMoon) {
            addMoon(x, y, iconSize);
        }
        if (iconSize == largeIcon) {
            scale = large;
        }
        y += (iconSize ? 0 : static_cast<int>(10 * scaleFactor));
        if (!nightIcon) {
            addSun(x, y, static_cast<int>(scale * (iconSize ? 1.7f : 1.2f)), iconSize);
        }
    };

    auto brokenClouds = [&](int x, int y, bool iconSize, const String &iconName) {
        int scale = small;
        int lineSize = static_cast<int>(5 * scaleFactor);
        const bool nightIcon = iconName.endsWith("n");
        if (nightIcon && !suppressMoon) {
            addMoon(x, y, iconSize);
        }
        y += static_cast<int>(15 * scaleFactor);
        if (iconSize == largeIcon) {
            scale = large;
        }
        if (!nightIcon) {
            addSun(x - static_cast<int>(scale * 1.8f), y - static_cast<int>(scale * 1.8f), scale, iconSize);
        }
        addCloud(x, y, static_cast<int>(scale * (iconSize ? 1.0f : 0.75f)), lineSize);
    };

    auto fewClouds = [&](int x, int y, bool iconSize, const String &iconName) {
        int scale = small;
        int lineSize = static_cast<int>(5 * scaleFactor);
        const bool nightIcon = iconName.endsWith("n");
        if (nightIcon && !suppressMoon) {
            addMoon(x, y, iconSize);
        }
        y += static_cast<int>(15 * scaleFactor);
        if (iconSize == largeIcon) {
            scale = large;
        }

        const int xShift = iconSize ? static_cast<int>(10 * scaleFactor) : 0;
        addCloud(x + xShift, y, static_cast<int>(scale * (iconSize ? 0.9f : 0.8f)), lineSize);
        if (!nightIcon) {
            addSun((x + xShift) - static_cast<int>(scale * 1.8f), y - static_cast<int>(scale * 1.6f), scale, iconSize);
        }
    };

    auto scatteredClouds = [&](int x, int y, bool iconSize, const String &iconName) {
        int scale = small;
        int lineSize = static_cast<int>(5 * scaleFactor);
        const bool nightIcon = iconName.endsWith("n");
        if (nightIcon && !suppressMoon) {
            addMoon(x, y, iconSize);
        }
        y += static_cast<int>(15 * scaleFactor);
        if (iconSize == largeIcon) {
            scale = large;
        }

        if (!nightIcon) {
            addCloud(x - (iconSize ? static_cast<int>(35 * scaleFactor) : 0),
                     static_cast<int>(y * (iconSize ? 0.75f : 0.93f)),
                     scale / 2,
                     lineSize);
        }
        addCloud(x, y, static_cast<int>(scale * 0.9f), lineSize);
    };

    auto rain = [&](int x, int y, bool iconSize, const String &iconName) {
        int scale = small;
        int lineSize = static_cast<int>(5 * scaleFactor);
        if (iconName.endsWith("n") && !suppressMoon) {
            addMoon(x, y, iconSize);
        }
        y += static_cast<int>(15 * scaleFactor);
        if (iconSize == largeIcon) {
            scale = large;
        }
        addCloud(x, y, static_cast<int>(scale * (iconSize ? 1.0f : 0.75f)), lineSize);
        addRain(x, y, scale, iconSize);
    };

    auto chanceRain = [&](int x, int y, bool iconSize, const String &iconName) {
        int scale = small;
        int lineSize = static_cast<int>(5 * scaleFactor);
        const bool nightIcon = iconName.endsWith("n");
        if (nightIcon && !suppressMoon) {
            addMoon(x, y, iconSize);
        }
        if (iconSize == largeIcon) {
            scale = large;
        }
        y += static_cast<int>(15 * scaleFactor);

        if (!nightIcon) {
            addSun(x - static_cast<int>(scale * 1.8f), y - static_cast<int>(scale * 1.8f), scale, iconSize);
        }
        addCloud(x, y, static_cast<int>(scale * (iconSize ? 1.0f : 0.65f)), lineSize);
        addRain(x, y, scale, iconSize);
    };

    auto thunderstorms = [&](int x, int y, bool iconSize, const String &iconName) {
        int scale = small;
        int lineSize = static_cast<int>(5 * scaleFactor);
        if (iconName.endsWith("n") && !suppressMoon) {
            addMoon(x, y, iconSize);
        }
        if (iconSize == largeIcon) {
            scale = large;
        }
        y += static_cast<int>(5 * scaleFactor);

        addCloud(x, y, static_cast<int>(scale * (iconSize ? 1.0f : 0.75f)), lineSize);
        addTstorm(x, y, scale);
    };

    auto snow = [&](int x, int y, bool iconSize, const String &iconName) {
        int scale = small;
        int lineSize = static_cast<int>(5 * scaleFactor);
        if (iconName.endsWith("n") && !suppressMoon) {
            addMoon(x, y, iconSize);
        }
        if (iconSize == largeIcon) {
            scale = large;
        }
        addCloud(x, y, static_cast<int>(scale * (iconSize ? 1.0f : 0.75f)), lineSize);
        addSnow(x, y, scale, iconSize);
    };

    auto mist = [&](int x, int y, bool iconSize, const String &iconName) {
        int scale = small;
        int lineSize = static_cast<int>(5 * scaleFactor);
        const bool nightIcon = iconName.endsWith("n");
        if (nightIcon && !suppressMoon) {
            addMoon(x, y, iconSize);
        }
        if (iconSize == largeIcon) {
            scale = large;
        }
        if (!nightIcon) {
            addSun(x, y, static_cast<int>(scale * (iconSize ? 1.0f : 0.75f)), iconSize);
        }
        addFog(x, y, scale, lineSize, iconSize);
    };

    auto noData = [&](int x, int y, bool iconSize, const String &) {
        if (iconSize == largeIcon) {
            drawIconText(&OpenSans18B, x - static_cast<int>(10 * scaleFactor), y + static_cast<int>(10 * scaleFactor), "?");
        } else {
            drawIconText(&OpenSans12B, x - static_cast<int>(3 * scaleFactor), y - static_cast<int>(10 * scaleFactor), "?");
        }
    };

    const String iconName = iconCodeFromWeatherId(weatherId, isDaylight);
    const int x = centerX;
    const int y = centerY;
    const bool useLargeIcon = !forceSmallStyle && size >= 48;

    if (iconName == "01d" || iconName == "01n") {
        clearSky(x, y, useLargeIcon, iconName);
    } else if (iconName == "02d" || iconName == "02n") {
        fewClouds(x, y, useLargeIcon, iconName);
    } else if (iconName == "03d" || iconName == "03n") {
        scatteredClouds(x, y, useLargeIcon, iconName);
    } else if (iconName == "04d" || iconName == "04n") {
        brokenClouds(x, y, useLargeIcon, iconName);
    } else if (iconName == "09d" || iconName == "09n") {
        chanceRain(x, y, useLargeIcon, iconName);
    } else if (iconName == "10d" || iconName == "10n") {
        rain(x, y, useLargeIcon, iconName);
    } else if (iconName == "11d" || iconName == "11n") {
        thunderstorms(x, y, useLargeIcon, iconName);
    } else if (iconName == "13d" || iconName == "13n") {
        snow(x, y, useLargeIcon, iconName);
    } else if (iconName == "50d" || iconName == "50n") {
        mist(x, y, useLargeIcon, iconName);
    } else {
        noData(x, y, useLargeIcon, iconName);
    }
}

String DisplayRenderer::formatLocalTime(int64_t utcEpoch, int32_t offsetSeconds) const
{
    return formatLocalEpoch(utcEpoch, offsetSeconds, "%H:%M");
}

String DisplayRenderer::formatLocalDay(int64_t utcEpoch, int32_t offsetSeconds) const
{
    return formatLocalEpoch(utcEpoch, offsetSeconds, "%a %d");
}





























