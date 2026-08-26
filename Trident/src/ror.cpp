#include "ror.h"
#include "dlog.h"

// ROR: how fast temp is climbing, in degrees/min. Ported to match Artisan's
// own default algorithm (artisanlib/canvas.py's compute_ror_simple(),
// polyfitRoRcalc=false is the default and what skywalker.aset uses) instead
// of our earlier home-grown "average a bunch of noisy instantaneous
// slopes over 5s" approach, so the number shown here reads similarly to
// what Artisan itself will plot from the same temperature stream.
//
// Artisan's actual algorithm: one slope between *now* and a point
// ROR_SPAN_MS ago (skywalker.aset's DeltaSpan/DeltaETspan = 20s -- if that's
// ever changed in Artisan, update this to match). The "old" end of that
// slope is smoothed by locally averaging samples within
// ROR_LEFT_SMOOTH_MS of it -- deliberately *not* averaging the current/new
// end, so smoothing doesn't add lag to the most recent reading. Artisan
// does this as a 5-sample average at its own ~2s sampling rate (skywalker
// .aset's Delay=2000); we sample much faster (BT ~114ms via RMT, ET ~250ms),
// so the equivalent is expressed as a time window rather than a fixed sample
// count.
#define ROR_SPAN_MS 20000UL
#define ROR_LEFT_SMOOTH_MS 2000UL

void RorTracker::reset() {
  _count = 0;
  _head = 0;
  _ror = 0.0;
}

double RorTracker::update(double newTemp) {
  unsigned long now = millis();

  _hist[_head] = {now, newTemp};
  _head = (_head + 1) % HISTORY_SIZE;
  if (_count < HISTORY_SIZE) {
    _count++;
  }

  // Walk backward from the most recent sample to find the oldest one that's
  // still within ROR_SPAN_MS -- that's our anchor (Artisan's left_index).
  int anchorIdx = -1;
  for (int j = 0; j < _count; j++) {
    int idx = (_head - 1 - j + HISTORY_SIZE) % HISTORY_SIZE;
    if (now - _hist[idx].ms >= ROR_SPAN_MS) {
      anchorIdx = idx;
      break;
    }
  }
  if (anchorIdx == -1) {
    return _ror; // not ROR_SPAN_MS of history yet -- keep the last value
  }

  double timedSec = (now - _hist[anchorIdx].ms) / 1000.0;
  if (timedSec <= 0) {
    return _ror;
  }

  // Local average of samples near the anchor's timestamp (not sample
  // count, since our sampling rate is much finer than Artisan's).
  double leftSum = 0.0;
  int leftN = 0;
  for (int j = 0; j < _count; j++) {
    int idx = (_head - 1 - j + HISTORY_SIZE) % HISTORY_SIZE;
    long delta = (long)_hist[idx].ms - (long)_hist[anchorIdx].ms;
    if (delta > (long)ROR_LEFT_SMOOTH_MS) {
      continue; // still newer than the smoothing window, keep scanning back
    }
    if (delta < -(long)ROR_LEFT_SMOOTH_MS) {
      break; // now older than the smoothing window -- everything further back is too
    }
    leftSum += _hist[idx].temp;
    leftN++;
  }
  double leftAvg = leftN > 0 ? leftSum / leftN : _hist[anchorIdx].temp;

  _ror = (newTemp - leftAvg) / timedSec * 60.0;
  D_printf("ROR: %.2f /min (span %.1fs, %d-sample left avg)\n", _ror, timedSec,
           leftN);
  return _ror;
}
