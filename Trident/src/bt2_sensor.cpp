#include "bt2_sensor.h"

double bt2Temp = 0.0;
double bt2Ror = 0.0;

#if defined(S3)

#include "dlog.h"
#include "max31865.h"
#include "pindef.h"

extern double temp; // NTC (roaster's own probe), from SkiComms.h
extern char CorF;   // 'C' / 'F', from SkiComms.h

// --- Breakout constants -- VERIFY on hardware ------------------------------
// Assumed 4300 ohm reference, same "PT100/PT1000 universal" board family as
// the ET probe (see et_sensor.cpp) -- but that was only confirmed by reading
// a real RTD value on THAT specific board. This is a second, physically
// different breakout; check its actual reading against a known temperature
// once wired, and correct BT2_RREF here if it doesn't land close. Unlike ET's
// PT100-on-4300-ohm mismatch, PT1000 on a 4300 ohm reference is the pairing
// this board family is actually designed for (PT1000 @ 25C is ~1097 ohm, a
// well-centered ~1/4 of the ADC range against 4300 ohm) -- so BT should read
// noticeably cleaner than ET does.
static const float BT2_RREF = 4300.0f;
static const float BT2_RNOMINAL = 1000.0f; // PT1000 probe

static Max31865Probe bt2Probe(BT2_CS_PIN, BT2_RREF, BT2_RNOMINAL,
                              /*threeWire=*/false, LOG_BT2, "[BT2]");

void bt2SensorInit() { bt2Probe.init(); }

void bt2SensorTick() {
  bt2Probe.tick(CorF);
  bt2Temp = bt2Probe.temperature();
  bt2Ror = bt2Probe.ror();
}

bool bt2SensorHealthy() { return bt2Probe.healthy(); }

double bt2Report() { return bt2Probe.healthy() ? bt2Temp : temp; }

void bt2SetFilter(int filtPercent) { bt2Probe.setFilter(filtPercent); }

#else // non-S3 builds: no BT2 probe, BT mirrors NTC (pre-sensor behaviour)

extern double temp;
void bt2SensorInit() {}
void bt2SensorTick() {}
bool bt2SensorHealthy() { return false; }
double bt2Report() { return temp; }
void bt2SetFilter(int filtPercent) {}

#endif
