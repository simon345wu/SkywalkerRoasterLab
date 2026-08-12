#include <Arduino.h>
#include "model.h"

void initBLE(String sketchName, String firmWareVersion, String boardID);
StateRequestT bleTick(StateDataT data);
// The actual advertised BLE name (same string passed as boardID to
// initBLE()) -- for the LVGL Config screen to display, so it doesn't need
// its own separate copy of that string to keep in sync.
String getBleDeviceName();
