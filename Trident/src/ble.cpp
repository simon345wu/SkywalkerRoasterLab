
/* Replaces Classic Bluetooth with BLE (NUS).
 *
 * Service UUID:
 *     6e400001-b5a3-f393-e0a9-e50e24dcca9e
 * Characteristics UUIDs:
 *   - Write Characteristic (RX):
 *     6e400002-b5a3-f393-e0a9-e50e24dcca9e
 *   - Notify Characteristic (TX):
 *     6e400003-b5a3-f393-e0a9-e50e24dcca9e
 *
 * Sends notifications for temperature/status data.
 * Expects commands via the write characteristic.
 *
 * Built on NimBLE-Arduino (h2zero), not the ESP32 Arduino core's own
 * bundled BLEDevice.h -- matches HiBean's official "Skywalker Comm"
 * reference firmware (github.com/MagnmCI/SkiBeanCommunity) exactly. Both
 * wrap the same underlying NimBLE stack but are different libraries with
 * different defaults; confirmed via that repo's real source (`lib/SkiBLE.h`)
 * that HiBean's own reference never calls addServiceUUID() and still
 * advertises the service UUID -- something the ESP32 Arduino core's own BLE
 * classes did NOT do automatically (had to add that call by hand, see the
 * PROGRESS.md/memory writeup from the investigation that led here). This
 * swap also adds the PID_TUNE/PID_MODE/PID_SAMPLE_TIME/PID_MAX_POWER
 * characteristics HiBean's Comm mode exposes, which this firmware never
 * had at all -- TC4-style commands can't fully express PID state (no way to
 * read it back), which is exactly why the official firmware added these as
 * a separate BLE-only interface.
 */

#include "ble.h"
#include "dlog.h"
#include "model.h"
#include "state_request_queue.h"
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <PID_v1.h> // for the PID type + P_ON_E/P_ON_M, shared with main.cpp's myPID

// -----------------------------------------------------------------------------
// BLE UUIDs for Nordic UART Service
// -----------------------------------------------------------------------------
#define SERVICE_UUID "6e400001-b5a3-f393-e0a9-e50e24dcca9e" // NUS service
#define CHARACTERISTIC_UUID_RX "6e400002-b5a3-f393-e0a9-e50e24dcca9e" // Write
#define CHARACTERISTIC_UUID_TX "6e400003-b5a3-f393-e0a9-e50e24dcca9e" // Notify

// -----------------------------------------------------------------------------
// BLE UUIDs for PID config -- matches HiBean's official Skywalker Comm
// firmware's scheme exactly (lib/SkiBLE.h in the SkiBeanCommunity repo).
// -----------------------------------------------------------------------------
#define PID_TUNE_UUID "6dbf0201-758d-4b5e-bc11-40cfaea42dfe" // "kp,ki,kd"
#define PID_MODE_UUID "6dbf0202-758d-4b5e-bc11-40cfaea42dfe" // "P_ON_M" | "P_ON_E"
#define PID_SAMPLE_TIME_UUID "6dbf0203-758d-4b5e-bc11-40cfaea42dfe" // "iiii" (ms)
#define PID_MAX_POWER_UUID "6dbf0204-758d-4b5e-bc11-40cfaea42dfe" // "0"-"100" (%)

// -----------------------------------------------------------------------------
// BLE Globals
// -----------------------------------------------------------------------------
NimBLEServer *pServer = nullptr;
NimBLECharacteristic *pTxCharacteristic = nullptr;
bool extern deviceConnected = false;
// Distinct from deviceConnected: that's just the BLE link (radio-level
// pairing), this is "HiBean actually started talking TC4 to us" (sent
// CHAN). Mirrors artisanHandshakeDone for USB in main.cpp -- same "OK"
// should mean the controlling app is really using us, not just connected.
bool hibeanHandshakeDone = false;
extern String firmWareVersion;
extern String sketchName;

// Trident's existing PID state, shared with the TC4-text-command path
// (SkiCMD.h's "PID;T;..."/"PID;PM;..."/"PID;CT;..." handlers in main.cpp) --
// the new PID_* BLE characteristics below read/write these same globals
// directly rather than introducing a second, parallel config object (which
// is what the official firmware's PIDConfig class is -- Trident already has
// a single source of truth for this, no need for a second one to keep in
// sync).
extern PID myPID;
extern double Kp, Ki, Kd;
extern int pMode, pSampleTime;
// PID_v1 has no getter for the output-limit ceiling it was last given, so
// this tracks it locally for PID_MAX_POWER's read-back. Kept in sync with
// main.cpp's own initial `myPID.SetOutputLimits(0, 95)` call by starting at
// the same value -- if that default ever changes, update both.
static int pidMaxPower = 95;

StateRequestT _currentRequest = {255, 255, 255, 255};
StateDataT _currentData = {0};

void notifyBLEClient(const String &message);

// No space after the leading "0," -- matches both reference
// HiBean-compatible firmwares in this repo
// (HiBean/SkiBeanQuickSV/SkiCMD.h and
// HiBean/ESP32S3_Zero_Artisan_HiBean_Roaster_Control_v1.63.ino). Line
// ending kept as "\r\n": the two references actually disagree with each
// other on that (bare "\n" vs "\r\n"), so it evidently doesn't matter to
// HiBean's parser -- reverted that part of an earlier attempt rather than
// keep an unverified change.
String buildReadMessage() {
  // ambient, ET, BT, heater, fan -- ET/BT are the external MAX31865 #1/#2
  // probes (_currentData.et/.bt each mirror the next one down the fallback
  // chain -- ET->BT->NTC -- when their own probe is absent/faulted).
  return "0," + String(_currentData.et, 1) + "," +
        String(_currentData.bt, 1) + "," +
        String(_currentData.request.heater) + "," +
        String(_currentData.request.fan) + "\r\n";
}

// HiBean's "Skywalker HB" mode never sends READ (confirmed via a WebSerial
// capture of its actual traffic: only PID;SV/FILTER/OT2 control commands
// came through, never READ or CHAN) -- unlike Artisan's TC4, which polls.
// It apparently expects the roaster to push temperature on its own, so
// this sends one unsolicited notify per second whenever a client is
// connected, independent of whether anything ever asks for it. Gated on
// deviceConnected rather than hibeanHandshakeDone since that flag depends
// on CHAN, which this same traffic capture showed HB mode also never
// sends.
unsigned long _lastAutoNotifyMs = 0;
const unsigned long AUTO_NOTIFY_INTERVAL_MS = 1000;

StateRequestT bleTick(StateDataT data) {
  _currentData = data;

  if (deviceConnected &&
      millis() - _lastAutoNotifyMs >= AUTO_NOTIFY_INTERVAL_MS) {
    _lastAutoNotifyMs = millis();
    notifyBLEClient(buildReadMessage());
  }

  StateRequestT response = _currentRequest;
  _currentRequest = {255, 255, 255, 255};
  return response;
}

// -----------------------------------------------------------------------------
// BLE Server Callbacks
// -----------------------------------------------------------------------------
class MyServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *pServer, NimBLEConnInfo &connInfo) override {
    deviceConnected = true;

    // Change BLE connection parameters per apple ble guidelines
    // (for this client, min interval 15ms (/1.25), max 30ms (/1.25), latency 4
    // frames, timeout 5sec(/10ms)
    // https://docs.silabs.com/bluetooth/4.0/bluetooth-miscellaneous-mobile/selecting-suitable-connection-parameters-for-apple-devices
    pServer->updateConnParams(connInfo.getConnHandle(), 12, 24, 4, 500);

    D_println(LOG_BLE, "BLE: Client connected.");
  }
  void onDisconnect(NimBLEServer *pServer, NimBLEConnInfo &connInfo,
                    int reason) override {
    deviceConnected = false;
    hibeanHandshakeDone = false;
    D_println(LOG_BLE, "BLE: Client disconnected. Restarting advertising...");
    pServer->getAdvertising()->start();
  }
};

// -----------------------------------------------------------------------------
// BLE Characteristic Callbacks
// -----------------------------------------------------------------------------
class MyCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *pCharacteristic,
              NimBLEConnInfo &connInfo) override {
    String rxValue = String(pCharacteristic->getValue().c_str());

    if (rxValue.length() > 0) {
      String input = String(rxValue.c_str());
      D_print(LOG_BLE, "BLE Write Received: ");
      D_println(LOG_BLE, input);
      CommandTypeT type = classifyCommandType(input);
      if (type == CMDType_READ) {
        notifyBLEClient(buildReadMessage());
        return;
      }
      if (type == CMDType_CHAN) {
        String message = "# Active channels set to 2100\r\n";
        D_println(LOG_BLE, message);
        notifyBLEClient(message);
        hibeanHandshakeDone = true;
      }
      _currentRequest = parseCommandToStateRequest(input);
      enqueueStateRequest(_currentRequest, SOURCE_BLE);
    }
  }
};

// Diagnostic only: onWrite() logs every command HiBean sends us, but GATT
// *reads* (e.g. HiBean checking the TX characteristic or the Device
// Information Service's board/sketchName/firmware fields to decide if this
// is a device it recognizes) go straight through the BLE stack with zero
// visibility -- there's no equivalent log for them at all otherwise. This
// makes that visible.
class LoggingReadCallbacks : public NimBLECharacteristicCallbacks {
public:
  explicit LoggingReadCallbacks(const char *label) : _label(label) {}
  void onRead(NimBLECharacteristic *pCharacteristic,
             NimBLEConnInfo &connInfo) override {
    D_print(LOG_BLE, "BLE GATT READ: ");
    D_println(LOG_BLE, _label);
  }

private:
  const char *_label;
};

// PID_TUNE: "kp,ki,kd" -- comma-separated, matching the official firmware's
// format exactly (its onWrite splits on ',', not ';' like the TC4 command
// set uses elsewhere in this codebase).
class PIDTuneCallback : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *pCharacteristic,
              NimBLEConnInfo &connInfo) override {
    String rxValue = String(pCharacteristic->getValue().c_str());
    double pidTune[3] = {Kp, Ki, Kd};
    int paramCount = 0;
    while (rxValue.length() > 0 && paramCount < 3) {
      int index = rxValue.indexOf(',');
      if (index == -1) {
        pidTune[paramCount++] = rxValue.toDouble();
        break;
      }
      pidTune[paramCount++] = rxValue.substring(0, index).toDouble();
      rxValue = rxValue.substring(index + 1);
    }
    Kp = pidTune[0];
    Ki = pidTune[1];
    Kd = pidTune[2];
    myPID.SetTunings(Kp, Ki, Kd, pMode);
    D_printf(LOG_PID, "BLE PID_TUNE: Kp=%.2f Ki=%.2f Kd=%.2f\n", Kp, Ki, Kd);
  }
  void onRead(NimBLECharacteristic *pCharacteristic,
             NimBLEConnInfo &connInfo) override {
    pCharacteristic->setValue(String(Kp) + "," + String(Ki) + "," + String(Kd));
  }
};

class PIDModeCallback : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *pCharacteristic,
              NimBLEConnInfo &connInfo) override {
    String rxValue = String(pCharacteristic->getValue().c_str());
    pMode = (rxValue == "P_ON_E") ? P_ON_E : P_ON_M;
    myPID.SetTunings(Kp, Ki, Kd, pMode);
    D_println(LOG_PID, "BLE PID_MODE: " + rxValue);
  }
  void onRead(NimBLECharacteristic *pCharacteristic,
             NimBLEConnInfo &connInfo) override {
    pCharacteristic->setValue(pMode == P_ON_E ? "P_ON_E" : "P_ON_M");
  }
};

class PIDSampleTimeCallback : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *pCharacteristic,
              NimBLEConnInfo &connInfo) override {
    String rxValue = String(pCharacteristic->getValue().c_str());
    int newSampleTime = rxValue.toInt();
    if (newSampleTime > 0) {
      pSampleTime = newSampleTime;
      myPID.SetSampleTime(pSampleTime);
      D_println(LOG_PID, "BLE PID_SAMPLE_TIME: " + rxValue);
    }
  }
  void onRead(NimBLECharacteristic *pCharacteristic,
             NimBLEConnInfo &connInfo) override {
    pCharacteristic->setValue(String(pSampleTime));
  }
};

class PIDMaxPowerCallback : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *pCharacteristic,
              NimBLEConnInfo &connInfo) override {
    String rxValue = String(pCharacteristic->getValue().c_str());
    int newMax = rxValue.toInt();
    if (newMax >= 0 && newMax <= 100) {
      pidMaxPower = newMax;
      myPID.SetOutputLimits(0, pidMaxPower);
      D_println(LOG_PID, "BLE PID_MAX_POWER: " + rxValue);
    }
  }
  void onRead(NimBLECharacteristic *pCharacteristic,
             NimBLEConnInfo &connInfo) override {
    pCharacteristic->setValue(String(pidMaxPower));
  }
};

void notifyBLEClient(const String &message) {
  D_println(LOG_BLE, "Attempting to notify BLE client with: " + message);

  if (deviceConnected && pTxCharacteristic) {
    // Matches the official firmware's notifyNimBLEClient() exactly -- its
    // comment: "Give up time so hibean sees delta between write and notify
    // timestamps." Apparently HiBean's client associates a write's response
    // with the following notify partly by timing; sending back-to-back with
    // no gap risks them looking simultaneous to it.
    delay(30);
    pTxCharacteristic->setValue(message.c_str());
    pTxCharacteristic->notify();
    D_println(LOG_BLE, "Notification sent successfully.");
  } else {
    D_println(LOG_BLE,
              "Notification failed. Device not connected or TX characteristic "
              "unavailable.");
  }
}

static String bleDeviceName;

String getBleDeviceName() { return bleDeviceName; }

void initBLE(String sketchName, String firmWareVersion, String boardID) {
  bleDeviceName = boardID;
  NimBLEDevice::init(std::string(boardID.c_str()));
  NimBLEDevice::setMTU(185);

  pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  NimBLEService *pService = pServer->createService(SERVICE_UUID);

  // Roaster notifes to HiBean. No addDescriptor(BLE2902)/CCCD call needed --
  // NimBLE-Arduino auto-creates the CCCD descriptor for any characteristic
  // with the NOTIFY property, unlike the ESP32 Arduino core's own BLE
  // classes which needed it added by hand.
  pTxCharacteristic = pService->createCharacteristic(
      CHARACTERISTIC_UUID_TX, NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ);
  pTxCharacteristic->setCallbacks(new LoggingReadCallbacks("TX (data/notify)"));

  // Hibean commands to Roaster
  NimBLECharacteristic *pRxCharacteristic = pService->createCharacteristic(
      CHARACTERISTIC_UUID_RX,
      NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  pRxCharacteristic->setCallbacks(new MyCallbacks());

  // PID config characteristics -- HiBean's Comm mode's BLE-only interface
  // for PID state, since TC4 commands can set tunings/mode/sample-time but
  // can't read any of them back. Matches the official firmware's UUIDs and
  // value formats exactly.
  NimBLECharacteristic *pidTuneCharacteristic = pService->createCharacteristic(
      PID_TUNE_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE);
  pidTuneCharacteristic->setCallbacks(new PIDTuneCallback());

  NimBLECharacteristic *pidModeCharacteristic = pService->createCharacteristic(
      PID_MODE_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE);
  pidModeCharacteristic->setCallbacks(new PIDModeCallback());

  NimBLECharacteristic *pidSampleTimeCharacteristic =
      pService->createCharacteristic(
          PID_SAMPLE_TIME_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE);
  pidSampleTimeCharacteristic->setCallbacks(new PIDSampleTimeCallback());

  NimBLECharacteristic *pidMaxPowerCharacteristic =
      pService->createCharacteristic(
          PID_MAX_POWER_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE);
  pidMaxPowerCharacteristic->setCallbacks(new PIDMaxPowerCallback());

  pService->start();

  // esp32 information to HiBean for support/debug purposes
  NimBLEService *devInfoService = pServer->createService("180A");
  NimBLECharacteristic *boardCharacteristic =
      devInfoService->createCharacteristic("2A29", NIMBLE_PROPERTY::READ);
  boardCharacteristic->setValue(boardID.c_str());
  boardCharacteristic->setCallbacks(
      new LoggingReadCallbacks("Device Info: board (2A29)"));
  NimBLECharacteristic *sketchNameCharacteristic =
      devInfoService->createCharacteristic("2A28", NIMBLE_PROPERTY::READ);
  sketchNameCharacteristic->setValue(sketchName.c_str());
  sketchNameCharacteristic->setCallbacks(
      new LoggingReadCallbacks("Device Info: sketchName (2A28)"));
  NimBLECharacteristic *firmwareCharacteristic =
      devInfoService->createCharacteristic("2A26", NIMBLE_PROPERTY::READ);
  firmwareCharacteristic->setValue((sketchName + ", " + firmWareVersion).c_str());
  firmwareCharacteristic->setCallbacks(
      new LoggingReadCallbacks("Device Info: firmware (2A26)"));

  devInfoService->start();

  NimBLEAdvertising *pAdvertising = pServer->getAdvertising();
  // Deliberately NOT calling addServiceUUID() here, unlike the previous
  // (ESP32-core-BLEDevice-based) version of this file -- that call was
  // added there because that library's advertisement genuinely omitted the
  // service UUID (confirmed via a real nRF Connect packet capture) and
  // HiBean's "Skywalker Comm" scan couldn't find the device without it.
  // HiBean's own official firmware (SkiBeanCommunity's SkiBLE.h) never
  // calls this either, on NimBLE-Arduino -- betting that this library
  // includes registered services' UUIDs in the advertisement by default,
  // matching official behavior exactly. If hardware testing shows Comm
  // mode can't discover the device again, add addServiceUUID(SERVICE_UUID)
  // back here -- it's a known-working fallback either way.
  pAdvertising->setName(std::string(boardID.c_str()));
  pAdvertising->start();

  D_println(LOG_BLE, "BLE Advertising started...");
}
