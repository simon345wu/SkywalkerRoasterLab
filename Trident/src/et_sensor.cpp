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

static const unsigned long ET_SAMPLE_INTERVAL_MS = 250;

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
// Median-7, same as BT's tempFilter -- rejects corrupted SPI reads and damps
// the raw ADC noise (this board's 4300 ohm reference leaves a PT100 using only
// ~1/10th of the range, so raw counts jitter ~+/-1C; see ET_RREF).
static MedianFilter<double> etFilter(7);
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
  D_printf("[ET] MAX31865 init: cfg wrote 0x%02X, readback 0x%02X\n", etCfg,
           etReadReg8(REG_CONFIG));
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
    D_printf("[ET] MAX31865 fault 0x%02X\n", fault);
    return;
  }

  double c = cvdTemperature(rtd, ET_RNOMINAL, ET_RREF);
  double v = (CorF == 'F') ? (c * 1.8 + 32.0) : c;

  // Same spirit as filtTemp()'s BT sanity gate: reject blatantly bogus values
  // (open probe, corrupted read) before the filter / ROR / Artisan see them.
  double maxV = (CorF == 'F') ? 1100.0 : 600.0;
  if (v < -50.0 || v > maxV) {
    healthy = false;
    D_printf("[ET] out-of-range reading ignored: %.1f\n", v);
    return;
  }

  etFilter.AddValue(v);
  etTemp = etFilter.GetFiltered();
  etRor = etRorTracker.update(etTemp);
  healthy = true;
  D_printf("[ET] %.1f  RoR %.2f\n", etTemp, etRor);
}

bool etSensorHealthy() { return healthy; }

double etReport() { return healthy ? etTemp : temp; }

#else // non-S3 builds: no ET probe, ET mirrors BT (pre-sensor behaviour)

extern double temp;
void etSensorInit() {}
void etSensorTick() {}
bool etSensorHealthy() { return false; }
double etReport() { return temp; }

#endif
