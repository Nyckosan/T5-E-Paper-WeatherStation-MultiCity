#pragma once

#include <Arduino.h>
#include <Preferences.h>

#include "weather_types.h"

class StateStore {
public:
    bool begin();

    bool loadActiveCityIndex(uint8_t &index);
    void saveActiveCityIndex(uint8_t index);

    bool loadRoundRobinIndex(uint8_t &index);
    void saveRoundRobinIndex(uint8_t index);

    bool loadRefreshCounter(uint32_t &counter);
    void saveRefreshCounter(uint32_t counter);

    bool loadCityState(uint8_t cityIndex, CityState &state);
    bool saveCityState(uint8_t cityIndex, const CityState &state);

private:
    String cityPath(uint8_t cityIndex) const;

    Preferences _prefs;
};
