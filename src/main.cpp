#include <Arduino.h>

#include <WiFi.h>
#include <esp_sleep.h>
#include <time.h>

#include "app_config.h"
#include "display_renderer.h"
#include "state_store.h"
#include "touch_input.h"
#include "weather_client.h"
#include "weather_types.h"

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "Missing include/secrets.h. Copy include/secrets.example.h to include/secrets.h and fill credentials."
#endif

namespace {

constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 20000;
constexpr uint32_t NTP_WAIT_TIMEOUT_MS = 12000;

String maskedKeyInfo()
{
    const String key = String(OWM_API_KEY);
    const size_t len = key.length();
    String suffix = key;
    if (len > 4) {
        suffix = key.substring(len - 4);
    }

    return "len=" + String(len) + ", suffix=" + suffix;
}

DisplayRenderer gDisplay;
TouchInput gTouch;
StateStore gStateStore;
WeatherClient gWeather(OWM_API_KEY);

CityState gCities[AppConfig::kMaxCities];
uint8_t gCityCount = 0;

uint8_t clampIndex(uint8_t candidate)
{
    if (gCityCount == 0) {
        return 0;
    }
    return static_cast<uint8_t>(candidate % gCityCount);
}

uint8_t initializeCities()
{
    uint8_t count = 0;
    for (uint8_t i = 0; i < AppConfig::kMaxCities; ++i) {
        const char *entry = AppConfig::kCityQueries[i];
        if (entry == nullptr || entry[0] == '\0') {
            continue;
        }

        gCities[count].query = String(entry);
        ++count;
    }
    return count;
}

bool connectWifi()
{
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    const uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED) {
        delay(200);
        if ((millis() - start) > WIFI_CONNECT_TIMEOUT_MS) {
            return false;
        }
    }

    return true;
}

void disconnectWifi()
{
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
}

bool syncTime()
{
    configTime(0, 0, AppConfig::kNtpServer1, AppConfig::kNtpServer2);

    const uint32_t start = millis();
    while ((millis() - start) < NTP_WAIT_TIMEOUT_MS) {
        const time_t now = time(nullptr);
        if (now > 1700000000) {
            return true;
        }
        delay(200);
    }

    return false;
}

void refreshStaleFlags()
{
    const time_t now = time(nullptr);

    for (uint8_t i = 0; i < gCityCount; ++i) {
        CityState &city = gCities[i];
        if (!city.snapshot.valid) {
            city.snapshot.stale = true;
            continue;
        }

        if (now <= 0 || city.snapshot.fetchedAtEpoch <= 0) {
            city.snapshot.stale = true;
            continue;
        }

        const int64_t age = static_cast<int64_t>(now) - city.snapshot.fetchedAtEpoch;
        city.snapshot.stale = age > AppConfig::kStaleAfterSeconds;
    }
}

String statusDetailForCity(const CityState &city)
{
    if (!city.lastError.isEmpty()) {
        String detail = city.lastError;
        if (detail.length() > 72) {
            detail = detail.substring(0, 69) + "...";
        }
        return detail;
    }

    return "No data for " + city.query;
}

bool refreshCity(uint8_t cityIndex)
{
    if (cityIndex >= gCityCount) {
        return false;
    }

    CityState &city = gCities[cityIndex];
    const bool ok = gWeather.fetchCityWeather(city);
    if (!ok) {
        city.lastError = gWeather.lastError();
        city.snapshot.stale = true;
        Serial.println("[weather] Failed " + city.query + " -> " + city.lastError);
        return false;
    }

    city.lastError = "";
    city.snapshot.stale = false;
    gStateStore.saveCityState(cityIndex, city);

    Serial.println("[weather] Updated " + city.query + " -> " + city.resolvedName);
    return true;
}

uint32_t computeSleepSeconds()
{
    const time_t now = time(nullptr);
    if (now <= 0) {
        return AppConfig::kUpdateIntervalSeconds;
    }

    const uint32_t interval = AppConfig::kUpdateIntervalSeconds;
    uint32_t sleepFor = interval - (static_cast<uint32_t>(now) % interval);

    if (sleepFor < 15) {
        sleepFor += interval;
    }

    return sleepFor;
}

void enterDeepSleep()
{
    const uint32_t sleepSeconds = computeSleepSeconds();

    esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(sleepSeconds) * 1000000ULL);

    if (AppConfig::kEnableTouchWakeup) {
        const uint64_t touchMask = 1ULL << TouchPins::kTouchInt;
        esp_sleep_enable_ext1_wakeup(touchMask, ESP_EXT1_WAKEUP_ANY_HIGH);
    }

    epd_poweroff_all();
    esp_deep_sleep_start();
}

void renderActiveCity(uint8_t activeCityIndex, bool forceFullRefresh)
{
    if (activeCityIndex >= gCityCount) {
        gDisplay.renderStatusMessage("No city", "No city available to render", true);
        return;
    }

    const CityState &city = gCities[activeCityIndex];
    if (!city.snapshot.valid) {
        gDisplay.renderStatusMessage("Weather unavailable", statusDetailForCity(city), true);
        return;
    }

    gDisplay.renderDashboard(city, activeCityIndex, gCityCount, forceFullRefresh);
}

} // namespace

void setup()
{
    Serial.begin(115200);
    delay(100);
    Serial.println("[cfg] OWM key " + maskedKeyInfo());

    if (!gDisplay.begin()) {
        while (true) {
            delay(1000);
        }
    }

    gDisplay.renderStatusMessage("Boot", "Initializing weather station", true);

    if (!gStateStore.begin()) {
        gDisplay.renderStatusMessage("Storage error", "Failed to initialize state store", true);
        delay(4000);
        enterDeepSleep();
    }

    gCityCount = initializeCities();
    if (gCityCount == 0) {
        gDisplay.renderStatusMessage("Configuration error", "Add at least one city in app_config.h", true);
        delay(5000);
        enterDeepSleep();
    }

    uint8_t activeCityIndex = 0;
    uint8_t roundRobinIndex = 0;
    uint32_t refreshCounter = 0;

    gStateStore.loadActiveCityIndex(activeCityIndex);
    gStateStore.loadRoundRobinIndex(roundRobinIndex);
    gStateStore.loadRefreshCounter(refreshCounter);

    activeCityIndex = clampIndex(activeCityIndex);
    roundRobinIndex = clampIndex(roundRobinIndex);

    for (uint8_t i = 0; i < gCityCount; ++i) {
        CityState loaded;
        if (!gStateStore.loadCityState(i, loaded)) {
            continue;
        }

        const String configuredQuery = gCities[i].query;
        if (!configuredQuery.equalsIgnoreCase(loaded.query)) {
            // City list changed, drop stale coordinates/cache for this slot.
            gCities[i] = CityState{};
            gCities[i].query = configuredQuery;
            continue;
        }

        loaded.query = configuredQuery;
        gCities[i] = loaded;
    }

    gTouch.begin();

    gDisplay.renderStatusMessage("Network", "Connecting Wi-Fi", true);

    const bool wifiConnected = connectWifi();
    if (wifiConnected) {
        syncTime();

        refreshCity(activeCityIndex);

        if (gCityCount > 1) {
            uint8_t extraCity = roundRobinIndex;
            if (extraCity == activeCityIndex) {
                extraCity = clampIndex(static_cast<uint8_t>(extraCity + 1));
            }

            refreshCity(extraCity);
            roundRobinIndex = clampIndex(static_cast<uint8_t>(extraCity + 1));
            gStateStore.saveRoundRobinIndex(roundRobinIndex);
        }
    } else if (activeCityIndex < gCityCount) {
        gCities[activeCityIndex].lastError = "Wi-Fi connection failed";
    }

    refreshStaleFlags();

    // Always do a clean full refresh after status screens to prevent ghost text
    // (for example, "Network") from lingering into the dashboard.
    renderActiveCity(activeCityIndex, true);

    gStateStore.saveRefreshCounter(refreshCounter + 1);
    gStateStore.saveActiveCityIndex(activeCityIndex);

    uint32_t interactionStart = millis();
    while ((millis() - interactionStart) < AppConfig::kInteractionWindowMs) {
        const TouchAction action = gTouch.pollTouchAction(AppConfig::kTouchDebounceMs);
        if (action == TouchAction::None) {
            delay(20);
            continue;
        }

        if (action == TouchAction::PrevCity) {
            activeCityIndex = (activeCityIndex == 0) ? static_cast<uint8_t>(gCityCount - 1)
                                                     : static_cast<uint8_t>(activeCityIndex - 1);
        } else if (action == TouchAction::NextCity) {
            activeCityIndex = clampIndex(static_cast<uint8_t>(activeCityIndex + 1));
        }

        gStateStore.saveActiveCityIndex(activeCityIndex);
        refreshStaleFlags();

        if (wifiConnected && gCities[activeCityIndex].snapshot.stale) {
            refreshCity(activeCityIndex);
            refreshStaleFlags();
        }

        // Force full refresh when user flips city pages to minimize ghosting artifacts.
        renderActiveCity(activeCityIndex, true);

        // Keep the screen interactive a bit longer after user activity.
        interactionStart = millis();
    }

    disconnectWifi();
    enterDeepSleep();
}

void loop()
{
    delay(1000);
}


