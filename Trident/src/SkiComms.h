#include "dlog.h"
#include "pindef.h"
#include "ror.h"
#include <MedianFilterLib.h>
#include <cstdint>

#ifdef _ROASTER_RX_RMT_
#include "driver/rmt_rx.h"
#endif
#ifdef _ROASTER_TX_RMT_
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#endif

// -----------------------------------------------------------------------------
// Timing Constants
// -----------------------------------------------------------------------------
const int PREAMBLE = 7000;
const int PULSE_ONE = 1150;
const int PULSE_ZERO = 650;
const int POST_PULSE_DELAY = 750;
const int START_PULSE = 7500;
const int START_DELAY = 3800;

// -----------------------------------------------------------------------------
// Buffer Sizes
// -----------------------------------------------------------------------------
const int ROASTER_LENGTH = 7;    // 7 bytes received from roaster
const int CONTROLLER_LENGTH = 6; // 6 bytes sent to roaster

// -----------------------------------------------------------------------------
// Allocate buffers
// -----------------------------------------------------------------------------
uint8_t receiveBuffer[ROASTER_LENGTH];
uint8_t sendBuffer[CONTROLLER_LENGTH];

// -----------------------------------------------------------------------------
// Control Byte Indices
// -----------------------------------------------------------------------------
enum ControlBytes {
  VENT_BYTE = 0,
  DRUM_BYTE = 3,
  COOL_BYTE = 2,
  FILTER_BYTE = 1,
  HEAT_BYTE = 4,
  CHECK_BYTE = 5
};

// Raw temperature values
uint16_t rawTempX, rawTempY;
char CorF = 'C'; // 'C' or 'F'

// Global temp value
double extern temp;

// Global ROR (rate of rise) value, degrees/min
double extern ror;

void pulsePin(int pin, int duration) {
  digitalWrite(pin, LOW);
  delayMicroseconds(duration);
  digitalWrite(pin, HIGH);
}

// Control Bytes & Checksum
void setControlChecksum() {
  uint8_t sum = 0;
  for (int i = 0; i < (CONTROLLER_LENGTH - 1); i++) {
    sum += sendBuffer[i];
  }
  sendBuffer[CHECK_BYTE] = sum; // Correct use of CHECK_BYTE
}

void setValue(uint8_t *bytePtr, uint8_t value) {
  *bytePtr = value;
  setControlChecksum();
}

#ifdef _ROASTER_TX_RMT_
// -----------------------------------------------------------------------------
// Roaster TX via RMT hardware -- replaces the delayMicroseconds() bit-bang
// below. Same waveform timing as the bit-banged version (each bit =
// {LOW, HIGH 750}; preamble = {LOW 7500, HIGH 3800}), just hardware-generated
// so the CPU isn't busy-waiting through it. Ported from
// TEST_SkyCommand_Node32s's rmt-roaster-rx branch.
// -----------------------------------------------------------------------------
static rmt_channel_handle_t roasterTxChan = NULL;
static rmt_encoder_handle_t roasterCopyEnc = NULL;

void initRoasterTxRMT() {
  rmt_tx_channel_config_t txCfg = {};
  txCfg.gpio_num = (gpio_num_t)TX_PIN;
  txCfg.clk_src = RMT_CLK_SRC_DEFAULT;
  txCfg.resolution_hz = 1000000; // 1 tick = 1us
  txCfg.mem_block_symbols = 64;  // one frame is 1+48=49 symbols < 64
  txCfg.trans_queue_depth = 4;
  if (rmt_new_tx_channel(&txCfg, &roasterTxChan) != ESP_OK) {
    D_println(LOG_ROASTER, "[RMT] roaster TX channel init failed");
    return;
  }
  rmt_copy_encoder_config_t encCfg = {};
  rmt_new_copy_encoder(&encCfg, &roasterCopyEnc);
  rmt_enable(roasterTxChan);
}

void extern sendRoasterMessage() {
  rmt_symbol_word_t sym[1 + CONTROLLER_LENGTH * 8];
  int k = 0;
  sym[k].level0 = 0; sym[k].duration0 = START_PULSE; // preamble
  sym[k].level1 = 1; sym[k].duration1 = START_DELAY; k++;
  for (int i = 0; i < CONTROLLER_LENGTH; i++) {
    for (int j = 0; j < 8; j++) { // LSB first, matches the bit-bang version
      uint16_t low = bitRead(sendBuffer[i], j) ? 1500 : PULSE_ZERO;
      sym[k].level0 = 0; sym[k].duration0 = low;
      sym[k].level1 = 1; sym[k].duration1 = POST_PULSE_DELAY; k++;
    }
  }

  rmt_transmit_config_t txc = {};
  txc.loop_count = 0;
  txc.flags.eot_level = 1; // leave HIGH (idle) once the frame is sent
  rmt_transmit(roasterTxChan, roasterCopyEnc, sym, k * sizeof(rmt_symbol_word_t), &txc);
  rmt_tx_wait_all_done(roasterTxChan, 200); // block until this frame is fully sent (~78ms); yields the task instead of busy-waiting
}
#else
void extern sendRoasterMessage() {
  // D_println("sending message to roaster");
  // Start pulse
  pulsePin(TX_PIN, START_PULSE);
  delayMicroseconds(START_DELAY);

  // Send each byte, bit by bit
  for (int i = 0; i < CONTROLLER_LENGTH; i++) {
    for (int j = 0; j < 8; j++) {
      if (bitRead(sendBuffer[i], j) == 1) {
        // '1' bit
        pulsePin(TX_PIN, 1500);
      } else {
        // '0' bit
        pulsePin(TX_PIN, PULSE_ZERO);
      }
      delayMicroseconds(POST_PULSE_DELAY);
    }
  }
}
#endif // _ROASTER_TX_RMT_

#ifdef _ROASTER_RX_RMT_
// -----------------------------------------------------------------------------
// Roaster RX via RMT hardware -- replaces the interrupt + blocking pulseIn()
// approach below. The peripheral captures pulse widths in hardware, so
// timing isn't affected by WiFi/BLE interrupt jitter the way a software ISR
// + pulseIn() is. Ported from TEST_SkyCommand_Node32s's rmt-roaster-rx
// branch, validated there on real hardware.
//
// Key trick: Arduino's rmtRead() wrapper only supports single-shot capture
// and deadlocks ("partial receive not supported") on a continuous stream
// like the roaster's. The on_recv_done callback re-arms rmt_receive()
// immediately so the peripheral keeps capturing continuously.
// -----------------------------------------------------------------------------
#define RMT_MAXSYM 128
static const uint32_t PRE_MIN = 6000, PRE_MAX = 9000; // preamble LOW window (us); bit threshold reuses PULSE_ONE

static rmt_channel_handle_t roasterRxChan = NULL;
static rmt_receive_config_t roasterRxCfg;
static rmt_symbol_word_t rmtRawBuf[RMT_MAXSYM];   // rmt_receive() writes here
static rmt_symbol_word_t rmtReadyBuf[RMT_MAXSYM]; // callback's handoff copy for the consumer
static volatile size_t rmtReadyNum = 0;
static volatile bool rmtFrameReady = false; // single-producer(callback)/single-consumer(getRoasterMessage) barrier

// ISR context: copy the frame out and re-arm immediately so the hardware
// keeps receiving. Do NOT mark the rmt_symbol_word_t array volatile -- it's
// a bitfield struct and a volatile assignment won't compile.
static bool IRAM_ATTR onRoasterRecvDone(rmt_channel_handle_t ch,
                                         const rmt_rx_done_event_data_t *ed,
                                         void *user) {
  if (!rmtFrameReady) { // only take a new frame once the previous one has been consumed
    size_t n = ed->num_symbols;
    if (n > RMT_MAXSYM) n = RMT_MAXSYM;
    for (size_t i = 0; i < n; i++) rmtReadyBuf[i] = ed->received_symbols[i];
    rmtReadyNum = n;
    rmtFrameReady = true;
  }
  rmt_receive(ch, rmtRawBuf, sizeof(rmtRawBuf), &roasterRxCfg);
  return false;
}

// Decodes one frame of RMT symbols into receiveBuffer (ROASTER_LENGTH bytes,
// LSB first). Returns the number of bits decoded, or 0 if no preamble found.
static int decodeRoasterFrame(const rmt_symbol_word_t *buf, size_t n) {
  memset(receiveBuffer, 0, ROASTER_LENGTH);
  int bit = 0;
  bool started = false;
  for (size_t i = 0; i < n; i++) {
    for (int half = 0; half < 2; half++) {
      uint32_t lvl = half == 0 ? buf[i].level0 : buf[i].level1;
      uint32_t dur = half == 0 ? buf[i].duration0 : buf[i].duration1;
      if (dur == 0) continue;
      if (lvl != 0) continue; // only LOW pulses carry timing info
      if (!started) {
        if (dur > PRE_MIN && dur < PRE_MAX) started = true;
      } else if (bit < ROASTER_LENGTH * 8) {
        if (dur > (uint32_t)PULSE_ONE) receiveBuffer[bit / 8] |= (1 << (bit % 8));
        bit++;
      }
    }
  }
  return started ? bit : 0;
}

void initRoasterRMT() {
  rmt_rx_channel_config_t chCfg = {};
  chCfg.gpio_num = (gpio_num_t)RX_PIN;
  chCfg.clk_src = RMT_CLK_SRC_DEFAULT;
  chCfg.resolution_hz = 1000000; // 1 tick = 1us
  chCfg.mem_block_symbols = RMT_MAXSYM;
  if (rmt_new_rx_channel(&chCfg, &roasterRxChan) != ESP_OK) {
    D_println(LOG_ROASTER, "[RMT] roaster RX channel init failed");
    return;
  }
  rmt_rx_event_callbacks_t cbs = {};
  cbs.on_recv_done = onRoasterRecvDone;
  rmt_rx_register_event_callbacks(roasterRxChan, &cbs, NULL);
  rmt_enable(roasterRxChan);
  roasterRxCfg.signal_range_min_ns = 2000;    // 2us glitch filter (ESP32 filter cap is ~3.19us)
  roasterRxCfg.signal_range_max_ns = 8000000; // 8ms with no edge = end of frame
  rmt_receive(roasterRxChan, rmtRawBuf, sizeof(rmtRawBuf), &roasterRxCfg);
}
#else
// -----------------------------------------------------------------------------
// Interrupt to watch for start of roaster message (fallback when
// _ROASTER_RX_RMT_ is off)
// https://forum.arduino.cc/t/detecting-pulses-of-certain-lengths-using-interrupts/360570/12
// -----------------------------------------------------------------------------
unsigned long lastPulse;
volatile bool roasterStartFound = 0;

void watchRoasterStart() {
  // D_println("Preamble found");
  unsigned long now = micros();

  if ((now - lastPulse) >= PREAMBLE) {
    roasterStartFound = 1;
  } else {
    roasterStartFound = 0;
  }
  lastPulse = now;
}

void getMessage(int bytes, int pin) {
  D_println(LOG_ROASTER, "getting message from roaster");
  unsigned long timeIntervals[ROASTER_LENGTH * 8];
  unsigned long pulseDuration = 0;
  int bits = bytes * 8;

  // Read bits
  for (int i = 0; i < bits; i++) {
    timeIntervals[i] = pulseIn(pin, LOW);
  }

  // Clear receiveBuffer
  for (int i = 0; i < bytes; i++) {
    receiveBuffer[i] = 0;
  }

  // Convert intervals into bits
  for (int i = 0; i < bits; i++) {
    if (timeIntervals[i] >= PULSE_ONE) {
      receiveBuffer[i / 8] |= (1 << (i % 8));
    }
  }
}
#endif // _ROASTER_RX_RMT_

bool calculateRoasterChecksum() {
  uint8_t sum = 0;
  for (int i = 0; i < (ROASTER_LENGTH - 1); i++) {
    sum += receiveBuffer[i];
  }
	bool valid = (sum == receiveBuffer[ROASTER_LENGTH - 1]);
	D_printf(LOG_ROASTER, "checksum: %d, buf: %d, match: %d\n", sum,
	         receiveBuffer[ROASTER_LENGTH - 1], valid);
	if (1) {
    D_printf(LOG_ROASTER, "Buffer: ");
    for (int i = 0; i < ROASTER_LENGTH; i++) {
      D_printf(LOG_ROASTER, "%02X ", receiveBuffer[i]);
    }
    D_printf(LOG_ROASTER, "\n");
  }
  return valid;
}

double calculateTemp() {
  rawTempX = ((receiveBuffer[0] << 8) + receiveBuffer[1]);
  rawTempY = ((receiveBuffer[2] << 8) + receiveBuffer[3]);

  double x = 0.001 * rawTempX;
  double y = 0.001 * rawTempY;
  double v; // Declare v here

  if (rawTempX > 836 || rawTempY > 221) {
    v = -224.2 * y * y * y + 385.9 * y * y - 327.1 * y + 171;
  } else {
    v = -278.33 * x * x * x + 491.944 * x * x - 451.444 * x + 310.668;
  }

  if (CorF == 'F') {
    v = 1.8 * v + 32.0;
  }
  return v;
}

// BT rate-of-rise. The Artisan-matching algorithm this used to spell out
// inline now lives in RorTracker (ror.h/ror.cpp) so the ET channel
// (et_sensor.cpp) runs the identical calculation instead of a hand-copied
// second copy. This instance is BT's; it writes the global `ror` that the
// dashboard and the rest of the firmware already read.
RorTracker btRor;

// Window size must stay != 3: MedianFilterLib's window-3 fast path
// (addValue3) never updates the field GetFiltered() reads, so GetFiltered()
// would always return 0. Fine at 7; if this is ever lowered to 3, switch to
// using AddValue()'s return value like et_sensor.cpp does.
MedianFilter<double> tempFilter(7);
void filtTemp(double v){
  int maxV = ((CorF == 'F') ? 500 : 260); //pick appropriate max cutoff given C or F units
  if(v < 0 || v > maxV) { return; } //don't process blatantly bogus values
  tempFilter.AddValue(v); //add to the collection
  temp = tempFilter.GetFiltered(); //update global temp
  D_printf(LOG_ROASTER, "filtered temp: %.2f\n", temp);
  ror = btRor.update(temp);
}

#ifdef _ROASTER_RX_RMT_
// Non-blocking: the main loop runs faster than the roaster's ~8.75Hz frame
// rate, so most calls will find no new frame yet -- that's normal, not a
// read failure.
void extern getRoasterMessage() {
  if (!rmtFrameReady) {
    return;
  }
  // Copy out while rmtFrameReady is still true so the callback won't
  // overwrite rmtReadyBuf mid-copy, then release the buffer.
  rmt_symbol_word_t local[RMT_MAXSYM];
  size_t n = rmtReadyNum;
  for (size_t i = 0; i < n; i++) local[i] = rmtReadyBuf[i];
  rmtFrameReady = false;

  int bits = decodeRoasterFrame(local, n);
  if (bits < ROASTER_LENGTH * 8 || !calculateRoasterChecksum()) {
    D_println(LOG_ROASTER, "Not valid roaster message.");
    return;
  }
  filtTemp(calculateTemp());
}
#else
void extern getRoasterMessage() {
  getMessage(ROASTER_LENGTH, RX_PIN);

  if (calculateRoasterChecksum()) {
    // Valid checksum, compute temperature with filtering
    filtTemp(calculateTemp());
  } else {
    D_println(LOG_ROASTER, "Not valid roaster message.");
  }
}
#endif // _ROASTER_RX_RMT_
