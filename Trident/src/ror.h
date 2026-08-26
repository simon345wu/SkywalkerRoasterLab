#pragma once
#include <Arduino.h>

// Rate-of-rise tracker -- one instance per temperature channel (BT, ET, ...).
// Implements Artisan's own default ROR algorithm; see ror.cpp for the full
// rationale. Previously this lived as a set of file-scope globals + updateROR()
// in SkiComms.h and only served BT; pulled out into a class here so the ET
// channel (et_sensor.cpp) gets the identical calculation instead of a
// hand-copied second version that could drift.
class RorTracker {
public:
  // Feed a new (already filtered) temperature sample. Updates and returns the
  // rate of rise in degrees/min; returns the previous value until there is at
  // least ROR_SPAN_MS of history.
  double update(double newTemp);
  double value() const { return _ror; }
  void reset();

private:
  // 256 comfortably covers ROR_SPAN_MS at BT's ~114ms roaster sample rate
  // (and ET's slower cadence needs far less).
  static const int HISTORY_SIZE = 256;
  struct Sample {
    unsigned long ms;
    double temp;
  };
  Sample _hist[HISTORY_SIZE];
  int _count = 0;
  int _head = 0; // index one past the most recently written sample
  double _ror = 0.0;
};
