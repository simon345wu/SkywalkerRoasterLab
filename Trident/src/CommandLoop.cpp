#include <ArduinoJson.h>

#include "CommandLoop.h"
#include "dlog.h"
#include "model.h"
#include "state_request_queue.h"
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
#ifdef DEBUG
    D_printf(LOG_WS, "ws[%s][%u] %s-message[%llu]: ", server->url(),
             client->id(), (info->opcode == WS_TEXT) ? "text" : "binary",
             info->len);
    D_printf(LOG_WS, "final: %d\n", info->final);
#endif
    String msg = "";
    /*if (info->opcode != WS_TEXT || !info->final) {*/
    /*  break;*/
    /*}*/

    for (size_t i = 0; i < info->len; i++) {
      msg += (char)data[i];
    }
#ifdef DEBUG
    D_printf(LOG_WS, "msg: %s\n", msg.c_str());
#endif

    JsonDocument doc;

    // DEBUG WEBSOCKET
    // D_printf("[%u] get Text: %s\n", num, payload);

    // Extract Values lt. https://arduinojson.org/v6/example/http-client/
    // Artisan Anleitung: https://artisan-scope.org/devices/websockets/

    deserializeJson(doc, msg);

    long ln_id = doc["id"].as<long>();
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
    const char *command = doc["command"].as<const char *>();
    if (command != NULL && strncmp(command, "getData", 7) == 0) {
      wsHandshakeDone = true;
      root["data"]["ET"] = state.et;   // external MAX31865 probe (or BT mirrored)
      root["data"]["BT"] = state.temp; // roaster's own probe
      root["data"]["BurnerVal"] = state.request.heater;
      root["data"]["FanVal"] = state.request.fan;
      root["data"]["Drum"] = state.request.drum;
      root["data"]["Cool"] = state.request.cooling;
    }

    char buffer[200];                         // create temp buffer
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
