#pragma once
#include <Arduino.h>

// True while the touch panel is actively driving the roaster (set on any
// button press, cleared by STOP). SkiCMD.h's itsbeentoolong() watchdog
// bypasses its normal inactivity timeout while this is true.
extern bool touchSessionActive;

void touchInit();
void touchLoop();
