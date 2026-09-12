#ifndef WIFI_SETUP
#define WIFI_SETUP

// AsyncWebServer / WebSerial / the "/ws" and "/wserial" WebSocket endpoints
// all share this one port (main.cpp's `AsyncWebServer server(WEB_SERVER_PORT)`).
// Named here so display.cpp's Config-screen IP readout can show it without
// hardcoding a second copy of the number.
#define WEB_SERVER_PORT 80

// can take a while - check nvs for stored ssid and pass
// check if can connect
// if not, start AP mode
void setupWifi();

extern const char *wifiPrefsKey;
extern const char *wifiSSIDKey;
extern const char *wifiPassKey;

#endif
