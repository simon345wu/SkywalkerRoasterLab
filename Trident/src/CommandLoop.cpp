#include <ArduinoJson.h>

#include "CommandLoop.h"
#include "dlog.h"
#include "model.h"
#include "state_request_queue.h"
#include "weather.h"
#include <ESPAsyncWebServer.h>
#include <esp_heap_caps.h>

// Heap diagnostics for the "WebSocket dies after a while" investigation.
// Prints total free heap, the lowest free heap ever seen (min watermark), the
// largest contiguous INTERNAL-RAM block (fragmentation -- WiFi/AsyncTCP can
// only use internal RAM, not PSRAM), free internal RAM, and the live WS client
// count. Emitted to WebSerial, which stays reachable after Artisan drops since
// the board itself keeps running, so the reading at the moment of failure is
// visible. Toggle off at runtime with "LOG;WS;OFF" if it gets noisy.
static void logHeapStats(const char *tag, size_t wsClients) {
  D_printf(LOG_DIAG,
           "[HEAP] %s free=%u minFree=%u internalFree=%u internalLargest=%u "
           "wsClients=%u\n",
           tag, (unsigned)esp_get_free_heap_size(),
           (unsigned)esp_get_minimum_free_heap_size(),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
           (unsigned)wsClients);
}

AsyncWebSocket ws("/ws");

StateDataT state = {0};
StateRequestT request = {255, 255, 255, 255};

// Distinct from "a WebSocket client is connected" (see wsClientConnected()):
// this only flips once the client actually sends a getData request, i.e.
// Artisan (not just some raw TCP/WS connection) is really talking to us.
// Mirrors artisanHandshakeDone/hibeanHandshakeDone for USB/BLE.
bool wsHandshakeDone = false;

bool wsClientConnected() { return ws.count() > 0; }

// getData polls counted in onWsEvent (AsyncTCP task), read + reset every ~3s in
// socketTick (webSerialLoop task) to print the actual request rate. A rate
// counter tolerates the tiny cross-task read/reset race, so plain volatile is
// enough here.
static volatile uint32_t s_getDataCount = 0;
// Replies dropped because the client's send queue was full (the queueIsFull
// guard below). Each drop is one sample the client never receives for the
// channel it asked -- i.e. a gap in Artisan. Printed alongside the rate so we
// can tell a firmware-side drop from an Artisan-side (request_timeout) gap.
static volatile uint32_t s_replyDropped = 0;

void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
               AwsEventType type, void *arg, uint8_t *data, size_t len) {

  switch (type) {
  case WS_EVT_CONNECT: {
    D_printf(LOG_WS, "[%u] Connected!\n", client->id());
    logHeapStats("connect", ws.count());
    // Defense-in-depth against the "WebSocket dies after a while" hang. By
    // default AsyncWebSocketClient::closeWhenFull is true, so the moment our
    // per-client send queue overflows (Artisan polls getData faster than WiFi
    // can flush during a hiccup) the library calls _client->close() and drops
    // the connection -- board stays alive (display/touch are other tasks) but
    // Artisan sees a disconnect and hangs. For a continuous getData feed, losing
    // a reply is fine (the next poll is a fresh snapshot), so tell the library
    // to DISCARD on a full queue instead of closing. Paired with the
    // queueIsFull() guard before client->text() below, which keeps our own send
    // path from ever filling the queue in the first place.
    client->setCloseClientOnQueueFull(false);
    // Artisan's own "ON" event action is unreliable over WebSocket (races
    // its device connection setup, see PROGRESS.md) so the drum is started
    // here instead, directly on socket connect, rather than depending on
    // Artisan sending a command for it.
    StateRequestT onConnectReq = {255, 255, 255, 100};
    enqueueStateRequest(onConnectReq, SOURCE_WEBSOCKET);
  } break;
  case WS_EVT_DISCONNECT: {
    D_printf(LOG_WS, "[%u] Disconnected!\n", client->id());
    // Snapshot heap right at the disconnect -- if the WS is dying from memory
    // exhaustion/fragmentation, this line captures how low it got.
    logHeapStats("disconnect", ws.count());
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
    if (isGetDataPoll) {
      s_getDataCount++; // tallied here, rate printed from socketTick every ~3s
    }
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

    // Only enqueue a reply if the client's send queue has room. Artisan polls
    // getData at high frequency (once per configured channel, many times a
    // second), so if WiFi/TCP momentarily can't flush as fast as replies are
    // produced, an unconditional client->text() overflows AsyncWebSocket's
    // per-client queue -- _queueMessage() then logs "Too many messages queued"
    // (AsyncWebSocket.cpp:436) and the socket stalls, which is the "WebSocket
    // freezes after running a while" hang. The library's own header recommends
    // checking queueIsFull() before sending. A getData reply is an always-fresh
    // snapshot, so dropping one when the queue is backed up is harmless -- the
    // next poll carries current values -- and it keeps the board alive instead
    // of wedging. (Was previously only papered over by silencing the IDF log
    // that this same overflow emits, see main.cpp's esp_log_set_vprintf.)
    if (!client->queueIsFull()) {
      client->text(buffer);
    } else {
      s_replyDropped++;
      D_printf(LOG_DIAG, "WS send queue full -- dropping getData reply (%u bytes)\n",
               (unsigned)len);
    }

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
  // Periodic heap trend (every ~3s). Watching this while Artisan polls until it
  // dies tells us whether free heap / largest internal block is steadily
  // dropping (leak or fragmentation) or holding flat (then the disconnect is
  // something other than memory). Also surfaces ghost WS clients piling up if
  // cleanupClients() ever fails to prune a half-closed connection.
  static unsigned long lastHeapLogMs = 0;
  unsigned long nowMs = millis();
  if (nowMs - lastHeapLogMs >= 3000) {
    unsigned long elapsed = nowMs - lastHeapLogMs;
    lastHeapLogMs = nowMs;
    // Snapshot + reset the getData tally and turn it into a per-second rate over
    // the actual elapsed window -- this is how many getData polls Artisan is
    // really sending (one per channel whose Request is getData, per sample).
    uint32_t count = s_getDataCount;
    uint32_t dropped = s_replyDropped;
    s_getDataCount = 0;
    s_replyDropped = 0;
    float rate = elapsed > 0 ? (count * 1000.0f) / (float)elapsed : 0.0f;
    // dropped>0 => the gaps are firmware-side (queue full). dropped==0 while
    // Artisan still shows gaps => the drop is Artisan-side (request_timeout).
    D_printf(LOG_DIAG, "[WSRATE] getData=%u dropped=%u over %lums -> %.1f/s\n",
             (unsigned)count, (unsigned)dropped, elapsed, rate);
    logHeapStats("tick", ws.count());
  }
  StateRequestT response = request;
  request = {255, 255, 255, 255};
  return response;
}
