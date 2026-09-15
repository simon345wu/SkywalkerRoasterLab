#include "weather.h"
#include "dlog.h"

#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>

static const char *kWeatherPrefs = "weather";
static const char *kProxyKey = "proxy";

// Refresh from the PC proxy this often once we have a working one. Ambient
// drifts slowly and the proxy itself caches the Open-Meteo call.
static const uint32_t kRefreshIntervalMs = 5UL * 60UL * 1000UL;
// Retry cadence while we have no working proxy yet (still discovering / proxy
// offline) -- much shorter than the refresh interval so it comes up quickly.
static const uint32_t kRetryMs = 20UL * 1000UL;
static const uint32_t kPollTickMs = 2000;

// mDNS service the PC proxy (artisan_weather_ws.py) advertises.
static const char *kMdnsService = "artisanwx"; // -> _artisanwx._tcp
static const char *kMdnsProto = "tcp";

// Cache + URLs guarded by a spinlock: the poll task writes, the WebSocket
// callback (CommandLoop.cpp) and display (display.cpp) read.
static portMUX_TYPE weatherMux = portMUX_INITIALIZER_UNLOCKED;
static WeatherData cached = {0, 0, 0, false};
static String proxyUrl = "";      // manual override (NVS); empty = auto-discover
static String discoveredUrl = ""; // last mDNS-discovered proxy (RAM only)

static volatile bool forceRefresh = false;
static volatile uint32_t lastSuccessMs = 0;

static void loadProxy() {
  Preferences prefs;
  prefs.begin(kWeatherPrefs, /*readOnly=*/true);
  String p = prefs.getString(kProxyKey, "");
  prefs.end();
  portENTER_CRITICAL(&weatherMux);
  proxyUrl = p;
  portEXIT_CRITICAL(&weatherMux);
}

// One plain-HTTP GET to <base>/api/preview. Runs only on the poll task. The
// proxy returns {"temp":..,"pressure":..,"humidity":..,"valid":bool}.
static bool fetchOnce(const String &base) {
  String url = base + "/api/preview";
  HTTPClient http;
  http.setConnectTimeout(4000);
  http.setTimeout(4000);
  if (!http.begin(url)) {
    D_println(LOG_WEATHER, "weather: http.begin failed");
    return false;
  }
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    D_printf(LOG_WEATHER, "weather: HTTP %d\n", code);
    http.end();
    return false;
  }
  String payload = http.getString();
  http.end();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    D_printf(LOG_WEATHER, "weather: JSON parse failed (%s)\n", err.c_str());
    return false;
  }
  if (doc["temp"].isNull()) {
    D_printf(LOG_WEATHER, "weather: unexpected response: %.100s\n",
             payload.c_str());
    return false;
  }

  WeatherData w;
  w.tempC = doc["temp"].as<float>();
  w.pressureHpa = doc["pressure"].as<float>();
  w.humidity = doc["humidity"].as<float>();
  // Trust the proxy's own valid flag (false until its location is set / first
  // Open-Meteo fetch lands).
  w.valid = doc["valid"].as<bool>();

  portENTER_CRITICAL(&weatherMux);
  cached = w;
  portEXIT_CRITICAL(&weatherMux);
  lastSuccessMs = millis();

  if (w.valid) {
    D_printf(LOG_WEATHER, "weather: %.1fC %.1fhPa %.0f%%\n", w.tempC,
             w.pressureHpa, w.humidity);
  } else {
    D_println(LOG_WEATHER, "weather: proxy has no reading yet (set location "
                           "on the PC page)");
  }
  return true;
}

// Query mDNS for the PC proxy and return "http://<ip>:<port>" (or "" if not
// found). Blocking (~1-2s) -- only ever called from the poll task. Uses the
// ESPmDNS responder already started by wifi_setup.cpp.
static String discover() {
  int n = MDNS.queryService(kMdnsService, kMdnsProto);
  if (n <= 0) {
    return "";
  }
  IPAddress ip = MDNS.address(0);
  uint16_t port = MDNS.port(0);
  if (port == 0) {
    port = 8765;
  }
  // Reject a bogus advert (e.g. 0.0.0.0 from a proxy that couldn't determine
  // its own LAN IP) so we don't cache and keep retrying an unreachable URL.
  if (ip == IPAddress(0, 0, 0, 0)) {
    return "";
  }
  return String("http://") + ip.toString() + ":" + String(port);
}

static void weatherTask(void *params) {
  uint32_t nextMs = 0; // when the next attempt is due
  while (1) {
    if (WiFi.status() == WL_CONNECTED && (forceRefresh || millis() >= nextMs)) {
      forceRefresh = false;

      // Pick the base URL: a manual override wins; otherwise use the last
      // discovered one, and if we have none, run an mDNS query now.
      String manual;
      String base;
      portENTER_CRITICAL(&weatherMux);
      manual = proxyUrl;
      base = manual.length() ? manual : discoveredUrl;
      portEXIT_CRITICAL(&weatherMux);

      if (base.length() == 0) {
        base = discover();
        if (base.length()) {
          portENTER_CRITICAL(&weatherMux);
          discoveredUrl = base;
          portEXIT_CRITICAL(&weatherMux);
          D_printf(LOG_WEATHER, "weather: discovered proxy %s\n", base.c_str());
        }
      }

      bool ok = false;
      if (base.length() > 0) {
        ok = fetchOnce(base);
        // A discovered proxy that stops answering (e.g. PC IP changed) is
        // forgotten so the next attempt re-discovers. A manual override is
        // kept as-is.
        if (!ok && manual.length() == 0) {
          portENTER_CRITICAL(&weatherMux);
          discoveredUrl = "";
          portEXIT_CRITICAL(&weatherMux);
        }
      }
      // Slow cadence once it works; fast retry while there's no working proxy.
      nextMs = millis() + (ok ? kRefreshIntervalMs : kRetryMs);
    }
    vTaskDelay(pdMS_TO_TICKS(kPollTickMs));
  }
}

void weatherInit() {
  loadProxy();
  // Small stack -- plain HTTP, no TLS handshake (that was the whole reason to
  // move off HTTPS), so 4KB is ample and leaves internal RAM for BLE/async.
  xTaskCreate(weatherTask, "WeatherTask", configMINIMAL_STACK_SIZE + 4096, NULL,
              1, NULL);
}

WeatherData weatherGet() {
  portENTER_CRITICAL(&weatherMux);
  WeatherData w = cached;
  portEXIT_CRITICAL(&weatherMux);
  return w;
}

String weatherGetProxy() {
  portENTER_CRITICAL(&weatherMux);
  String p = proxyUrl;
  portEXIT_CRITICAL(&weatherMux);
  return p;
}

String weatherGetActiveBase() {
  portENTER_CRITICAL(&weatherMux);
  String b = proxyUrl.length() ? proxyUrl : discoveredUrl;
  portEXIT_CRITICAL(&weatherMux);
  return b;
}

bool weatherSetProxy(const String &url) {
  String u = url;
  u.trim();
  // Strip a trailing slash so fetchOnce()'s "<base>/api/preview" is clean.
  while (u.endsWith("/")) {
    u.remove(u.length() - 1);
  }
  Preferences prefs;
  prefs.begin(kWeatherPrefs, /*readOnly=*/false);
  prefs.putString(kProxyKey, u);
  prefs.end();

  portENTER_CRITICAL(&weatherMux);
  proxyUrl = u;
  portEXIT_CRITICAL(&weatherMux);

  forceRefresh = true;
  D_printf(LOG_WEATHER, "weather: proxy set to %s\n", u.c_str());
  return true;
}

bool weatherHandleCommand(const String &inputRaw) {
  String upper = inputRaw;
  upper.trim();
  upper.toUpperCase();
  if (upper != "WEATHER" && !upper.startsWith("WEATHER;")) {
    return false;
  }
  if (upper == "WEATHER;NOW") {
    forceRefresh = true;
    WebSerial.println("[WEATHER] refresh requested");
    return true;
  }
  String manual = weatherGetProxy();
  String base = weatherGetActiveBase();
  WeatherData w = weatherGet();
  if (base.length() == 0) {
    WebSerial.println("[WEATHER] no proxy yet -- searching via mDNS (is the PC "
                      "proxy running?)");
    return true;
  }
  WebSerial.println(String("[WEATHER] proxy: ") + base +
                    (manual.length() ? " (manual)" : " (mDNS auto)"));
  if (!w.valid) {
    WebSerial.println("[WEATHER] no reading yet (proxy offline, or its location "
                      "not set)");
    return true;
  }
  WebSerial.println(String("[WEATHER] ") + String(w.tempC, 1) + "C  " +
                    String(w.pressureHpa, 1) + "hPa  " + String(w.humidity, 0) +
                    "%");
  uint32_t last = lastSuccessMs;
  if (last != 0) {
    WebSerial.println(String("[WEATHER] fetched ") +
                      String((millis() - last) / 1000) + "s ago");
  }
  return true;
}
