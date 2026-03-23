# T5 ePaper Weather Station (Multi-City + Touch)

Firmware for the LilyGO T5 4.7 S3 e-paper board using PlatformIO + Arduino.

## Features
- Multi-city weather dashboard (up to 5 configured cities)
- Touch navigation (`< Prev`, `Next >`) between cities
- Current conditions with humidity, cloud cover, and rain chance
- 24-hour forecast and 7-day high/low forecast
- Cached city snapshots stored in SPIFFS
- Deep sleep refresh cycle (default: every 60 minutes)

## Hardware / Software
- Board: LilyGO T5 4.7 S3 (`esp32s3box` profile)
- Framework: Arduino (PlatformIO)
- API provider: OpenWeatherMap (Geocoding + One Call)

## Project Layout
- `src/` firmware source code
- `include/app_config.h` city list and app settings
- `include/secrets.example.h` credentials template
- `include/secrets.h` local credentials (gitignored)
- `platformio.ini` PlatformIO environment and dependencies

## Quick Start
1. Install [PlatformIO](https://platformio.org/).
2. Create your local credentials file:
   - Copy `include/secrets.example.h` to `include/secrets.h`
   - Fill in `WIFI_SSID`, `WIFI_PASSWORD`, and `OWM_API_KEY`
3. Configure your cities in `include/app_config.h` (`kCityQueries`).
4. Build and upload:
   - `pio run`
   - `pio run -t upload`
5. Optional serial monitor:
   - `pio device monitor --baud 115200`

## Security Notes
- `include/secrets.h` is intentionally excluded from git via `.gitignore`.
- Never commit real WiFi passwords or API keys.
- Keep `include/secrets.example.h` as placeholders only.

## Publishing Checklist
1. Confirm `include/secrets.h` is placeholder-only or local-only.
2. Run a secret scan before pushing (example):
   - `Get-ChildItem -Recurse -File | Select-String -Pattern 'WIFI_SSID|WIFI_PASSWORD|OWM_API_KEY|apikey|api_key|password'`
3. Commit and push to your remote repository.

## Notes
- Units default to metric and time format uses 24-hour clock.
- Touch wake from deep sleep is disabled by default (`kEnableTouchWakeup = false`) due board variance.
