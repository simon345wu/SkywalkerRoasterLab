#include "bt_probe.h"

double btTemp = 0.0;
double btRor = 0.0;

#if defined(S3)

#include "dlog.h"
#include "max31865.h"
#include "pindef.h"
#include <Preferences.h>

extern double temp; // NTC (roaster's own probe), from SkiComms.h
extern double ror;  // NTC rate-of-rise, from SkiComms.h
extern char CorF;   // 'C' / 'F', from SkiComms.h

// --- Breakout constants -- VERIFY on hardware ------------------------------
// Assumed 4300 ohm reference, same "PT100/PT1000 universal" board family as
// the ET probe (see et_probe.cpp) -- but that was only confirmed by reading
// a real RTD value on THAT specific board. This is a second, physically
// different breakout; check its actual reading against a known temperature
// once wired, and correct BT_RREF here if it doesn't land close. Unlike ET's
// PT100-on-4300-ohm mismatch, PT1000 on a 4300 ohm reference is the pairing
// this board family is actually designed for (PT1000 @ 25C is ~1097 ohm, a
// well-centered ~1/4 of the ADC range against 4300 ohm) -- so BT should read
// noticeably cleaner than ET does.
static const float BT_RREF = 4300.0f;
static const float BT_RNOMINAL = 1000.0f; // PT1000 probe

static Max31865Probe btProbe(BT_CS_PIN, BT_RREF, BT_RNOMINAL,
                              /*threeWire=*/false, LOG_BT, "[BT]");

// BT source selection (touchscreen), persisted in NVS.
static const char *kBtSrcNs = "btsrc";
static const char *kBtSrcKey = "src";
static BtSource s_btSource = BT_SRC_MAX31865;

// True when BT should currently use the external probe: MAX31865 is selected AND
// the probe is healthy. Otherwise BT (value + RoR) uses NTC -- this covers both
// the explicit NTC choice and an auto-fallback when the probe faults.
static bool btUsingProbe() {
  return s_btSource == BT_SRC_MAX31865 && btProbe.healthy();
}

void btSensorInit() {
  btProbe.init();
  Preferences prefs;
  prefs.begin(kBtSrcNs, /*readOnly=*/true);
  s_btSource =
      prefs.getUChar(kBtSrcKey, BT_SRC_MAX31865) == BT_SRC_NTC ? BT_SRC_NTC
                                                               : BT_SRC_MAX31865;
  prefs.end();
}

void btSensorTick() {
  btProbe.tick(CorF);
  btTemp = btProbe.temperature();               // raw probe temp
  btRor = btUsingProbe() ? btProbe.ror() : ror; // BT RoR follows the source
}

bool btSensorHealthy() { return btProbe.healthy(); }

double btReport() { return btUsingProbe() ? btTemp : temp; }

void btSetFilter(int filtPercent) { btProbe.setFilter(filtPercent); }

void btSetMedianWindow(int window) { btProbe.setMedianWindow(window); }

BtSource btGetSource() { return s_btSource; }

void btSetSource(BtSource source) {
  s_btSource = source;
  Preferences prefs;
  prefs.begin(kBtSrcNs, /*readOnly=*/false);
  prefs.putUChar(kBtSrcKey, (uint8_t)source);
  prefs.end();
  D_printf(LOG_BT, "[BT] source -> %s\n",
           source == BT_SRC_NTC ? "NTC" : "MAX31865");
}

#else // non-S3 builds: no BT probe, BT mirrors NTC (pre-sensor behaviour)

extern double temp;
void btSensorInit() {}
void btSensorTick() {}
bool btSensorHealthy() { return false; }
double btReport() { return temp; }
void btSetFilter(int filtPercent) {}
void btSetMedianWindow(int window) {}
BtSource btGetSource() { return BT_SRC_NTC; }
void btSetSource(BtSource source) {}

#endif
