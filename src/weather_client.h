#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

#include "weather_types.h"

class WeatherClient {
public:
    explicit WeatherClient(const String &apiKey);

    bool resolveCity(CityState &city);
    bool fetchCityWeather(CityState &city);

    const String &lastError() const;

private:
    bool httpGetJson(const String &url, DynamicJsonDocument &doc);
    String urlEncode(const String &raw) const;

    String _apiKey;
    String _lastError;
};
