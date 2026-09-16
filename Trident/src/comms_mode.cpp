#include "comms_mode.h"
#include <Preferences.h>

static const char *kNamespace = "comms";
static const char *kModeKey = "mode";

CommsMode commsModeGet() {
  Preferences prefs;
  prefs.begin(kNamespace, /*readOnly=*/true);
  uint8_t v = prefs.getUChar(kModeKey, COMMS_WEBSOCKET);
  prefs.end();
  return (v == COMMS_BLE) ? COMMS_BLE : COMMS_WEBSOCKET;
}

void commsModeSet(CommsMode mode) {
  Preferences prefs;
  prefs.begin(kNamespace, /*readOnly=*/false);
  prefs.putUChar(kModeKey, (uint8_t)mode);
  prefs.end();
}

const char *commsModeName(CommsMode mode) {
  return mode == COMMS_BLE ? "BLE" : "WebSocket";
}
