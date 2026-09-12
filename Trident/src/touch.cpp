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
const int STEP = 5;
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

bool pointInRect(int x, int y, int rx, int ry, int rw, int rh) {
  return x >= rx && x < (rx + rw) && y >= ry && y < (ry + rh);
}

uint8_t clampPercent(int value) {
  if (value < 0)
    return 0;
  if (value > 100)
    return 100;
  return (uint8_t)value;
}

void handleTouch(int screenX, int screenY) {
  StateRequestT current = getCurrentState();

  if (pointInRect(screenX, screenY, BTN_FAN_ZERO_X, BTN_FAN_ROW_Y, BTN_WIDTH,
                   BTN_HEIGHT)) {
    sendFan(0);
  } else if (pointInRect(screenX, screenY, BTN_FAN_MINUS_X, BTN_FAN_ROW_Y,
                          BTN_WIDTH, BTN_HEIGHT)) {
    sendFan(current.fan - STEP);
  } else if (pointInRect(screenX, screenY, BTN_FAN_PLUS_X, BTN_FAN_ROW_Y,
                          BTN_WIDTH, BTN_HEIGHT)) {
    sendFan(current.fan + STEP);
  } else if (pointInRect(screenX, screenY, BTN_FAN_MAX_X, BTN_FAN_ROW_Y,
                          BTN_WIDTH, BTN_HEIGHT)) {
    sendFan(100);
  } else if (pointInRect(screenX, screenY, BTN_HEAT_ZERO_X, BTN_HEAT_ROW_Y,
                          BTN_WIDTH, BTN_HEIGHT)) {
    sendHeat(0);
  } else if (pointInRect(screenX, screenY, BTN_HEAT_MINUS_X, BTN_HEAT_ROW_Y,
                          BTN_WIDTH, BTN_HEIGHT)) {
    sendHeat(current.heater - STEP);
  } else if (pointInRect(screenX, screenY, BTN_HEAT_PLUS_X, BTN_HEAT_ROW_Y,
                          BTN_WIDTH, BTN_HEIGHT)) {
    sendHeat(current.heater + STEP);
  } else if (pointInRect(screenX, screenY, BTN_HEAT_MAX_X, BTN_HEAT_ROW_Y,
                          BTN_WIDTH, BTN_HEIGHT)) {
    sendHeat(100);
  } else if (pointInRect(screenX, screenY, BTN_DRUM_X, BTN_DRUM_Y,
                          BTN_DRUM_WIDTH, BTN_DRUM_HEIGHT)) {
    sendDrum(current.drum != 0 ? 0 : 100);
  } else if (pointInRect(screenX, screenY, BTN_COOL_X, BTN_COOL_Y,
                          BTN_COOL_WIDTH, BTN_COOL_HEIGHT)) {
    sendCool(current.cooling != 0 ? 0 : 100);
  } else if (pointInRect(screenX, screenY, BTN_STOP_X, BTN_STOP_Y,
                          BTN_STOP_WIDTH, BTN_STOP_HEIGHT)) {
    D_println(LOG_TOUCH, "Touch: STOP pressed");
    sendStop();
  }
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

// lvgl-ui branch: deliberately a no-op for now, not deleted. The old
// dispatch below (handleTouch() -> sendFan()/sendHeat()/.../sendStop() ->
// enqueueStateRequest()) fires real heater/fan/drum/cooling commands, but
// this branch's screen no longer draws those buttons -- reading the panel
// here and dispatching against those (now invisible) button coordinates
// would let a tap silently drive real hardware blind. Touch reads now also
// happen from the LVGL indev read callback in display.cpp, on displayLoop's
// task; calling touchGetPoint() here too would mean two tasks hitting the
// touch controller's SPI bus concurrently -- same class of problem as the
// lv_timer_handler() race this branch already hit once. Once the real
// dashboard is rebuilt in LVGL, this dispatch either moves to LVGL widget
// event callbacks (most likely) or this loop resumes calling
// touchGetPoint() itself if LVGL input isn't in the picture for it.
void touchLoop() {}
#endif
