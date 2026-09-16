#pragma once
#include <Arduino.h>

// Second external probe: MAX31865 + 4-wire PT1000, sharing the touch SPI bus
// (SCLK=GPIO42, MISO=GPIO41, MOSI=GPIO2) with its own chip-select
// (BT2_CS_PIN, see pindef.h). Uses the shared Max31865Probe driver
// (max31865.h) -- same code as et_sensor.h, different instance/constants.
//
// This is now the reported "BT" (bean temperature) everywhere -- TC4 serial,
// WebSocket, BLE, the dashboard's BT tile, and (per user's choice) the PID
// control loop's input. The roaster's own built-in NTC still feeds
// `temp`/`ror` (SkiComms.h) exactly as before; it's reported separately as
// "NTC" (WebSocket only -- there's no room for a 6th field in TC4's fixed
// serial format) so that reading isn't lost, and it's what BT falls back to
// if this probe is absent or faults.

extern double bt2Temp; // last good BT reading, in the current display unit (C/F)
extern double bt2Ror;  // BT rate-of-rise, deg/min (same algorithm as ET/NTC)

// Call once, after touchInit() -- it shares the SPI bus touchInit() brings up.
void bt2SensorInit();

// Call frequently from the display task (same task that does the touch SPI
// reads / ET's tick, so the shared bus is never touched from two tasks at
// once). Self rate-limited internally; cheap on the calls in between.
void bt2SensorTick();

// True once a fault-free reading has been seen (probe present and sane).
bool bt2SensorHealthy();

// The value to report/control on as "BT": the real probe when healthy,
// otherwise NTC (the roaster's own probe) mirrored. PID control
// (handlePIDControl(), SkiCMD.h) reads this, so a lost/faulted probe falls
// back to the roaster's own sensor instead of freezing or feeding garbage
// into the control loop.
double bt2Report();

// Set the BT smoothing level, 0-100, following Artisan's TC4 FILT convention
// (the fraction kept from history each step -- higher = smoother and laggier;
// capped at 99 so it can't freeze). Default 70. Driven by the serial
// "FILT;<et>;<bt>;..." command's second value.
void bt2SetFilter(int filtPercent);

// Set the BT stage-1 median window: 1 (off) / 3 / 5 / 7 / 9. Driven by the
// touchscreen Smoothing screen (temp_smoothing.h).
void bt2SetMedianWindow(int window);
