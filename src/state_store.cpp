#include "state_store.h"

#include <ArduinoJson.h>
#include <FS.h>
#include <SPIFFS.h>

namespace {

constexpr const char *PREF_NAMESPACE = "weather_app";
constexpr const char *KEY_ACTIVE_CITY = "active_city";
constexpr const char *KEY_ROUND_ROBIN = "round_robin";
constexpr const char *KEY_REFRESH_COUNT = "refresh_count";

uint8_t clampPct(int value)
{
    if (value < 0) {
        return 0;
    }
    if (value > 100) {
        return 100;
    }
    return static_cast<uint8_t>(value);
}

} // namespace

bool StateStore::begin()
{
    if (!SPIFFS.begin(true)) {
        return false;
    }

    return _prefs.begin(PREF_NAMESPACE, false);
}

bool StateStore::loadActiveCityIndex(uint8_t &index)
{
    if (!_prefs.isKey(KEY_ACTIVE_CITY)) {
        return false;
    }
    index = static_cast<uint8_t>(_prefs.getUChar(KEY_ACTIVE_CITY, 0));
    return true;
}

void StateStore::saveActiveCityIndex(uint8_t index)
{
    _prefs.putUChar(KEY_ACTIVE_CITY, index);
}

bool StateStore::loadRoundRobinIndex(uint8_t &index)
{
    if (!_prefs.isKey(KEY_ROUND_ROBIN)) {
        return false;
    }
    index = static_cast<uint8_t>(_prefs.getUChar(KEY_ROUND_ROBIN, 0));
    return true;
}

void StateStore::saveRoundRobinIndex(uint8_t index)
{
    _prefs.putUChar(KEY_ROUND_ROBIN, index);
}

bool StateStore::loadRefreshCounter(uint32_t &counter)
{
    if (!_prefs.isKey(KEY_REFRESH_COUNT)) {
        return false;
    }
    counter = _prefs.getUInt(KEY_REFRESH_COUNT, 0);
    return true;
}

void StateStore::saveRefreshCounter(uint32_t counter)
{
    _prefs.putUInt(KEY_REFRESH_COUNT, counter);
}

bool StateStore::loadCityState(uint8_t cityIndex, CityState &state)
{
    const String path = cityPath(cityIndex);
    if (!SPIFFS.exists(path)) {
        return false;
    }

    File file = SPIFFS.open(path, FILE_READ);
    if (!file) {
        return false;
    }

    DynamicJsonDocument doc(24576);
    const DeserializationError error = deserializeJson(doc, file);
    file.close();

    if (error) {
        return false;
    }

    state.query = doc["query"].as<String>();
    state.resolvedName = doc["resolved_name"].as<String>();
    state.latitude = doc["lat"] | 0.0f;
    state.longitude = doc["lon"] | 0.0f;
    state.hasCoordinates = doc["has_coordinates"] | false;
    state.lastUpdatedEpoch = doc["last_updated"] | 0;

    JsonObjectConst snap = doc["snapshot"];
    state.snapshot.valid = snap["valid"] | false;
    state.snapshot.stale = snap["stale"] | true;
    state.snapshot.fetchedAtEpoch = snap["fetched"] | 0;
    state.snapshot.nowEpochUtc = snap["now"] | 0;
    state.snapshot.sunriseEpochUtc = snap["sunrise"] | 0;
    state.snapshot.sunsetEpochUtc = snap["sunset"] | 0;
    state.snapshot.timezoneOffsetSec = snap["tz_offset"] | 0;
    state.snapshot.humidityPct = clampPct(snap["humidity"] | 0);
    state.snapshot.currentTempC = snap["current_temp"] | 0;
    state.snapshot.currentWeatherId = snap["current_weather"] | 0;

    JsonArrayConst hourly = snap["hourly"];
    for (size_t i = 0; i < HOURLY_FORECAST_COUNT; ++i) {
        if (i < hourly.size()) {
            JsonObjectConst item = hourly[i];
            state.snapshot.hourly[i].timestampUtc = item["ts"] | 0;
            state.snapshot.hourly[i].temperatureC = item["temp"] | 0;
            state.snapshot.hourly[i].rainProbPct = clampPct(item["rain"] | 0);
            state.snapshot.hourly[i].cloudPct = clampPct(item["cloud"] | 0);
            state.snapshot.hourly[i].weatherId = item["weather"] | 0;
        } else {
            state.snapshot.hourly[i] = HourlyForecast{};
        }
    }

    JsonArrayConst daily = snap["daily"];
    for (size_t i = 0; i < DAILY_FORECAST_COUNT; ++i) {
        if (i < daily.size()) {
            JsonObjectConst item = daily[i];
            state.snapshot.daily[i].timestampUtc = item["ts"] | 0;
            state.snapshot.daily[i].tempMinC = item["min"] | 0;
            state.snapshot.daily[i].tempMaxC = item["max"] | 0;
            state.snapshot.daily[i].rainProbPct = clampPct(item["rain"] | 0);
            state.snapshot.daily[i].cloudPct = clampPct(item["cloud"] | 0);
            state.snapshot.daily[i].humidityPct = clampPct(item["humidity"] | state.snapshot.daily[i].cloudPct);
            state.snapshot.daily[i].weatherId = item["weather"] | 0;
        } else {
            state.snapshot.daily[i] = DailyForecast{};
        }
    }

    return true;
}

bool StateStore::saveCityState(uint8_t cityIndex, const CityState &state)
{
    DynamicJsonDocument doc(24576);
    doc["query"] = state.query;
    doc["resolved_name"] = state.resolvedName;
    doc["lat"] = state.latitude;
    doc["lon"] = state.longitude;
    doc["has_coordinates"] = state.hasCoordinates;
    doc["last_updated"] = state.lastUpdatedEpoch;

    JsonObject snap = doc.createNestedObject("snapshot");
    snap["valid"] = state.snapshot.valid;
    snap["stale"] = state.snapshot.stale;
    snap["fetched"] = state.snapshot.fetchedAtEpoch;
    snap["now"] = state.snapshot.nowEpochUtc;
    snap["sunrise"] = state.snapshot.sunriseEpochUtc;
    snap["sunset"] = state.snapshot.sunsetEpochUtc;
    snap["tz_offset"] = state.snapshot.timezoneOffsetSec;
    snap["humidity"] = state.snapshot.humidityPct;
    snap["current_temp"] = state.snapshot.currentTempC;
    snap["current_weather"] = state.snapshot.currentWeatherId;

    JsonArray hourly = snap.createNestedArray("hourly");
    for (size_t i = 0; i < HOURLY_FORECAST_COUNT; ++i) {
        JsonObject item = hourly.createNestedObject();
        item["ts"] = state.snapshot.hourly[i].timestampUtc;
        item["temp"] = state.snapshot.hourly[i].temperatureC;
        item["rain"] = state.snapshot.hourly[i].rainProbPct;
        item["cloud"] = state.snapshot.hourly[i].cloudPct;
        item["weather"] = state.snapshot.hourly[i].weatherId;
    }

    JsonArray daily = snap.createNestedArray("daily");
    for (size_t i = 0; i < DAILY_FORECAST_COUNT; ++i) {
        JsonObject item = daily.createNestedObject();
        item["ts"] = state.snapshot.daily[i].timestampUtc;
        item["min"] = state.snapshot.daily[i].tempMinC;
        item["max"] = state.snapshot.daily[i].tempMaxC;
        item["rain"] = state.snapshot.daily[i].rainProbPct;
        item["humidity"] = state.snapshot.daily[i].humidityPct;
        item["cloud"] = state.snapshot.daily[i].cloudPct;
        item["weather"] = state.snapshot.daily[i].weatherId;
    }

    const String path = cityPath(cityIndex);
    File file = SPIFFS.open(path, FILE_WRITE);
    if (!file) {
        return false;
    }

    const size_t written = serializeJson(doc, file);
    file.close();
    return written > 0;
}

String StateStore::cityPath(uint8_t cityIndex) const
{
    return "/city_" + String(cityIndex) + ".json";
}
