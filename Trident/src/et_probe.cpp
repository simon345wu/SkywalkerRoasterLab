#include "et_probe.h"

double etTemp = 0.0;
double etRor = 0.0;

#if defined(S3)

#include "bt_probe.h" // fallback chain: ET -> BT -> NTC
#include "max31865.h"
#include "pindef.h"

extern char CorF; // 'C' / 'F', from SkiComms.h

// --- Breakout constants ----------------------------------------------------
// Reference resistor on the breakout. This board (a "PT100/PT1000 universal"
// red board) has a 4300 ohm reference, not the 430 ohm an Adafruit PT100
// board uses -- confirmed on hardware 2026-08-26: with RREF=430 a room-temp
// PT100 read rtd=844 -> Rt=11 ohm -> -217C (garbage); with RREF=4300 the same
// rtd gives Rt=110.7 ohm -> ~27C (correct). Trade-off: the PT100 only spans
// ~1/10th of the ADC range against a 4300 ohm reference, so ET resolution is
// ~0.35C/count over 0-400C (vs ~0.035C on a 430 ohm board). Fine for exhaust
// temp; swap in a 430 ohm-reference board if finer ET is ever wanted.
static Max31865Probe etProbe(ET_CS_PIN, /*rref=*/4300.0f, /*rnominal=*/100.0f,
                             /*threeWire=*/false, LOG_ET, "[ET]");

void etSensorInit() { etProbe.init(); }

void etSensorTick() {
  etProbe.tick(CorF);
  etTemp = etProbe.temperature();
  etRor = etProbe.ror();
}

bool etSensorHealthy() { return etProbe.healthy(); }

double etReport() { return etProbe.healthy() ? etTemp : btReport(); }

void etSetFilter(int filtPercent) { etProbe.setFilter(filtPercent); }

void etSetMedianWindow(int window) { etProbe.setMedianWindow(window); }

#else // non-S3 builds: no ET probe, ET mirrors BT (pre-sensor behaviour)

extern double temp;
void etSensorInit() {}
void etSensorTick() {}
bool etSensorHealthy() { return false; }
double etReport() { return temp; }
void etSetFilter(int filtPercent) {}
void etSetMedianWindow(int window) {}

#endif
