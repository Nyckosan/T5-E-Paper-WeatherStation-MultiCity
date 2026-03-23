#pragma once

#include <Arduino.h>

namespace AppConfig {

static constexpr uint8_t kMaxCities = 5;

// Fill non-empty city entries to enable them (e.g. "Paris,FR" or "Redmond,WA,US").
// Extra spaces around commas are ignored automatically ("Redmond, US" is accepted).
static constexpr const char *kCityQueries[kMaxCities] = {
    "Redmond,US",
    "Seoul,KR",
    "Antony,FR",
    "",
    ""
};

static constexpr uint32_t kUpdateIntervalSeconds = 3600;
static constexpr uint32_t kInteractionWindowMs = 120000;
static constexpr uint32_t kTouchDebounceMs = 350;
static constexpr uint32_t kStaleAfterSeconds = 5400;
static constexpr uint8_t kFullRefreshEveryNCycles = 6;

// Allow touch to wake the board between hourly timer refreshes.
static constexpr bool kEnableTouchWakeup = true;

static constexpr const char *kUnits = "metric";
static constexpr const char *kTimeFormat = "%H:%M";

static constexpr const char *kNtpServer1 = "pool.ntp.org";
static constexpr const char *kNtpServer2 = "time.nist.gov";

} // namespace AppConfig
