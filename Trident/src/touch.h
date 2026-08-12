#pragma once
#include <Arduino.h>

// True while the touch panel is actively driving the roaster (set on any
// button press, cleared by STOP). SkiCMD.h's itsbeentoolong() watchdog
// bypasses its normal inactivity timeout while this is true.
extern bool touchSessionActive;

void touchInit();
void touchLoop();

// Raw calibrated read, level-based (not edge-triggered like touchLoop()'s
// internal dispatch) -- reports "touched right now, and where" on every
// call. Shared by touchLoop() and (lvgl-ui branch) the LVGL indev read
// callback in display.cpp, so there's exactly one place owning the
// touch-controller SPI reads and the screen-calibration constants.
bool touchGetPoint(int *screenX, int *screenY);

// The actual heater/fan/drum/cooling dispatch, through the normal
// arbitration queue (enqueueStateRequest(), SOURCE_TOUCH) -- same functions
// the old pointInRect() button dispatch used, now also called directly by
// the lvgl-ui branch's dashboard widgets (display.cpp) since that dashboard
// bypasses hit-testing entirely (LVGL already knows which widget was
// touched). newValue is a percentage, 0-100 (out-of-range values are
// clamped).
void sendFan(int newValue);
void sendHeat(int newValue);
void sendDrum(uint8_t newValue);
void sendCool(uint8_t newValue);
void sendStop();
