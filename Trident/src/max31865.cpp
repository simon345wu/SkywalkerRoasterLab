#include "max31865.h"
#include <math.h>

// --- MAX31865 registers / config bits (datasheet) --------------------------
static const uint8_t REG_CONFIG = 0x00;
static const uint8_t REG_RTD_MSB = 0x01; // RTD LSB auto-follows at 0x02
static const uint8_t REG_FAULTSTAT = 0x07;
static const uint8_t CFG_BIAS = 0x80;
static const uint8_t CFG_MODEAUTO = 0x40;
static const uint8_t CFG_3WIRE = 0x10;
static const uint8_t CFG_FAULTCLEAR = 0x02;
// D0 (50/60Hz filter) left clear = 60Hz notch, matching Taiwan mains.

// Callendar-Van Dusen, copied from Adafruit_MAX31865::calculateTemperature().
#define RTD_A 3.9083e-3
#define RTD_B -5.775e-7
static float cvdTemperature(uint16_t rtdRaw, float rtdNominal,
                            float refResistor) {
  float Rt = rtdRaw;
  Rt /= 32768;
  Rt *= refResistor;

  float Z1 = -RTD_A;
  float Z2 = RTD_A * RTD_A - (4 * RTD_B);
  float Z3 = (4 * RTD_B) / rtdNominal;
  float Z4 = 2 * RTD_B;

  float t = Z2 + (Z3 * Rt);
  t = (sqrtf(t) + Z1) / Z4;
  if (t >= 0)
    return t;

  // Below 0C the equation above loses accuracy -- use the polynomial fit.
  Rt = rtdRaw;
  Rt /= 32768;
  Rt *= refResistor;
  Rt /= rtdNominal;
  Rt *= 100;

  float rpoly = Rt;
  t = -242.02;
  t += 2.2228 * rpoly;
  rpoly *= Rt;
  t += 2.5859e-3 * rpoly;
  rpoly *= Rt;
  t -= 4.8260e-6 * rpoly;
  rpoly *= Rt;
  t -= 2.8183e-8 * rpoly;
  rpoly *= Rt;
  t += 1.5243e-10 * rpoly;
  return t;
}

// 1 MHz, MSB first, SPI mode 1 -- exactly what Adafruit_MAX31865 uses. Applied
// per transaction, so it coexists on a bus shared with other devices running
// different clocks/modes (e.g. the XPT2046 touch controller's 2 MHz mode 0).
Max31865Probe::Max31865Probe(int csPin, float rref, float rnominal,
                             bool threeWire, LogCategory logCat,
                             const char *tag, unsigned long sampleIntervalMs)
    : _csPin(csPin), _rref(rref), _rnominal(rnominal),
      _wireCfgBit(threeWire ? CFG_3WIRE : 0), _logCat(logCat), _tag(tag),
      _sampleIntervalMs(sampleIntervalMs),
      _spiSettings(1000000, MSBFIRST, SPI_MODE1), _filter(7) {}

// --- Raw register I/O -----------------------------------------------------
// Deliberately not using Adafruit_MAX31865's own read path: its readRTD()
// hard-codes a bias-settle + one-shot delay(10)+delay(65) into every call,
// which would stall the display/LVGL task for ~75ms on each sample. In
// continuous auto-convert mode (set once in init()) the RTD register is
// always fresh, so a sample is just a 3-byte register read here -- tens of
// microseconds, no blocking.

void Max31865Probe::writeReg8(uint8_t addr, uint8_t value) {
  SPI.beginTransaction(_spiSettings);
  digitalWrite(_csPin, LOW);
  SPI.transfer(addr | 0x80); // MSB set = write
  SPI.transfer(value);
  digitalWrite(_csPin, HIGH);
  SPI.endTransaction();
}

uint8_t Max31865Probe::readReg8(uint8_t addr) {
  SPI.beginTransaction(_spiSettings);
  digitalWrite(_csPin, LOW);
  SPI.transfer(addr & 0x7F);
  uint8_t v = SPI.transfer(0xFF);
  digitalWrite(_csPin, HIGH);
  SPI.endTransaction();
  return v;
}

uint16_t Max31865Probe::readReg16(uint8_t addr) {
  SPI.beginTransaction(_spiSettings);
  digitalWrite(_csPin, LOW);
  SPI.transfer(addr & 0x7F);
  uint16_t v = SPI.transfer(0xFF);
  v <<= 8;
  v |= SPI.transfer(0xFF); // MAX31865 auto-increments to the LSB register
  digitalWrite(_csPin, HIGH);
  SPI.endTransaction();
  return v;
}

void Max31865Probe::init() {
  pinMode(_csPin, OUTPUT);
  digitalWrite(_csPin, HIGH);

  // VBIAS on + continuous auto-convert (~one conversion per mains cycle);
  // 60Hz filter (bit clear); wire count per the probe.
  _cfg = CFG_BIAS | CFG_MODEAUTO | _wireCfgBit;
  writeReg8(REG_CONFIG, _cfg);
  writeReg8(REG_CONFIG, _cfg | CFG_FAULTCLEAR); // clear any latched fault
  delay(10);                                    // one bias settle, at boot only

  _inited = true;
  // Config readback confirms SPI comms. Kept on WebSerial only -- anything on
  // the USB Serial here would contaminate Artisan's TC4 stream.
  D_printf(_logCat, "%s MAX31865 init: cfg wrote 0x%02X, readback 0x%02X\n",
           _tag, _cfg, readReg8(REG_CONFIG));
}

void Max31865Probe::tick(char corF) {
  if (!_inited) {
    return;
  }
  unsigned long now = millis();
  if (now - _lastSampleMs < _sampleIntervalMs) {
    return;
  }
  _lastSampleMs = now;

  // Periodic re-arm (~5s): re-assert VBIAS + continuous auto-convert and clear
  // any latched fault. This self-heals a brownout/glitch WITHOUT touching the
  // fault on every sample. The old per-sample path cleared the fault (which
  // disturbs the very next continuous conversion) AND re-seeded the EMA (so the
  // next good value snapped instead of being smoothed) -- together those were
  // the source of the occasional spikes. The spike-free reference build
  // (TEST_SkyCommand_Node32s) likewise ignores the per-sample fault bit and
  // just re-arms periodically.
  if (now - _lastArmMs >= 5000) {
    _lastArmMs = now;
    writeReg8(REG_CONFIG, _cfg);
    writeReg8(REG_CONFIG, _cfg | CFG_FAULTCLEAR);
  }

  // Continuous mode: the RTD register is always the latest conversion. Drop D0
  // (the fault flag) positionally and use the value directly -- do NOT branch on
  // the fault bit per sample. A genuinely bad reading is caught by the range
  // guard below instead.
  uint16_t rtd = readReg16(REG_RTD_MSB) >> 1;
  double c = cvdTemperature(rtd, _rnominal, _rref);
  double v = (corF == 'F') ? (c * 1.8 + 32.0) : c;

  // Range guard for a genuinely open/shorted probe (extreme value). Skip the
  // sample but KEEP the EMA state, so a single transient bad read never snaps
  // the smoothed output. Only after a *sustained* run of bad reads (>=3) do we
  // let the EMA re-seed, so a real probe recovery snaps rather than crawls.
  double maxV = (corF == 'F') ? 1100.0 : 600.0;
  if (v < -50.0 || v > maxV) {
    _healthy = false;
    if (++_badCount >= 3) {
      _emaSeeded = false;
    }
    D_printf(_logCat, "%s out-of-range reading skipped: %.1f\n", _tag, v);
    return;
  }
  _badCount = 0;

  // Stage 1: median-7 -> rejects short bursts (up to 3 of 7) of corrupted SPI
  // reads before the EMA. AddValue() returns the freshly computed median, so
  // read that directly. (For window 3 GetFiltered() would be stale -- its fast
  // path addValue3 skips updating _lastFiltered; the window-!=3 path used here
  // does update it, but reading AddValue()'s return keeps this independent of
  // the window size.)
  double m = _filter.AddValue(v);

  // Stage 2: EMA -> the actual noise damping, level set by setFilter().
  if (!_emaSeeded) {
    _ema = m;
    _emaSeeded = true;
  } else {
    _ema = _emaPrevWeight * _ema + (1.0f - _emaPrevWeight) * m;
  }

  _temp = _ema;
  _ror = _rorTracker.update(_temp);
  _healthy = true;
  D_printf(_logCat, "%s %.1f  RoR %.2f  (filt %.2f)\n", _tag, _temp, _ror,
           _emaPrevWeight);
}

void Max31865Probe::setFilter(int filtPercent) {
  if (filtPercent < 0) {
    filtPercent = 0;
  } else if (filtPercent > 99) {
    filtPercent = 99;
  }
  _emaPrevWeight = filtPercent / 100.0f;
  D_printf(_logCat, "%s FILT set to %d (EMA prev-weight %.2f)\n", _tag,
           filtPercent, _emaPrevWeight);
}
