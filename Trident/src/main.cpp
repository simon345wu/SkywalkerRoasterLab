#include <Arduino.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <PID_v1.h>
#include <WebSerial.h>
#include <WiFi.h>
#include <esp_log.h>

#include "CommandLoop.h"
#include "SkiComms.h"
#include "SkiCMD.h"
#include "api.h"
#include "ble.h"
#include "display.h"
#include "et_sensor.h"
#include "model.h"
#include "pindef.h"
#include "state_request_queue.h"
#include "touch.h"
#include "wifi_setup.h"

// -----------------------------------------------------------------------------
// Global Bean Temperature Variable
// -----------------------------------------------------------------------------
double temp = 0.0; // Filtered temperature
double ror = 0.0;  // Rate of rise, degrees/min

// -----------------------------------------------------------------------------
// Define PID variables
// -----------------------------------------------------------------------------
double pInput, pOutput;
double pSetpoint = 0.0; // Desired temperature (adjustable on the fly)
int pMode = P_ON_M;
// was 20,1,3 at 1sec
// pid calibrations (adjustable on the fly)
double Kp = 20.0, Ki = 0.5, Kd = 4.0;
int pSampleTime = 1000; // ms (adjustable on the fly)
int manualHeatLevel = 0;
// pid instance with our default values
PID myPID(&pInput, &pOutput, &pSetpoint, Kp, Ki, Kd, pMode, DIRECT);

const unsigned int LED_BLUE[3] = {0, 0, 32};
const unsigned int LED_GREEN[3] = {0, 128, 0};
const unsigned int LED_RED[3] = {128, 0, 0};
const unsigned int LED_BLACK[3] = {0, 0, 0};
const unsigned int LED_YELLOW[3] = {0, 128, 128};

typedef enum { booting = 0, connected, disconnected } BloodhoundStateT;

AsyncWebServer server(80);
const char rgbLedPin = RGB_PIN;
// const char ledPin = 15;
bool isOn = false;
BloodhoundStateT m_state = booting;
void webSerialLoop(void *params);
void displayLoop(void *params);
void ledControl();
void serialCommandTask(void *params);
extern bool deviceConnected;
extern bool hibeanHandshakeDone;
unsigned long lastUsbActivityTime = 0; // marker for USB status display
// Set once Artisan's CHAN handshake succeeds -- distinct from mere USB
// activity (raw testing/garbage bytes shouldn't count as "connected").
// Cleared if traffic goes quiet (see displayLoop's usbStatus), since a
// fresh Artisan session always re-sends CHAN before it resumes polling.
bool artisanHandshakeDone = false;

// ESP-IDF's own logging (log_e()/log_w()/... -- used internally by bundled
// libraries, e.g. AsyncWebSocket.cpp's "too many messages queued" error)
// writes straight to physical Serial (UART0) by default, the same port
// Artisan's TC4 protocol lives on. Confirmed on hardware 2026-09-12: with a
// WebSerial browser tab left open and its message queue backing up,
// AsyncWebSocket's log_e() calls fired fast enough to visibly interleave/
// corrupt each other on the wire (garbled, doubled-up characters). Installed
// before anything else can log, so IDF log output never reaches Serial at
// all -- dropped rather than rerouted to WebSerial, since WebSerial has the
// same queue-backpressure problem and could just move the corruption there.
static int droppedVprintf(const char *fmt, va_list args) { return 0; }

void setup() {
  logInit(); // WebSerial log-category defaults; before anything else logs
  esp_log_set_vprintf(droppedVprintf);
  Serial.begin(115200);
#ifdef S3
  // Own dedicated task instead of polling from webSerialLoop()'s 250ms-paced
  // task -- that delay was sized for display refresh, not for how fast
  // Artisan's TC4 handshake (e.g. CHAN, ~100ms budget) needs a reply.
  // (Tried Serial.onReceive() first to make this event-driven like
  // BLE/WebSocket already are -- confirmed via WebSerial + raw serial
  // round-trip tests that the callback never fires at all on this
  // pioarduino/IDF 5.5.5 + ESP32-S3 combination, so falling back to polling,
  // just decoupled from the display's cadence and fast enough -- 5ms --
  // to comfortably clear Artisan's window.)
  xTaskCreate(serialCommandTask, "SerialCmdTask", configMINIMAL_STACK_SIZE + 4096,
              NULL, 2, NULL);
#endif
  delay(100);
  pinMode(rgbLedPin, OUTPUT);

  rgbLedWrite(rgbLedPin, LED_RED[0], LED_RED[1], LED_RED[2]);

  initStateQueue();
  setupWifi();
  WebSerial.begin(&server);

  WebSerial.onMessage([](uint8_t *data, size_t len) {
    String input = String(data, len);
    // "LOG;..." is a WebSerial-console-only debug control (which message
    // categories print, see dlog.h) -- deliberately kept out of
    // parseAndExecuteCommands() so it can never be confused with a TC4/
    // Artisan command, even though only this console (not USB serial) can
    // actually reach this callback.
    if (logHandleCommand(input)) {
      return;
    }
    parseAndExecuteCommands(input);
  });
  setupMainLoop(&server);
  setupApi(&server);
  server.begin();
  xTaskCreate(webSerialLoop, "WebSerialTask", configMINIMAL_STACK_SIZE + 2048,
              NULL, 1, NULL);
  // Display drawing was only ever sharing webSerialLoop()'s 250ms delay by
  // coincidence -- nothing else left in that loop actually needs pacing
  // (WebSerial.loop() is designed for tight-loop calling, ledControl()
  // self-paces off its own millis() check, webSocket/BLE just refresh a
  // state snapshot). Split out so display refresh keeps its own 250ms
  // rhythm without holding anything else to it.
  // Stack bumped well past the old dashboard code's +2048 -- LVGL's
  // lv_timer_handler() (object tree walk, font/glyph rendering) runs much
  // deeper than the old direct tft.print() calls ever did, and the original
  // size was crashing/rebooting the board (confirmed via hardware testing:
  // screen cycling between blank/partial/never-quite-finished states).
  xTaskCreate(displayLoop, "DisplayTask", configMINIMAL_STACK_SIZE + 8192,
              NULL, 1, NULL);
  displayInit();
  // touchInit() before lvglInit(): lvglInit() now wires an LVGL indev whose
  // read callback (display.cpp) calls into touch.cpp's touchGetPoint(),
  // which needs the XPT2046 hardware already begun.
  touchInit();
  // ET probe (MAX31865) shares the SPI bus touchInit() just brought up -- must
  // come after it. No-op on non-S3 builds.
  etSensorInit();
  lvglInit(); // minimal LVGL bring-up (lvgl-ui branch), see displayLoop()
  myPID.SetOutputLimits(0, 95);
  delay(5000);
  // HiBean's "Skywalker Comm" mode wouldn't connect at all with either
  // "Skywalker-Trident" (this fork's original name) or "SkiBean" (an
  // unverified internet guess, also tried and also didn't help). Found the
  // actual reference name in this repo's HiBean/SkiBeanQuickSV/SkiBLE.h
  // (a real HiBean-compatible firmware) -- it advertises as exactly this
  // string. "Skywalker HB" mode has already connected successfully under
  // two different names, so it's evidently not name-gated and shouldn't
  // care about this change.
  // Experiment: HiBean HB mode connects and takes control commands fine,
  // but never displays temperature and never sends READ/CHAN to ask for
  // it -- yet both reference firmwares in this repo (HiBean/SkiBeanQuickSV
  // and HiBean/ESP32S3_Zero_Artisan_HiBean_Roaster_Control_v1.63.ino) show
  // HiBean *should* poll via READ. Unproven theory: HiBean reads the BLE
  // Device Information Service (characteristics 2A28/2A26, sketchName/
  // firmwareVersion below) to decide whether it recognizes this as a
  // compatible device before it starts asking for temperature -- GATT
  // reads don't go through onWrite()/D_println, so there's no visibility
  // into whether this actually happens. Trying the exact identity strings
  // from the more complete Artisan+HiBean reference firmware (the closest
  // analog to what Trident does) instead of the arbitrary "Trident"/
  // "1.0.2" this fork used.
  initBLE("ESP32S3_Zero_Artisan_HiBean_Roaster_Control_v1.63.ino",
         "ESP32S3-Zero_Artisan+HiBean_v1.6.3", "ESP32_Skycommand_BLE");

#ifdef _ROASTER_TX_RMT_
  initRoasterTxRMT();
#else
  pinMode(TX_PIN, OUTPUT);
  digitalWrite(TX_PIN, HIGH);
#endif
#ifdef _ROASTER_RX_RMT_
  initRoasterRMT();
#else
  pinMode(RX_PIN, INPUT);
  attachInterrupt(RX_PIN, watchRoasterStart, FALLING);
#endif

  shutdown();
}

StateRequestT _currentState = {0};
void webSocketLoop() {
  handleREAD();
  StateDataT data = {temp, etReport(), _currentState};
  StateRequestT req = socketTick(data);
}

void bleLoop() {

  StateDataT data = {temp, etReport(), _currentState};
  StateRequestT req = bleTick(data);
}

void handleSerialCommand(String command) {
  command.trim();
  CommandTypeT type = classifyCommandType(command);
  if (type == CMDType_READ) {

    // TC4 READ reply: ambient, ET, BT, heater, fan. ET = MAX31865 probe
    // (etReport() falls back to BT when no probe / faulted); BT = roaster's
    // own probe. skywalker.aset maps arduinoETChannel=1, arduinoBTChannel=2.
    String readMsg = "0," + String(etReport(), 1) + "," + String(temp, 1) + "," +
                     String(_currentState.heater) + "," +
                     String(_currentState.fan) + "\r\n";
    Serial.println(readMsg);
  } else if (type == CMDType_CHAN) {
    Serial.println("# Active channels set to 2100\r\n");
    artisanHandshakeDone = true;
  } else if (type == CMDType_STATE_REQUEST) {
    StateRequestT req = parseCommandToStateRequest(command);
    enqueueStateRequest(req, SOURCE_USB);
  } else {
    parseAndExecuteCommands(command);
  }
}

// Dedicated task, polling every 5ms -- independent of webSerialLoop()'s
// 250ms display-refresh cadence, so a command like Artisan's CHAN (~100ms
// budget) gets read and replied to well within its window.
void serialCommandTask(void *params) {
  String serialAccum;
  while (1) {
    while (Serial.available() > 0) {
      char c = (char)Serial.read();
      if (c == '\n') {
        lastUsbActivityTime = millis();
        handleSerialCommand(serialAccum);
        serialAccum = "";
      } else {
        serialAccum += c;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

void webSerialLoop(void *params) {
  while (1) {
    WebSerial.loop();
    // No longer paced to 250ms for display's sake (see displayLoop) --
    // just a minimal yield so this task doesn't starve the scheduler/
    // watchdog. ledControl() self-paces via its own millis() check;
    // WebSerial.loop()/webSocketLoop()/bleLoop() are fine called this often.
    vTaskDelay(pdMS_TO_TICKS(1));
    ledControl();
    webSocketLoop();
    bleLoop();
  }
  vTaskDelete(NULL);
}

// Minimal LVGL bring-up (lvgl-ui branch): the old status-string dashboard
// drawing is on hold here (still on touch-panel-control/main) while this
// proves LVGL itself works end-to-end. lv_timer_handler() wants calling
// frequently, not paced to display refresh, so this task's cadence dropped
// from 250ms to 5ms.
void displayLoop(void *params) {
  while (1) {
    lvglLoop();
    // Same task as the LVGL touch-SPI reads -- keeps the shared touch/ET SPI
    // bus single-threaded without a lock. Self rate-limited to its own
    // sampling interval, so this is cheap on most iterations.
    etSensorTick();
    delay(5);
  }
  vTaskDelete(NULL);
}

void loop() {
  // roaster shut down, clear our buffers
  if (itsbeentoolong()) {
    D_println(LOG_SYS, "too long, shutting down");
    shutdown();
  }

  // RMT RX is non-blocking and returns immediately when there's no new
  // frame yet, so it can just be called every loop iteration. The fallback
  // pulseIn() path still needs the interrupt-set flag gating it.
#ifdef _ROASTER_RX_RMT_
  getRoasterMessage();
#else
  if (roasterStartFound) {
    getRoasterMessage();
  }
#endif

  touchLoop();

  processStateQueue();
  // Ensure PID or manual heat control is handled
  handlePIDControl();

  sendRoasterMessage();

  _currentState = getCurrentState();
}

unsigned long LED_LAST_ON_MS = 0;
const unsigned long LED_FLASH_DELAY_MS = 2000;

void ledControl() {
  int now = millis();
  if (now - LED_LAST_ON_MS > LED_FLASH_DELAY_MS) {
    isOn = !isOn;
    LED_LAST_ON_MS = now;
  }
  if (isOn) {
    rgbLedWrite(rgbLedPin, 0, 0, 0);
  } else {
    rgbLedWrite(rgbLedPin, LED_BLUE[0], LED_BLUE[1], LED_BLUE[2]);
  }
}
