#pragma once

// Global ET/BT temperature smoothing settings, persisted in NVS and applied
// live (both probes share one setting -- Phase 1 scope; NTC is not covered
// yet). Two independent stages:
//   stage 1 = median window  (spike guard)
//   stage 2 = EMA weight      (noise damping)
// Selected from the touchscreen Smoothing screen (display.cpp). Changes apply
// immediately (sensor ticks and the LVGL callbacks share one task, so no
// locking and no reboot is needed). Artisan's serial FILT command can still
// override the EMA weight live for a session -- both just write the probe's
// EMA weight, last-write-wins.

// Current persisted values.
int tempSmoothingMedian();   // median window: 1 (off) / 3 / 5 / 7 / 9
int tempSmoothingEmaX100();  // EMA weight * 100: 0 (off) / 50 / 70 / 80 / 90

// Change one stage: persists to NVS and pushes it to ET + BT immediately.
void tempSmoothingSetMedian(int window);
void tempSmoothingSetEmaX100(int emaX100);

// Load persisted values from NVS and push them to ET + BT. Call once at boot,
// after etSensorInit()/btSensorInit() and before the Smoothing screen is built
// (so its buttons show the right current values).
void tempSmoothingApply();
