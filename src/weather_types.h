#pragma once

#include <Arduino.h>
#include <stdint.h>

static constexpr size_t HOURLY_FORECAST_COUNT = 24;
static constexpr size_t DAILY_FORECAST_COUNT = 7;

struct HourlyForecast {
    int64_t timestampUtc = 0;
    int16_t temperatureC = 0;
    uint8_t rainProbPct = 0;
    uint8_t cloudPct = 0;
    int16_t weatherId = 0;
};

struct DailyForecast {
    int64_t timestampUtc = 0;
    int16_t tempMinC = 0;
    int16_t tempMaxC = 0;
    uint8_t rainProbPct = 0;
    uint8_t humidityPct = 0;
    uint8_t cloudPct = 0;
    int16_t weatherId = 0;
};

struct WeatherSnapshot {
    bool valid = false;
    bool stale = true;
    int64_t fetchedAtEpoch = 0;
    int64_t nowEpochUtc = 0;
    int64_t sunriseEpochUtc = 0;
    int64_t sunsetEpochUtc = 0;
    int32_t timezoneOffsetSec = 0;
    uint8_t humidityPct = 0;
    int16_t currentTempC = 0;
    int16_t currentWeatherId = 0;
    HourlyForecast hourly[HOURLY_FORECAST_COUNT];
    DailyForecast daily[DAILY_FORECAST_COUNT];
};

struct CityState {
    String query;
    String resolvedName;
    String lastError;
    float latitude = 0.0f;
    float longitude = 0.0f;
    bool hasCoordinates = false;
    uint32_t lastUpdatedEpoch = 0;
    WeatherSnapshot snapshot;
};
