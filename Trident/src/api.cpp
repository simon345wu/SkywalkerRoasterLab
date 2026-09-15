#include "dlog.h"
#include "weather.h"
#include "wifi_setup.h"
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <Preferences.h>

void setupApi(AsyncWebServer *server) {
  D_println(LOG_WIFI, "setting up api");
  server->on("/api/wifi", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("ssid") || !request->hasParam("pass")) {
      AsyncWebServerResponse *response = request->beginResponse(400);
      request->send(response);
      return;
    }

    const char *ssid = request->getParam("ssid")->value().c_str();
    const char *pass = request->getParam("pass")->value().c_str();

    Preferences prefs;
    prefs.begin(wifiPrefsKey, false);
    prefs.putString(wifiSSIDKey, ssid);
    prefs.putString(wifiPassKey, pass);
    D_printf(LOG_WIFI, "saving to prefs, ssid: %s\n", ssid);

    prefs.end();
    request->send(200);
  });

  // GET with ?proxy=<url> stores the PC weather-proxy base URL (e.g.
  // http://192.168.31.50:8765); without params returns the current proxy and
  // the latest cached ambient reading as JSON.
  server->on("/api/weather", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (request->hasParam("proxy")) {
      weatherSetProxy(request->getParam("proxy")->value());
      request->send(200);
      return;
    }
    WeatherData w = weatherGet();
    JsonDocument doc;
    doc["proxy"] = weatherGetProxy();          // manual override ("" = auto)
    doc["active"] = weatherGetActiveBase();     // URL actually in use (mDNS or manual)
    doc["valid"] = w.valid;
    doc["temp"] = w.tempC;
    doc["pressure"] = w.pressureHpa;
    doc["humidity"] = w.humidity;
    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
  });
}
