#pragma once
#include "dlog.h"
#include "ror.h"
#include <Arduino.h>
#include <MedianFilterLib.h>
#include <SPI.h>

// Shared MAX31865 RTD-to-digital driver -- register-level I/O (not the
// Adafruit library; see max31865.cpp for why), Callendar-Van Dusen
// conversion, and two-stage smoothing (median spike-guard + EMA). Extracted
// from et_sensor.cpp when a second probe (bt2_sensor.cpp) was added, so both
// run the identical logic from separate instances instead of a hand-copied
// second copy that could drift -- same reasoning as ror.h's RorTracker.
//
// Each instance owns one chip-select pin and assumes the SPI bus it's wired
// to has already been begun elsewhere (see touchInit()). tick() must be
// called from whichever single task also drives that bus -- there's no
// locking here, by design (see et_sensor.h's original comment for why that's
// safe on this board).
class Max31865Probe {
public:
  Max31865Probe(int csPin, float rref, float rnominal, bool threeWire,
                LogCategory logCat, const char *tag,
                unsigned long sampleIntervalMs = 125);

  // Call once, after the shared SPI bus is already begun (e.g. touchInit()).
  void init();

  // Call frequently from the task that owns the SPI bus; self rate-limited
  // to sampleIntervalMs internally, so cheap on the calls in between. Needs
  // the current display unit ('C'/'F') to apply the right sanity-gate bounds
  // and convert the Celsius CVD result.
  void tick(char corF);

  bool healthy() const { return _healthy; }
  double temperature() const { return _temp; } // last good reading, current unit
  double ror() const { return _ror; }           // deg/min, same RorTracker algorithm

  // Artisan TC4 FILT convention: 0-100, the fraction of history kept each
  // step (higher = smoother/laggier); capped at 99 so it can't freeze.
  void setFilter(int filtPercent);

private:
  int _csPin;
  float _rref;
  float _rnominal;
  uint8_t _wireCfgBit;
  LogCategory _logCat;
  const char *_tag;
  unsigned long _sampleIntervalMs;

  SPISettings _spiSettings;
  RorTracker _rorTracker;
  MedianFilter<double> _filter; // stage 1: kills a single corrupted SPI read

  // Stage 2: EMA -- ema = prevWeight*ema + (1-prevWeight)*medianOut.
  float _emaPrevWeight = 0.70f;
  double _ema = 0.0;
  bool _emaSeeded = false;

  bool _inited = false;
  bool _healthy = false;
  unsigned long _lastSampleMs = 0;
  uint8_t _cfg = 0;
  double _temp = 0.0;
  double _ror = 0.0;

  void writeReg8(uint8_t addr, uint8_t value);
  uint8_t readReg8(uint8_t addr);
  uint16_t readReg16(uint8_t addr);
};
