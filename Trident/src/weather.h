#ifndef TRIDENT_WEATHER_H
#define TRIDENT_WEATHER_H

#include <Arduino.h>

// Ambient conditions for the roast environment. Fetched over PLAIN HTTP from a
// small proxy running on the PC (artisan_weather_ws.py), which does the HTTPS
// call to Open-Meteo and holds the location. Plain HTTP is deliberate: doing
// TLS on this board (BLE + WiFi + LVGL + AsyncWebServer already fill internal
// RAM to ~17KB free) needs 16-40KB and starves the async server, wedging
// WebSerial. Plain HTTP needs only a few KB. Values are cached and served to
// Artisan as extra WebSocket channels (AT/AP/AH) and shown on the Config
// screen.
struct WeatherData {
  float tempC;       // ambient temperature, degrees Celsius
  float pressureHpa; // barometric pressure at location, hPa
  float humidity;    // relative humidity, %
  bool valid;        // false until a good reading has been fetched
};

// Start the background weather-polling task. Call once in setup(), after
// setupWifi(). Idles until a proxy URL is set and WiFi is connected.
void weatherInit();

// Thread-safe copy of the latest cached reading.
WeatherData weatherGet();

// Manual proxy base URL override, e.g. "http://192.168.31.50:8765", stored in
// NVS. Empty (the default) means "auto-discover via mDNS". Setting it pins the
// proxy and disables discovery.
String weatherGetProxy();
bool weatherSetProxy(const String &url);

// The base URL actually in use right now: the manual override if set, else the
// last mDNS-discovered proxy. Empty if nothing found yet.
String weatherGetActiveBase();

// WebSerial console command handler (wired into main.cpp's onMessage).
// "WEATHER" prints the proxy + cached reading + age; "WEATHER;NOW" forces an
// immediate refresh. Returns true if it handled `input`.
bool weatherHandleCommand(const String &input);

#endif
