#include "temp_smoothing.h"
#include "bt2_sensor.h"
#include "et_sensor.h"
#include <Preferences.h>

static const char *kNamespace = "tsmooth";
static const char *kMedianKey = "med";
static const char *kEmaKey = "ema";

// Defaults match the prior hard-coded behaviour: median-7, EMA weight 0.70.
static int s_median = 7;
static int s_emaX100 = 70;

static void save() {
  Preferences prefs;
  prefs.begin(kNamespace, /*readOnly=*/false);
  prefs.putInt(kMedianKey, s_median);
  prefs.putInt(kEmaKey, s_emaX100);
  prefs.end();
}

int tempSmoothingMedian() { return s_median; }
int tempSmoothingEmaX100() { return s_emaX100; }

void tempSmoothingApply() {
  Preferences prefs;
  prefs.begin(kNamespace, /*readOnly=*/true);
  s_median = prefs.getInt(kMedianKey, s_median);
  s_emaX100 = prefs.getInt(kEmaKey, s_emaX100);
  prefs.end();

  etSetMedianWindow(s_median);
  bt2SetMedianWindow(s_median);
  etSetFilter(s_emaX100);   // FILT convention is EMA weight * 100
  bt2SetFilter(s_emaX100);
}

void tempSmoothingSetMedian(int window) {
  s_median = window;
  save();
  etSetMedianWindow(window);
  bt2SetMedianWindow(window);
}

void tempSmoothingSetEmaX100(int emaX100) {
  s_emaX100 = emaX100;
  save();
  etSetFilter(emaX100);
  bt2SetFilter(emaX100);
}
