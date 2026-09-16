#pragma once
#include <Arduino.h>

// ET (environment / exhaust) temperature from an external MAX31865 + PT100
// probe, wired onto the shared touch SPI bus (SCLK=GPIO42, MISO=GPIO41,
// MOSI=GPIO2) with its own chip-select (ET_CS_PIN, see pindef.h). Uses the
// shared Max31865Probe driver (max31865.h).
//
// Reported to Artisan in the TC4 READ reply's ET field and shown on the
// dashboard's ET / ET RoR tiles. When no probe is fitted or it faults, the
// ET field falls back to mirroring BT (bt2_sensor.h's bt2Report(), which
// itself falls back to NTC -- see bt2_sensor.h).

extern double etTemp; // last good ET reading, in the current display unit (C/F)
extern double etRor;  // ET rate-of-rise, deg/min (same algorithm as BT/NTC)

// Call once, after touchInit() -- it shares the SPI bus touchInit() brings up.
void etSensorInit();

// Call frequently from the display task (same task that does the touch SPI
// reads, so the shared bus is never touched from two tasks at once). Self
// rate-limited internally; cheap on the calls in between.
void etSensorTick();

// True once a fault-free reading has been seen (probe present and sane).
bool etSensorHealthy();

// The value to put in the TC4/Artisan ET field: the real probe when healthy,
// otherwise BT mirrored.
double etReport();

// Set the ET smoothing level, 0-100, following Artisan's TC4 FILT convention
// (the fraction kept from history each step -- higher = smoother and laggier;
// capped at 99 so it can't freeze). Default 70. Driven by the serial
// "FILT;<et>;<bt>;..." command; only the ET (first) value is used.
void etSetFilter(int filtPercent);
