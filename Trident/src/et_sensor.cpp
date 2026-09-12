#include "et_sensor.h"

double etTemp = 0.0;
double etRor = 0.0;

#if defined(S3)

#include "dlog.h"
#include "pindef.h"
#include "ror.h"
#include <MedianFilterLib.h>
#include <SPI.h>
#include <math.h>

extern double temp; // BT, from SkiComms.h (compiled into main.cpp's TU)
extern char CorF;    // 'C' / 'F', from SkiComms.h

// --- Breakout constants ----------------------------------------------------
// Reference resistor on the breakout. This board (a "PT100/PT1000 universal"
// red board) has a 4300 ohm reference, not the 430 ohm an Adafruit PT100
// board uses -- confirmed on hardware 2026-08-26: with RREF=430 a room-temp
// PT100 read rtd=844 -> Rt=11 ohm -> -217C (garbage); with RREF=4300 the same
// rtd gives Rt=110.7 ohm -> ~27C (correct). Trade-off: the PT100 only spans
// ~1/10th of the ADC range against a 4300 ohm reference, so ET resolution is
// ~0.35C/count over 0-400C (vs ~0.035C on a 430 ohm board). Fine for exhaust
// temp; swap in a 430 ohm-reference board if finer ET is ever wanted.
static const float ET_RREF = 4300.0f;
static const float ET_RNOMINAL = 100.0f; // PT100 probe
static const bool ET_THREE_WIRE = false; // 4-wire probe

// 125ms ~= BT's ~114ms roaster frame rate, so the median-7 filter below
// covers roughly the same ~0.9s time window as BT's does (was 250ms, which
// made ET noticeably laggier than BT). Still well above the MAX31865's ~21ms
// continuous-conversion time, so every read gets a fresh conversion.
static const unsigned long ET_SAMPLE_INTERVAL_MS = 125;

// --- MAX31865 registers / config bits (datasheet) --------------------------
static const uint8_t REG_CONFIG = 0x00;
static const uint8_t REG_RTD_MSB = 0x01;   // RTD LSB auto-follows at 0x02
static const uint8_t REG_FAULTSTAT = 0x07;
static const uint8_t CFG_BIAS = 0x80;
static const uint8_t CFG_MODEAUTO = 0x40;
static const uint8_t CFG_3WIRE = 0x10;
static const uint8_t CFG_FAULTCLEAR = 0x02;
// D0 (50/60Hz filter) left clear = 60Hz notch, matching Taiwan mains.

// 1 MHz, MSB first, SPI mode 1 -- exactly what Adafruit_MAX31865 uses. Applied
// per transaction, so it coexists on the shared bus with the touch
// controller's 2 MHz / mode 0 transactions (see et_sensor.h).
static const SPISettings ET_SPI_SETTINGS(1000000, MSBFIRST, SPI_MODE1);

static RorTracker etRorTracker;

// Two-stage smoothing: a short median just to drop the odd corrupted SPI read
// (single-sample spike), then an EMA that does the actual noise damping. The
// EMA level is Artisan-TC4-FILT-settable at runtime (see etSetFilter()); the
// median stays fixed and small so it adds almost no lag.
static MedianFilter<double> etFilter(3);

// EMA: etEma = prevWeight*etEma + (1-prevWeight)*medianOut. prevWeight follows
// the Artisan TC4 FILT convention -- the fraction kept from history, so higher
// = smoother and laggier. Default 0.70 (== "FILT;70"); FILT;<n> overrides it.
static float etEmaPrevWeight = 0.70f;
static double etEma = 0.0;
static bool etEmaSeeded = false;

static bool inited = false;
static bool healthy = false;
static unsigned long lastSampleMs = 0;
static uint8_t etCfg = 0;

// --- Raw register I/O -----------------------------------------------------
// Deliberately not using Adafruit_MAX31865's own read path: its readRTD()
// hard-codes a bias-settle + one-shot delay(10)+delay(65) into every call,
// which would stall the display/LVGL task for ~75ms on each sample. In
// continuous auto-convert mode (set once in etSensorInit()) the RTD register
// is always fresh, so a sample is just a 3-byte register read here -- tens of
// microseconds, no blocking. The Callendar-Van Dusen math below is copied
// verbatim from that library so the conversion still matches.

static void etWriteReg8(uint8_t addr, uint8_t value) {
  SPI.beginTransaction(ET_SPI_SETTINGS);
  digitalWrite(ET_CS_PIN, LOW);
  SPI.transfer(addr | 0x80); // MSB set = write
  SPI.transfer(value);
  digitalWrite(ET_CS_PIN, HIGH);
  SPI.endTransaction();
}

static uint8_t etReadReg8(uint8_t addr) {
  SPI.beginTransaction(ET_SPI_SETTINGS);
  digitalWrite(ET_CS_PIN, LOW);
  SPI.transfer(addr & 0x7F);
  uint8_t v = SPI.transfer(0xFF);
  digitalWrite(ET_CS_PIN, HIGH);
  SPI.endTransaction();
  return v;
}

static uint16_t etReadReg16(uint8_t addr) {
  SPI.beginTransaction(ET_SPI_SETTINGS);
  digitalWrite(ET_CS_PIN, LOW);
  SPI.transfer(addr & 0x7F);
  uint16_t v = SPI.transfer(0xFF);
  v <<= 8;
  v |= SPI.transfer(0xFF); // MAX31865 auto-increments to the LSB register
  digitalWrite(ET_CS_PIN, HIGH);
  SPI.endTransaction();
  return v;
}

// Callendar-Van Dusen, copied from Adafruit_MAX31865::calculateTemperature().
#define RTD_A 3.9083e-3
#define RTD_B -5.775e-7
static float cvdTemperature(uint16_t rtdRaw, float rtdNominal, float refResistor) {
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

void etSensorInit() {
  // touchInit() already ran SPI.begin(SCLK,MISO,MOSI,SS) for the shared bus.
  pinMode(ET_CS_PIN, OUTPUT);
  digitalWrite(ET_CS_PIN, HIGH);

  // VBIAS on + continuous auto-convert (~one conversion per mains cycle);
  // 60Hz filter (bit clear); wire count per the probe.
  etCfg = CFG_BIAS | CFG_MODEAUTO;
  if (ET_THREE_WIRE) {
    etCfg |= CFG_3WIRE;
  }
  etWriteReg8(REG_CONFIG, etCfg);
  etWriteReg8(REG_CONFIG, etCfg | CFG_FAULTCLEAR); // clear any latched fault
  delay(10);                                       // one bias settle, at boot only

  inited = true;
  // Config readback confirms SPI comms (checked on hardware 2026-08-26:
  // wrote 0xC0, read back 0xC0). Kept on WebSerial only -- anything on the USB
  // Serial here would contaminate Artisan's TC4 stream.
  D_printf(LOG_ET, "[ET] MAX31865 init: cfg wrote 0x%02X, readback 0x%02X\n",
           etCfg, etReadReg8(REG_CONFIG));
}

void etSensorTick() {
  if (!inited) {
    return;
  }
  unsigned long now = millis();
  if (now - lastSampleMs < ET_SAMPLE_INTERVAL_MS) {
    return;
  }
  lastSampleMs = now;

  uint16_t raw = etReadReg16(REG_RTD_MSB);
  bool faultFlagged = raw & 0x0001;
  uint16_t rtd = raw >> 1; // drop D0 (fault flag) -> 15-bit ratio

  if (faultFlagged) {
    uint8_t fault = etReadReg8(REG_FAULTSTAT);
    etWriteReg8(REG_CONFIG, etCfg | CFG_FAULTCLEAR);
    healthy = false;
    etEmaSeeded = false; // re-seed on recovery so it snaps, not crawls
    D_printf(LOG_ET, "[ET] MAX31865 fault 0x%02X\n", fault);
    return;
  }

  double c = cvdTemperature(rtd, ET_RNOMINAL, ET_RREF);
  double v = (CorF == 'F') ? (c * 1.8 + 32.0) : c;

  // Same spirit as filtTemp()'s BT sanity gate: reject blatantly bogus values
  // (open probe, corrupted read) before the filter / ROR / Artisan see them.
  double maxV = (CorF == 'F') ? 1100.0 : 600.0;
  if (v < -50.0 || v > maxV) {
    healthy = false;
    etEmaSeeded = false;
    D_printf(LOG_ET, "[ET] out-of-range reading ignored: %.1f\n", v);
    return;
  }

  // Stage 1: short median -> kills a single corrupted SPI read. Must use
  // AddValue()'s return, NOT GetFiltered(): MedianFilterLib's window-3 fast
  // path (addValue3) never updates the field GetFiltered() reads, so it would
  // always return 0 here.
  double m = etFilter.AddValue(v);

  // Stage 2: EMA -> the actual noise damping, level set by FILT.
  if (!etEmaSeeded) {
    etEma = m;
    etEmaSeeded = true;
  } else {
    etEma = etEmaPrevWeight * etEma + (1.0f - etEmaPrevWeight) * m;
  }

  etTemp = etEma;
  etRor = etRorTracker.update(etTemp);
  healthy = true;
  D_printf(LOG_ET, "[ET] %.1f  RoR %.2f  (filt %.2f)\n", etTemp, etRor,
           etEmaPrevWeight);
}

bool etSensorHealthy() { return healthy; }

double etReport() { return healthy ? etTemp : temp; }

void etSetFilter(int filtPercent) {
  // Artisan TC4 FILT convention: 0-100, the fraction kept from history (higher
  // = smoother/laggier). 100 would freeze the reading, so cap at 99.
  if (filtPercent < 0) {
    filtPercent = 0;
  } else if (filtPercent > 99) {
    filtPercent = 99;
  }
  etEmaPrevWeight = filtPercent / 100.0f;
  D_printf(LOG_ET, "[ET] FILT set to %d (EMA prev-weight %.2f)\n", filtPercent,
           etEmaPrevWeight);
}

#else // non-S3 builds: no ET probe, ET mirrors BT (pre-sensor behaviour)

extern double temp;
void etSensorInit() {}
void etSensorTick() {}
bool etSensorHealthy() { return false; }
double etReport() { return temp; }
void etSetFilter(int filtPercent) {}

#endif
