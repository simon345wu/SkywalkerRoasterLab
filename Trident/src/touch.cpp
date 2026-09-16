#include "touch.h"
#ifdef NO_DISPLAY
bool touchSessionActive = false;
void touchInit() {}
void touchLoop() {}
bool touchGetPoint(int *screenX, int *screenY) { return false; }
void sendFan(int newValue) {}
void sendHeat(int newValue) {}
void sendDrum(uint8_t newValue) {}
void sendCool(uint8_t newValue) {}
void sendStop() {}
#else
#include "display.h"
#include "dlog.h"
#include "model.h"
#include "state_request_queue.h"
#include <Arduino.h>
#include <SPI.h>
#include <XPT2046_Touchscreen.h>

bool touchSessionActive = false;

namespace {
const int SCREEN_WIDTH = 320;
const int SCREEN_HEIGHT = 240;

// Calibrated from a 4-corner touch test on real hardware (2026-08-01). Raw
// p.x/p.y are NOT swapped between axes, but both run opposite to screen
// coordinates (raw value goes DOWN as the screen coordinate goes UP).
const int TS_MINX = 392;  // raw p.x at the right edge (screenX = SCREEN_WIDTH)
const int TS_MAXX = 3740; // raw p.x at the left edge (screenX = 0)
const int TS_MINY = 464;  // raw p.y at the bottom edge (screenY = SCREEN_HEIGHT)
const int TS_MAXY = 3651; // raw p.y at the top edge (screenY = 0)

XPT2046_Touchscreen touchScreen(TOUCH_CS);

uint8_t clampPercent(int value) {
  if (value < 0)
    return 0;
  if (value > 100)
    return 100;
  return (uint8_t)value;
}
} // namespace

void touchInit() {
  // XPT2046_Touchscreen always talks to the global `SPI` object (no way to
  // inject a custom SPIClass), so that object is reserved for touch's own
  // dedicated pins here; the TFT uses a separate SPIClass (see display.cpp).
  SPI.begin(TOUCH_CLK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
  touchScreen.begin();
}

bool touchGetPoint(int *screenX, int *screenY) {
  if (!touchScreen.touched()) {
    return false;
  }
  TS_Point p = touchScreen.getPoint();
  *screenX = constrain(map(p.x, TS_MAXX, TS_MINX, 0, SCREEN_WIDTH), 0,
                       SCREEN_WIDTH - 1);
  *screenY = constrain(map(p.y, TS_MAXY, TS_MINY, 0, SCREEN_HEIGHT), 0,
                       SCREEN_HEIGHT - 1);
  return true;
}

void sendFan(int newValue) {
  StateRequestT req = {255, clampPercent(newValue), 255, 255};
  enqueueStateRequest(req, SOURCE_TOUCH);
  touchSessionActive = true;
}

void sendHeat(int newValue) {
  StateRequestT req = {clampPercent(newValue), 255, 255, 255};
  enqueueStateRequest(req, SOURCE_TOUCH);
  touchSessionActive = true;
}

void sendDrum(uint8_t newValue) {
  StateRequestT req = {255, 255, 255, newValue};
  enqueueStateRequest(req, SOURCE_TOUCH);
  touchSessionActive = true;
}

// handleCOOL() actually takes a 0-100 percentage, but there's no screen room
// for a full C0/C-/C+/C100 row, so this is a toggle (0/100) like Drum.
void sendCool(uint8_t newValue) {
  StateRequestT req = {255, 255, newValue, 255};
  enqueueStateRequest(req, SOURCE_TOUCH);
  touchSessionActive = true;
}

// Emergency stop: heater off, vent full open -- same as the ESTOP command
// other interfaces use, but sent through the normal arbitration queue like
// every other touch command (no longer a special direct bypass).
void sendStop() {
  StateRequestT req = {0, 100, 255, 255};
  enqueueStateRequest(req, SOURCE_TOUCH);
  touchSessionActive = false;
}

// No-op: touch input is handled entirely by the LVGL widget event callbacks
// (the dashboard sliders/buttons in display.cpp call sendFan()/sendHeat()/... ->
// the command queue), and the panel itself is read from the LVGL indev read
// callback in display.cpp on displayLoop's task. Reading/dispatching here too
// would put a second task on the touch controller's SPI bus (the same class of
// race this branch hit once). Kept as a stub because loop() still calls it, in
// case a future non-LVGL path needs it. The old pointInRect()/handleTouch()
// coordinate-dispatch this used to hold was removed as dead code.
void touchLoop() {}
#endif
