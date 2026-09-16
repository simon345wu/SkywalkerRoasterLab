#include <ArduinoJson.h>

#include "CommandLoop.h"
#include "dlog.h"
#include "model.h"
#include "state_request_queue.h"
#include "weather.h"
#include <ESPAsyncWebServer.h>

AsyncWebSocket ws("/ws");

StateDataT state = {0};
StateRequestT request = {255, 255, 255, 255};

// Distinct from "a WebSocket client is connected" (see wsClientConnected()):
// this only flips once the client actually sends a getData request, i.e.
// Artisan (not just some raw TCP/WS connection) is really talking to us.
// Mirrors artisanHandshakeDone/hibeanHandshakeDone for USB/BLE.
bool wsHandshakeDone = false;

bool wsClientConnected() { return ws.count() > 0; }

void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
               AwsEventType type, void *arg, uint8_t *data, size_t len) {

  switch (type) {
  case WS_EVT_CONNECT: {
    D_printf(LOG_WS, "[%u] Connected!\n", client->id());
    // Artisan's own "ON" event action is unreliable over WebSocket (races
    // its device connection setup, see PROGRESS.md) so the drum is started
    // here instead, directly on socket connect, rather than depending on
    // Artisan sending a command for it.
    StateRequestT onConnectReq = {255, 255, 255, 100};
    enqueueStateRequest(onConnectReq, SOURCE_WEBSOCKET);
  } break;
  case WS_EVT_DISCONNECT: {
    D_printf(LOG_WS, "[%u] Disconnected!\n", client->id());
    wsHandshakeDone = false;
    // turn off heater and set fan to 100%
    // setHeaterPower(0);
    // setFanSpeed(100);
  } break;
  case WS_EVT_DATA: {

    AwsFrameInfo *info = (AwsFrameInfo *)arg;

    String msg = "";
    /*if (info->opcode != WS_TEXT || !info->final) {*/
    /*  break;*/
    /*}*/

    for (size_t i = 0; i < info->len; i++) {
      msg += (char)data[i];
    }

    JsonDocument doc;

    // Extract Values lt. https://arduinojson.org/v6/example/http-client/
    // Artisan Anleitung: https://artisan-scope.org/devices/websockets/

    DeserializationError jsonErr = deserializeJson(doc, msg);
    if (jsonErr) {
      // Previously silent: a malformed/unexpected payload here left doc
      // empty, so every doc["..."].isNull() check below was quietly true
      // and nothing happened -- indistinguishable from "nothing arrived" at
      // all without this. Always printed, even for the routine getData poll
      // below, since a parse failure is never routine.
      D_printf(LOG_WS, "JSON parse failed (%s): %s\n", jsonErr.c_str(),
               msg.c_str());
      break;
    }

    long ln_id = doc["id"].as<long>();
    const char *cmdPeek = doc["command"].as<const char *>();
    bool isGetDataPoll = cmdPeek != NULL && strncmp(cmdPeek, "getData", 7) == 0;
    if (!isGetDataPoll) {
      // Artisan polls with a plain {"command":"getData",...} many times a
      // second (once per configured channel) -- routine and uninteresting,
      // so it's skipped here to keep real commands (BurnerVal/FanVal/Drum/
      // Cooling) visible in "LOG;WS;ON" instead of buried under it. Was also
      // previously gated behind #ifdef DEBUG, a build flag never actually
      // defined anywhere in platformio.ini -- dead code in every build this
      // firmware has shipped, regardless of the getData noise.
      D_printf(LOG_WS, "ws[%s][%u] %s-message[%llu]: ", server->url(),
               client->id(), (info->opcode == WS_TEXT) ? "text" : "binary",
               info->len);
      D_printf(LOG_WS, "final: %d\n", info->final);
      D_printf(LOG_WS, "msg: %s\n", msg.c_str());
    }
    // Get BurnerVal from Artisan over Websocket
    if (!doc["BurnerVal"].isNull()) {
      unsigned char val = doc["BurnerVal"].as<unsigned char>();
      D_printf(LOG_WS, "BurnerVal: %d\n", val);
      // DimmerVal = doc["BurnerVal"].as<long>();
      request.heater = val;
    }
    if (!doc["FanVal"].isNull()) {
      unsigned char val = doc["FanVal"].as<unsigned char>();
      D_printf(LOG_WS, "FanVal: %d\n", val);
      request.fan = val;
    }
    if (!doc["Drum"].isNull()) {
      unsigned char val = doc["Drum"].as<unsigned char>();
      D_printf(LOG_WS, "Drum: %d\n", val);
      request.drum = val;
    }
    if (!doc["Cooling"].isNull()) {
      unsigned char val = doc["Cooling"].as<unsigned char>();
      D_printf(LOG_WS, "Cooling: %d\n", val);
      request.cooling = val;
    }

    // Send Values to Artisan over Websocket
    JsonDocument root;
    root["id"] = ln_id;
    if (isGetDataPoll) {
      wsHandshakeDone = true;
      root["data"]["ET"] = state.et;   // external MAX31865 #1 probe (or BT mirrored)
      root["data"]["BT"] = state.bt;   // external MAX31865 #2 probe (or NTC mirrored)
      root["data"]["NTC"] = state.ntc; // roaster's own built-in probe, raw
      root["data"]["BurnerVal"] = state.request.heater;
      root["data"]["FanVal"] = state.request.fan;
      root["data"]["Drum"] = state.request.drum;
      root["data"]["Cool"] = state.request.cooling;
      // Ambient conditions (weather.cpp fetches them over plain HTTP from the
      // PC proxy). Only emitted once a valid reading exists, so Artisan never
      // records a placeholder. Mapped as extra channels AT/AP/AH and assigned
      // in Artisan's Ambient tab.
      WeatherData w = weatherGet();
      if (w.valid) {
        root["data"]["AT"] = w.tempC;       // ambient temperature
        root["data"]["AP"] = w.pressureHpa; // barometric pressure
        root["data"]["AH"] = w.humidity;    // relative humidity
      }
    }

    char buffer[320];                         // create temp buffer
    size_t len = serializeJson(root, buffer); // serialize to buffer
    // DEBUG WEBSOCKET

    client->text(buffer);
    // send message to client
    // webSocket.sendTXT(num, "message here");

    // send data to all connected clients
    // webSocket.broadcastTXT("message here");
    enqueueStateRequest(request, SOURCE_WEBSOCKET);
  } break;
  case WS_EVT_PING:
  case WS_EVT_PONG:
    // WebSocket-level keepalive -- ESPAsyncWebServer already auto-replies to
    // pings internally regardless of this callback, nothing to do here.
    // Split out from default so this expected, harmless traffic doesn't get
    // logged as "unhandled" (it was, confusingly, before -- see PROGRESS.md).
    break;
  default:
    D_printf(LOG_WS, "unhandled message type: %d\n", type);
    break;
  }
}

void setupMainLoop(AsyncWebServer *server) {
  ws.onEvent(onWsEvent);
  server->addHandler(&ws);
}

StateRequestT socketTick(StateDataT data) {
  state = data;
  ws.cleanupClients();
  StateRequestT response = request;
  request = {255, 255, 255, 255};
  return response;
}
