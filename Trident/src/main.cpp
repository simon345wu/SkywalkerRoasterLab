#include <Arduino.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <PID_v1.h>
#include <WebSerial.h>
#include <WiFi.h>

#include "CommandLoop.h"
#include "SkiComms.h"
#include "SkiCMD.h"
#include "api.h"
#include "ble.h"
#include "display.h"
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

void setup() {
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
  xTaskCreate(displayLoop, "DisplayTask", configMINIMAL_STACK_SIZE + 2048,
              NULL, 1, NULL);
  displayInit();
  touchInit();
  myPID.SetOutputLimits(0, 95);
  delay(5000);
  initBLE("Trident", "1.0.2", "Skywalker-Trident");

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
  StateDataT data = {temp, _currentState};
  StateRequestT req = socketTick(data);
}

void bleLoop() {

  StateDataT data = {temp, _currentState};
  StateRequestT req = bleTick(data);
}

void handleSerialCommand(String command) {
  command.trim();
  CommandTypeT type = classifyCommandType(command);
  if (type == CMDType_READ) {

    String readMsg = "0, " + String(temp, 1) + "," + String(temp, 1) + "," +
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

void displayLoop(void *params) {
  while (1) {
    String wifiStatus;
    if (WiFi.getMode() == WIFI_AP) {
      wifiStatus = "AP " + WiFi.softAPIP().toString();
    } else if (WiFi.status() == WL_CONNECTED) {
      wifiStatus = "STA " + WiFi.localIP().toString();
    } else {
      wifiStatus = "--";
    }
    // Two independent states per interface, shown as 2 chars: 1st = link/
    // transport is there at all, 2nd = the controlling app actually did the
    // CHAN handshake over it. Being linked doesn't mean it's actually being
    // used (e.g. raw testing without ever sending CHAN), so collapsing both
    // into one OK/-- would hide that distinction.
    String bleStatus = String(deviceConnected ? "C" : "-") +
                       (hibeanHandshakeDone ? "H" : "-");

    // USB has no explicit connect/disconnect signal like BLE does, so
    // "link" is approximated as "seen any byte in the last 5s". If that
    // goes quiet, treat the handshake as stale too -- a fresh Artisan
    // session re-sends CHAN before it resumes polling, so this naturally
    // re-latches on reconnect rather than showing a stuck "handshaked"
    // state from a session that's actually long gone.
    bool usbLinkActive = (millis() - lastUsbActivityTime < 5000);
    if (!usbLinkActive) {
      artisanHandshakeDone = false;
    }
    String usbStatus = String(usbLinkActive ? "C" : "-") +
                       (artisanHandshakeDone ? "H" : "-");
    displayDashboard(temp, ror, sendBuffer[HEAT_BYTE], sendBuffer[VENT_BYTE],
                      sendBuffer[DRUM_BYTE] != 0, sendBuffer[COOL_BYTE] != 0,
                      wifiStatus.c_str(), bleStatus.c_str(),
                      usbStatus.c_str());
    delay(250);
  }
  vTaskDelete(NULL);
}

void loop() {
  // roaster shut down, clear our buffers
  if (itsbeentoolong()) {
    D_println("too long, shutting down");
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
