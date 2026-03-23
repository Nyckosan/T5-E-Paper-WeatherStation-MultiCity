#include "weather_client.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <math.h>
#include <time.h>

namespace {

constexpr uint32_t HTTP_TIMEOUT_MS = 15000;
constexpr uint8_t GEO_PRIMARY_LIMIT = 5;
constexpr uint8_t GEO_FALLBACK_LIMIT = 8;
constexpr int16_t TEMP_INIT_MAX = 32767;
constexpr int16_t TEMP_INIT_MIN = -32768;

template <typename T>
T jsonOr(const JsonVariantConst &value, T fallback)
{
    if (value.isNull()) {
        return fallback;
    }
    return value.as<T>();
}

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

int16_t toRoundedTemp(float value)
{
    return static_cast<int16_t>(lroundf(value));
}

char asciiUpperChar(char c)
{
    if (c >= 'a' && c <= 'z') {
        return static_cast<char>(c - ('a' - 'A'));
    }
    return c;
}

bool isAsciiAlpha(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

String asciiUpper(String value)
{
    for (size_t i = 0; i < value.length(); ++i) {
        value.setCharAt(i, asciiUpperChar(value[i]));
    }
    return value;
}

String normalizeGeoQuery(String query)
{
    query.trim();
    if (query.isEmpty()) {
        return query;
    }

    String normalized;
    normalized.reserve(query.length());

    int start = 0;
    while (start <= query.length()) {
        const int commaIndex = query.indexOf(',', start);
        const int end = (commaIndex < 0) ? query.length() : commaIndex;

        String part = query.substring(start, end);
        part.trim();

        if (!part.isEmpty()) {
            if (!normalized.isEmpty()) {
                normalized += ',';
            }
            normalized += part;
        }

        if (commaIndex < 0) {
            break;
        }

        start = commaIndex + 1;
    }

    return normalized;
}

String endpointFromUrl(const String &url)
{
    int start = 0;
    const int scheme = url.indexOf("://");
    if (scheme >= 0) {
        const int hostStart = scheme + 3;
        const int pathStart = url.indexOf('/', hostStart);
        start = (pathStart >= 0) ? pathStart : hostStart;
    } else {
        const int pathStart = url.indexOf('/');
        start = (pathStart >= 0) ? pathStart : 0;
    }

    int end = url.indexOf('?', start);
    if (end < 0) {
        end = url.length();
    }

    return url.substring(start, end);
}

struct GeoQueryCriteria {
    String normalizedQuery;
    String city;
    String stateCode;
    String countryCode;
};

GeoQueryCriteria parseGeoQuery(const String &rawQuery)
{
    GeoQueryCriteria criteria;
    criteria.normalizedQuery = normalizeGeoQuery(rawQuery);

    if (criteria.normalizedQuery.isEmpty()) {
        return criteria;
    }

    const int firstComma = criteria.normalizedQuery.indexOf(',');
    if (firstComma < 0) {
        criteria.city = criteria.normalizedQuery;
        return criteria;
    }

    criteria.city = criteria.normalizedQuery.substring(0, firstComma);
    criteria.city.trim();

    const int secondComma = criteria.normalizedQuery.indexOf(',', firstComma + 1);

    String second = (secondComma < 0) ? criteria.normalizedQuery.substring(firstComma + 1)
                                      : criteria.normalizedQuery.substring(firstComma + 1, secondComma);
    second.trim();

    if (secondComma < 0) {
        const String secondUpper = asciiUpper(second);
        const bool looksLikeIso2Country =
            secondUpper.length() == 2 && isAsciiAlpha(secondUpper[0]) && isAsciiAlpha(secondUpper[1]);

        if (looksLikeIso2Country) {
            criteria.countryCode = secondUpper;
        } else {
            criteria.stateCode = secondUpper;
        }

        return criteria;
    }

    String third = criteria.normalizedQuery.substring(secondComma + 1);
    third.trim();

    criteria.stateCode = asciiUpper(second);
    criteria.countryCode = asciiUpper(third);
    return criteria;
}

int scoreGeoCandidate(const JsonObjectConst &item, const GeoQueryCriteria &criteria)
{
    int score = 0;

    const String name = jsonOr<const char *>(item["name"], "");
    const String state = asciiUpper(jsonOr<const char *>(item["state"], ""));
    const String country = asciiUpper(jsonOr<const char *>(item["country"], ""));

    if (!criteria.city.isEmpty() && name.equalsIgnoreCase(criteria.city)) {
        score += 6;
    }

    if (!criteria.countryCode.isEmpty()) {
        if (country.equals(criteria.countryCode)) {
            score += 30;
        } else {
            score -= 100;
        }
    }

    if (!criteria.stateCode.isEmpty()) {
        if (state.equals(criteria.stateCode)) {
            score += 15;
        } else {
            score -= 40;
        }
    }

    return score;
}

String summarizeHttpErrorBody(String body)
{
    body.trim();
    if (body.isEmpty()) {
        return body;
    }

    DynamicJsonDocument errDoc(1024);
    if (!deserializeJson(errDoc, body)) {
        const char *message = errDoc["message"] | "";
        if (message[0] != '\0') {
            body = String(message);
        }
    }

    body.replace('\r', ' ');
    body.replace('\n', ' ');
    body.trim();

    if (body.length() > 80) {
        return body.substring(0, 77) + "...";
    }

    return body;
}

bool startsWithHttp(const String &error, int code)
{
    return error.startsWith("HTTP " + String(code));
}

bool shouldTryLegacyOneCall(const String &error)
{
    return startsWithHttp(error, 401) || startsWithHttp(error, 403) || startsWithHttp(error, 404);
}

bool isAuthError(const String &error)
{
    return startsWithHttp(error, 401) || startsWithHttp(error, 403);
}

bool isGeoAuthError(const String &error)
{
    return isAuthError(error) && error.indexOf("/geo/1.0/direct") >= 0;
}

void fillFromForecastRow(const JsonObjectConst &row, HourlyForecast &entry)
{
    entry.temperatureC = toRoundedTemp(jsonOr<float>(row["main"]["temp"], 0.0f));
    entry.rainProbPct = clampPct(static_cast<int>(lroundf(jsonOr<float>(row["pop"], 0.0f) * 100.0f)));
    entry.cloudPct = clampPct(jsonOr<int>(row["clouds"]["all"], 0));
    entry.weatherId = jsonOr<int16_t>(row["weather"][0]["id"], 0);
}

} // namespace

WeatherClient::WeatherClient(const String &apiKey) : _apiKey(apiKey)
{
    _apiKey.trim();
}

const String &WeatherClient::lastError() const
{
    return _lastError;
}

bool WeatherClient::resolveCity(CityState &city)
{
    const GeoQueryCriteria criteria = parseGeoQuery(city.query);
    if (criteria.normalizedQuery.isEmpty()) {
        _lastError = "City query is empty";
        return false;
    }

    DynamicJsonDocument doc(12288);

    auto requestGeocode = [&](const String &query, uint8_t limit) {
        const String url = "https://api.openweathermap.org/geo/1.0/direct?q=" +
                           urlEncode(query) +
                           "&limit=" + String(limit) +
                           "&appid=" + urlEncode(_apiKey);
        return httpGetJson(url, doc);
    };

    auto requestWeatherByQuery = [&](const String &query) {
        DynamicJsonDocument weatherDoc(8192);
        const String url = "https://api.openweathermap.org/data/2.5/weather?q=" +
                           urlEncode(query) +
                           "&appid=" + urlEncode(_apiKey);

        if (!httpGetJson(url, weatherDoc)) {
            return false;
        }

        JsonObjectConst root = weatherDoc.as<JsonObjectConst>();
        if (root.isNull()) {
            _lastError = "Weather query response invalid";
            return false;
        }

        city.latitude = jsonOr<float>(root["coord"]["lat"], 0.0f);
        city.longitude = jsonOr<float>(root["coord"]["lon"], 0.0f);

        if (city.latitude == 0.0f && city.longitude == 0.0f) {
            _lastError = "Weather query returned no coordinates for " + city.query;
            return false;
        }

        const String name = jsonOr<const char *>(root["name"], "");
        const String country = jsonOr<const char *>(root["sys"]["country"], "");
        if (!name.isEmpty()) {
            city.resolvedName = country.isEmpty() ? name : (name + ", " + country);
        }

        city.hasCoordinates = true;
        return true;
    };

    if (!requestGeocode(criteria.normalizedQuery, GEO_PRIMARY_LIMIT)) {
        if (isGeoAuthError(_lastError) && requestWeatherByQuery(criteria.normalizedQuery)) {
            return true;
        }
        return false;
    }

    JsonArrayConst root = doc.as<JsonArrayConst>();
    if ((root.isNull() || root.size() == 0) && !criteria.countryCode.isEmpty() && !criteria.city.isEmpty()) {
        // Fallback for city+country queries where API may interpret the country token as state.
        if (!requestGeocode(criteria.city, GEO_FALLBACK_LIMIT)) {
            if (isGeoAuthError(_lastError) && requestWeatherByQuery(criteria.normalizedQuery)) {
                return true;
            }
            return false;
        }
        root = doc.as<JsonArrayConst>();
    }

    if (root.isNull() || root.size() == 0) {
        if (requestWeatherByQuery(criteria.normalizedQuery)) {
            return true;
        }
        _lastError = "Geocoding returned no results for " + city.query;
        return false;
    }

    int bestScore = -32768;
    size_t bestIndex = 0;

    for (size_t i = 0; i < root.size(); ++i) {
        const int score = scoreGeoCandidate(root[i].as<JsonObjectConst>(), criteria);
        if (score > bestScore) {
            bestScore = score;
            bestIndex = i;
        }
    }

    JsonObjectConst item = root[bestIndex];

    if (!criteria.countryCode.isEmpty()) {
        const String selectedCountry = asciiUpper(jsonOr<const char *>(item["country"], ""));
        if (!selectedCountry.equals(criteria.countryCode)) {
            _lastError = "No geocode match for " + city.query;
            return false;
        }
    }

    city.latitude = jsonOr<float>(item["lat"], 0.0f);
    city.longitude = jsonOr<float>(item["lon"], 0.0f);

    const String name = jsonOr<const char *>(item["name"], "");
    const String country = jsonOr<const char *>(item["country"], "");
    if (!name.isEmpty()) {
        city.resolvedName = country.isEmpty() ? name : (name + ", " + country);
    }

    city.hasCoordinates = true;
    return true;
}

bool WeatherClient::fetchCityWeather(CityState &city)
{
    _lastError = "";

    if (!city.hasCoordinates && !resolveCity(city)) {
        return false;
    }

    const String apiParams =
        "lat=" + String(city.latitude, 6) +
        "&lon=" + String(city.longitude, 6) +
        "&exclude=minutely,alerts&units=metric&appid=" + urlEncode(_apiKey);

    DynamicJsonDocument doc(98304);
    bool oneCallOk = false;

    const String oneCallV3Url = "https://api.openweathermap.org/data/3.0/onecall?" + apiParams;
    if (httpGetJson(oneCallV3Url, doc)) {
        oneCallOk = true;
    } else if (shouldTryLegacyOneCall(_lastError)) {
        const String oneCallV25Url = "https://api.openweathermap.org/data/2.5/onecall?" + apiParams;
        if (httpGetJson(oneCallV25Url, doc)) {
            oneCallOk = true;
        }
    }

    if (oneCallOk) {
        JsonObjectConst root = doc.as<JsonObjectConst>();
        if (root.isNull()) {
            _lastError = "Forecast response was not a JSON object";
            return false;
        }

        WeatherSnapshot snapshot;
        snapshot.valid = true;
        snapshot.stale = false;
        snapshot.timezoneOffsetSec = jsonOr<int32_t>(root["timezone_offset"], 0);

        const JsonObjectConst current = root["current"];
        snapshot.nowEpochUtc = jsonOr<int64_t>(current["dt"], static_cast<int64_t>(time(nullptr)));
        snapshot.fetchedAtEpoch = static_cast<int64_t>(time(nullptr));
        if (snapshot.fetchedAtEpoch <= 0) {
            snapshot.fetchedAtEpoch = snapshot.nowEpochUtc;
        }

        snapshot.humidityPct = clampPct(jsonOr<int>(current["humidity"], 0));
        snapshot.currentTempC = toRoundedTemp(jsonOr<float>(current["temp"], 0.0f));
        snapshot.currentWeatherId = jsonOr<int16_t>(current["weather"][0]["id"], 0);
        snapshot.sunriseEpochUtc = jsonOr<int64_t>(current["sunrise"], snapshot.nowEpochUtc);
        snapshot.sunsetEpochUtc = jsonOr<int64_t>(current["sunset"], snapshot.nowEpochUtc);

        JsonArrayConst hourly = root["hourly"];
        for (size_t i = 0; i < HOURLY_FORECAST_COUNT; ++i) {
            HourlyForecast &entry = snapshot.hourly[i];
            if (i < hourly.size()) {
                JsonObjectConst h = hourly[i];
                entry.timestampUtc = jsonOr<int64_t>(h["dt"], snapshot.nowEpochUtc + static_cast<int64_t>(i * 3600));
                entry.temperatureC = toRoundedTemp(jsonOr<float>(h["temp"], 0.0f));
                entry.rainProbPct = clampPct(static_cast<int>(lroundf(jsonOr<float>(h["pop"], 0.0f) * 100.0f)));
                entry.cloudPct = clampPct(jsonOr<int>(h["clouds"], 0));
                entry.weatherId = jsonOr<int16_t>(h["weather"][0]["id"], 0);
            } else {
                entry.timestampUtc = snapshot.nowEpochUtc + static_cast<int64_t>(i * 3600);
                entry.temperatureC = 0;
                entry.rainProbPct = 0;
                entry.cloudPct = 0;
                entry.weatherId = 0;
            }
        }

        JsonArrayConst daily = root["daily"];
        for (size_t i = 0; i < DAILY_FORECAST_COUNT; ++i) {
            DailyForecast &entry = snapshot.daily[i];
            if (i < daily.size()) {
                JsonObjectConst d = daily[i];
                entry.timestampUtc = jsonOr<int64_t>(d["dt"], snapshot.nowEpochUtc + static_cast<int64_t>(i * 86400));
                entry.tempMinC = toRoundedTemp(jsonOr<float>(d["temp"]["min"], 0.0f));
                entry.tempMaxC = toRoundedTemp(jsonOr<float>(d["temp"]["max"], 0.0f));
                entry.rainProbPct = clampPct(static_cast<int>(lroundf(jsonOr<float>(d["pop"], 0.0f) * 100.0f)));
                entry.cloudPct = clampPct(jsonOr<int>(d["clouds"], 0));
                entry.humidityPct = clampPct(jsonOr<int>(d["humidity"], entry.cloudPct));
                entry.weatherId = jsonOr<int16_t>(d["weather"][0]["id"], 0);
            } else {
                entry.timestampUtc = snapshot.nowEpochUtc + static_cast<int64_t>(i * 86400);
                entry.tempMinC = 0;
                entry.tempMaxC = 0;
                entry.rainProbPct = 0;
                entry.cloudPct = 0;
                entry.humidityPct = 0;
                entry.weatherId = 0;
            }
        }

        city.snapshot = snapshot;
        city.lastUpdatedEpoch = static_cast<uint32_t>(snapshot.fetchedAtEpoch);
        return true;
    }

    const String oneCallError = _lastError;
    if (!isAuthError(oneCallError)) {
        return false;
    }

    // Fallback for keys that are valid but not authorized for One Call.
    DynamicJsonDocument currentDoc(12288);
    const String currentUrl =
        "https://api.openweathermap.org/data/2.5/weather?lat=" + String(city.latitude, 6) +
        "&lon=" + String(city.longitude, 6) +
        "&units=metric&appid=" + urlEncode(_apiKey);

    if (!httpGetJson(currentUrl, currentDoc)) {
        _lastError = "OneCall auth failed; current: " + _lastError;
        return false;
    }

    DynamicJsonDocument forecastDoc(98304);
    const String forecastUrl =
        "https://api.openweathermap.org/data/2.5/forecast?lat=" + String(city.latitude, 6) +
        "&lon=" + String(city.longitude, 6) +
        "&units=metric&appid=" + urlEncode(_apiKey);

    if (!httpGetJson(forecastUrl, forecastDoc)) {
        _lastError = "OneCall auth failed; forecast: " + _lastError;
        return false;
    }

    JsonObjectConst currentRoot = currentDoc.as<JsonObjectConst>();
    JsonObjectConst forecastRoot = forecastDoc.as<JsonObjectConst>();
    JsonArrayConst forecastList = forecastRoot["list"];

    if (currentRoot.isNull() || forecastRoot.isNull() || forecastList.isNull() || forecastList.size() == 0) {
        _lastError = "Fallback forecast response invalid";
        return false;
    }

    WeatherSnapshot snapshot;
    snapshot.valid = true;
    snapshot.stale = false;
    snapshot.timezoneOffsetSec = jsonOr<int32_t>(currentRoot["timezone"], 0);
    snapshot.nowEpochUtc = jsonOr<int64_t>(currentRoot["dt"], static_cast<int64_t>(time(nullptr)));
    snapshot.fetchedAtEpoch = static_cast<int64_t>(time(nullptr));
    if (snapshot.fetchedAtEpoch <= 0) {
        snapshot.fetchedAtEpoch = snapshot.nowEpochUtc;
    }

    snapshot.humidityPct = clampPct(jsonOr<int>(currentRoot["main"]["humidity"], 0));
    snapshot.currentTempC = toRoundedTemp(jsonOr<float>(currentRoot["main"]["temp"], 0.0f));
    snapshot.currentWeatherId = jsonOr<int16_t>(currentRoot["weather"][0]["id"], 0);
    snapshot.sunriseEpochUtc = jsonOr<int64_t>(currentRoot["sys"]["sunrise"], snapshot.nowEpochUtc);
    snapshot.sunsetEpochUtc = jsonOr<int64_t>(currentRoot["sys"]["sunset"], snapshot.nowEpochUtc);

    for (size_t i = 0; i < HOURLY_FORECAST_COUNT; ++i) {
        const int64_t targetUtc = snapshot.nowEpochUtc + static_cast<int64_t>(i * 3600);

        size_t chosen = forecastList.size() - 1;
        for (size_t j = 0; j < forecastList.size(); ++j) {
            const int64_t dt = jsonOr<int64_t>(forecastList[j]["dt"], 0);
            if (dt >= targetUtc) {
                chosen = j;
                break;
            }
        }

        const JsonObjectConst row = forecastList[chosen];
        HourlyForecast &entry = snapshot.hourly[i];
        entry.timestampUtc = targetUtc;
        fillFromForecastRow(row, entry);
    }

    struct DayAgg {
        bool hasData = false;
        int16_t minC = TEMP_INIT_MAX;
        int16_t maxC = TEMP_INIT_MIN;
        uint8_t maxRainPct = 0;
        int cloudSum = 0;
        int cloudCount = 0;
        int humiditySum = 0;
        int humidityCount = 0;
        int16_t weatherId = 0;
    };

    DayAgg dayAgg[DAILY_FORECAST_COUNT];
    const int64_t localNow = snapshot.nowEpochUtc + snapshot.timezoneOffsetSec;
    const int64_t baseLocalDay = localNow / 86400;
    const int64_t baseDayStartUtc = (baseLocalDay * 86400) - snapshot.timezoneOffsetSec;

    for (size_t i = 0; i < forecastList.size(); ++i) {
        const JsonObjectConst row = forecastList[i];
        const int64_t dtUtc = jsonOr<int64_t>(row["dt"], 0);
        const int64_t localDay = (dtUtc + snapshot.timezoneOffsetSec) / 86400;
        const int dayIndex = static_cast<int>(localDay - baseLocalDay);
        if (dayIndex < 0 || dayIndex >= static_cast<int>(DAILY_FORECAST_COUNT)) {
            continue;
        }

        DayAgg &agg = dayAgg[dayIndex];
        const int16_t tempMinC = toRoundedTemp(jsonOr<float>(row["main"]["temp_min"], jsonOr<float>(row["main"]["temp"], 0.0f)));
        const int16_t tempMaxC = toRoundedTemp(jsonOr<float>(row["main"]["temp_max"], jsonOr<float>(row["main"]["temp"], 0.0f)));
        const uint8_t rainPct = clampPct(static_cast<int>(lroundf(jsonOr<float>(row["pop"], 0.0f) * 100.0f)));
        const uint8_t cloudPct = clampPct(jsonOr<int>(row["clouds"]["all"], 0));
        const uint8_t humidityPct = clampPct(jsonOr<int>(row["main"]["humidity"], cloudPct));
        const int16_t weatherId = jsonOr<int16_t>(row["weather"][0]["id"], 0);

        if (!agg.hasData) {
            agg.hasData = true;
            agg.minC = tempMinC;
            agg.maxC = tempMaxC;
            agg.weatherId = weatherId;
        } else {
            if (tempMinC < agg.minC) {
                agg.minC = tempMinC;
            }
            if (tempMaxC > agg.maxC) {
                agg.maxC = tempMaxC;
            }
        }

        if (rainPct > agg.maxRainPct) {
            agg.maxRainPct = rainPct;
        }

        agg.cloudSum += cloudPct;
        agg.cloudCount += 1;
        agg.humiditySum += humidityPct;
        agg.humidityCount += 1;
    }

    for (size_t i = 0; i < DAILY_FORECAST_COUNT; ++i) {
        DailyForecast &entry = snapshot.daily[i];
        entry.timestampUtc = baseDayStartUtc + static_cast<int64_t>(i * 86400);

        if (dayAgg[i].hasData) {
            entry.tempMinC = dayAgg[i].minC;
            entry.tempMaxC = dayAgg[i].maxC;
            entry.rainProbPct = dayAgg[i].maxRainPct;
            entry.cloudPct = (dayAgg[i].cloudCount > 0)
                                 ? clampPct(dayAgg[i].cloudSum / dayAgg[i].cloudCount)
                                 : 0;
            entry.humidityPct = (dayAgg[i].humidityCount > 0)
                                    ? clampPct(dayAgg[i].humiditySum / dayAgg[i].humidityCount)
                                    : entry.cloudPct;
            entry.weatherId = dayAgg[i].weatherId;
            continue;
        }

        if (i > 0) {
            entry = snapshot.daily[i - 1];
            entry.timestampUtc = baseDayStartUtc + static_cast<int64_t>(i * 86400);
        } else {
            entry.tempMinC = snapshot.currentTempC;
            entry.tempMaxC = snapshot.currentTempC;
            entry.rainProbPct = 0;
            entry.cloudPct = 0;
            entry.humidityPct = snapshot.humidityPct;
            entry.weatherId = snapshot.currentWeatherId;
        }
    }

    city.snapshot = snapshot;
    city.lastUpdatedEpoch = static_cast<uint32_t>(snapshot.fetchedAtEpoch);
    return true;
}

bool WeatherClient::httpGetJson(const String &url, DynamicJsonDocument &doc)
{
    if (WiFi.status() != WL_CONNECTED) {
        _lastError = "Wi-Fi is not connected";
        return false;
    }

    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;
    http.setTimeout(HTTP_TIMEOUT_MS);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

    if (!http.begin(client, url)) {
        _lastError = "HTTP begin failed";
        return false;
    }

    const int statusCode = http.GET();
    if (statusCode != HTTP_CODE_OK) {
        const String detail = summarizeHttpErrorBody(http.getString());
        const String endpoint = endpointFromUrl(url);
        _lastError = detail.isEmpty()
                         ? ("HTTP " + String(statusCode) + " " + endpoint)
                         : ("HTTP " + String(statusCode) + " " + endpoint + ": " + detail);
        http.end();
        return false;
    }

    DeserializationError error = deserializeJson(doc, http.getStream());
    if (error) {
        _lastError = String("JSON parse error: ") + error.c_str();
        http.end();
        return false;
    }

    http.end();
    return true;
}

String WeatherClient::urlEncode(const String &raw) const
{
    static const char hex[] = "0123456789ABCDEF";
    String encoded;
    encoded.reserve(raw.length() * 3);

    for (size_t i = 0; i < raw.length(); ++i) {
        const unsigned char c = static_cast<unsigned char>(raw[i]);
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~' || c == ',') {
            encoded += static_cast<char>(c);
        } else if (c == ' ') {
            encoded += "%20";
        } else {
            encoded += '%';
            encoded += hex[(c >> 4) & 0x0F];
            encoded += hex[c & 0x0F];
        }
    }

    return encoded;
}
