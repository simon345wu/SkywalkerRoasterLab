#include "bt2_sensor.h"
#include "dlog.h"
#include "et_sensor.h"
#include <Arduino.h>
// -----------------------------------------------------------------------------
// External variables
// -----------------------------------------------------------------------------
extern PID myPID;
extern double pInput, pOutput, pSetpoint;
extern double Kp, Ki, Kd;
extern int pMode, pSampleTime, manualHeatLevel;

// -----------------------------------------------------------------------------
// Command Strings
// -----------------------------------------------------------------------------
const String CMD_READ = "READ";
const String CMD_HEAT = "OT1";
const String CMD_VENT = "OT2";
const String CMD_OFF = "OFF";
const String CMD_DRUM = "DRUM";
const String CMD_FILTER = "FILTER";
const String CMD_COOL = "COOL";
const String CMD_CHAN = "CHAN";
const String CMD_UNITS = "UNITS";

// -----------------------------------------------------------------------------
// Forward declarations
// -----------------------------------------------------------------------------
void handleCHAN();
void handleOT1(uint8_t value);
void handleREAD();
void handleHEAT(uint8_t value);
void handleVENT(uint8_t value);
void handleDRUM(uint8_t value);
void handleFILTER(uint8_t value);
void handleCOOL(uint8_t value);
void eStop();
void handlePIDControl();
void setPIDMode(bool usePID);

// -----------------------------------------------------------------------------
// Utility Functions
// -----------------------------------------------------------------------------
unsigned long lastEventTime = 0; // marker for last time we got HiBean message
const unsigned long LAST_EVENT_TIMEOUT =
    10UL * 1000000UL; // 10 seconds (micros)

extern bool touchSessionActive;

bool itsbeentoolong() {
  if (touchSessionActive) {
    // Touch panel is actively driving the roaster locally -- skip the
    // inactivity watchdog until the user presses STOP.
    return false;
  }
  unsigned long now = micros();
  unsigned long duration = now - lastEventTime;
  return (duration > LAST_EVENT_TIMEOUT);
}

void shutdown() {
  for (int i = 0; i < CONTROLLER_LENGTH; i++) {
    sendBuffer[i] = 0;
  }
  sendBuffer[CHECK_BYTE] = 0; // Reset checksum
}

// -----------------------------------------------------------------------------
// Command handlers
// -----------------------------------------------------------------------------
void handleCHAN() {
  String message = "# Active channels set to 2100\r\n";
  D_println(LOG_CMD, message);
  // notifyBLEClient(message);
}

void handleOT1(uint8_t value) {
  manualHeatLevel = constrain(value, 0, 100); // Set manual heat level
  if (myPID.GetMode() == AUTOMATIC) {
    setPIDMode(false); // Disable PID control
  }
  handleHEAT(manualHeatLevel);
}

void handleREAD() {
  // ambient, ET, BT, heater, fan. ET = external MAX31865 #1 probe
  // (etReport() mirrors BT when there's no probe / it faulted); BT = external
  // MAX31865 #2 probe (bt2Report() mirrors NTC, the roaster's own probe, when
  // there's no probe / it faulted).
  String readMsg = "0," + String(etReport(), 1) + "," + String(bt2Report(), 1) + "," +
                   String(sendBuffer[HEAT_BYTE]) + "," +
                   String(sendBuffer[VENT_BYTE]) + "\r\n";

  // Was D_print/D_println-ing readMsg here on every call -- webSocketLoop()
  // (main.cpp) calls handleREAD() unconditionally from the ~1ms
  // webSerialLoop task, so this flooded WebSerial with ~1000 "READ Output:"
  // lines/sec regardless of whether Artisan actually sent a READ, making the
  // console useless for anything else. Removed; uncomment briefly if this
  // specific message ever needs eyeballing again.
  // D_print("READ Output: ");
  // D_println(readMsg);

  // notifyBLEClient(readMsg);
  lastEventTime = micros();
}

void handleHEAT(uint8_t value) {
  if (value <= 100) {
    setValue(&sendBuffer[HEAT_BYTE], value);
  }
  lastEventTime = micros();
}

void handleVENT(uint8_t value) {
  if (value <= 100) {
    setValue(&sendBuffer[VENT_BYTE], value);
    if (value == 0) {
      handleFILTER(value); // off
    } else {
      handleFILTER((int)round(
          4 - ((value - 1) * 4 / 100))); // convert 0-100 to inverted 4-1
    }
  }
  lastEventTime = micros();
}

void handleDRUM(uint8_t value) {
  if (value != 0) {
    setValue(&sendBuffer[DRUM_BYTE], 100);
  } else {
    setValue(&sendBuffer[DRUM_BYTE], 0);
  }
  lastEventTime = micros();
}

void handleFILTER(uint8_t value) {
  if (value >= 0 && value <= 4) {
    setValue(&sendBuffer[FILTER_BYTE], value); // 0 off; 1 fastest -> 4 slowest
  }
  lastEventTime = micros();
}

void handleCOOL(uint8_t value) {
  if (value <= 100) {
    setValue(&sendBuffer[COOL_BYTE], value);
    handleFILTER(value);
  }
  lastEventTime = micros();
}

void eStop() {
  D_println(LOG_CMD, "Emergency Stop Activated! Heater OFF, Vent 100%");
  handleHEAT(0);   // Turn off heater
  handleVENT(100); // Set vent to 100%
}

// PID hControls///
// adjusting the heating power based on PID temperature control
void handlePIDControl() {
  if (myPID.GetMode() == AUTOMATIC) {
    // BT (bt2Report(), the external MAX31865/PT1000 probe) drives PID control
    // -- falls back to NTC (the roaster's own probe) automatically if that
    // probe is absent/faulted, so a lost wire can't freeze or feed garbage
    // into the control loop.
    pInput = bt2Report();
    myPID.Compute();
    int roundedHeat = std::round(pOutput / 5.0) * 5;
    handleHEAT(roundedHeat);
  } else {
    handleHEAT(manualHeatLevel); // Use stored manual heat level
  }
}

void setPIDMode(bool usePID) {
  if (usePID) {
    myPID.SetMode(AUTOMATIC); // Enable PID
    D_println(LOG_PID, "PID mode set to AUTOMATIC");
  } else {
    myPID.SetMode(MANUAL);       // Disable PID
    manualHeatLevel = 0;         // Set heat to 0% for safety
    handleHEAT(manualHeatLevel); // Apply the change immediately
    D_println(LOG_PID, "PID mode set to MANUAL");
  }
}

void parseAndExecuteCommands(String input) {
  input.trim();
  input.toUpperCase();

  // D_println("Parsing command: " + input);

  int split1 = input.indexOf(';');
  String command = "";
  String param = "";
  String subcommand = "";

  if (split1 >= 0) {
    command = input.substring(0, split1);
    String remainder = input.substring(split1 + 1);
    int split2 = remainder.indexOf(';');

    if (split2 >= 0) {
      subcommand = remainder.substring(0, split2);
      param = remainder.substring(split2 + 1);
    } else {
      param = remainder;
    }
  } else {
    command = input;
  }

  if (command == "PID") {
    if (param == "ON") {
      setPIDMode(true); // Enable PID control
    } else if (param == "OFF") {
      setPIDMode(false); // Disable PID control
    } else if (subcommand == "SV") {
      double newSetpoint = param.toDouble();
      if (newSetpoint > 0 && newSetpoint <= 300) { // Example range check
        pSetpoint = newSetpoint;
        D_print(LOG_PID, "New Setpoint: ");
        D_println(LOG_PID, pSetpoint);
      }
    } else if (subcommand == "T") {
      double pidTune[3]; // pp.p;ii.i;dd.d
      int paramCount = 0;
      while (param.length() > 0) {
        int index = param.indexOf(';');
        if (index == -1) {                          // No delim found
          pidTune[paramCount++] = param.toDouble(); // remaining string
          break;
        } else {
          pidTune[paramCount++] =
              param.substring(0, index).toDouble(); // grab first
          param = param.substring(index + 1);       // trim string
        }
      }
      Kp, Ki, Kd = pidTune[0], pidTune[1], pidTune[2];
      D_print(LOG_PID, "Kp: ");
      D_println(LOG_PID, Kp);
      D_print(LOG_PID, "Ki: ");
      D_println(LOG_PID, Ki);
      D_print(LOG_PID, "Kd: ");
      D_println(LOG_PID, Kd);
      myPID.SetTunings(Kp, Ki, Kd,
                       pMode); // apply the pid params to running config
    } else if (subcommand == "PM") {
      D_print(LOG_PID, "Setting PMode to: ");
      D_println(LOG_PID, param);
      if (param == "M") {
        pMode = P_ON_M;
        myPID.SetTunings(Kp, Ki, Kd,
                         pMode); // apply the pid params to running config
      } else {
        pMode = P_ON_E;
        myPID.SetTunings(Kp, Ki, Kd,
                         pMode); // apply the pid params to running config
      }
    } else if (subcommand == "CT") {
      D_print(LOG_PID, "Setting Cycle Time to: ");
      D_println(LOG_PID, param.toDouble());
      myPID.SetSampleTime(pSampleTime);
    }
  } else if (command == "OT1") {
    D_println(LOG_CMD, "Setting OT1: " + param);
    handleOT1(param.toInt()); // Manual heater control (only in MANUAL mode)
  } else if (command == "READ") {
    handleREAD();
  } else if (command == "OT2") {
    D_println(LOG_CMD, "Setting OT2: " + param);
    handleVENT(param.toInt()); // Set fan duty
  } else if (command == "OFF") {
    shutdown(); // Shut down system
  } else if (command == "ESTOP") {
    eStop(); // Emergency stop (heater = 0, vent = 100)
  } else if (command == "DRUM") {
    D_println(LOG_CMD, "Setting Drum: " + param);
    handleDRUM(param.toInt()); // Start/stop the drum
  } else if (command == "FILTER") {
    D_println(LOG_CMD, "Setting Filter: " + param);
    handleFILTER(param.toInt()); // Turn on/off filter fan
  } else if (command == "FILT") {
    // Artisan TC4 FILT;f1;f2;f3;f4 -- per-physical-channel digital filter
    // level, 0-100. ET is physical channel 1 (skywalker.aset
    // arduinoETChannel=1) and BT is channel 2 (arduinoBTChannel=2), so the
    // first value drives the ET EMA (et_sensor.cpp) and the second drives
    // the BT EMA (bt2_sensor.cpp). NTC's filtering is fixed (median-7 in
    // filtTemp()) and the aux channels are unused, so the rest are ignored.
    // "FILT" != the "FILTER" (filter-fan) command above.
    String etFilt = subcommand.length() > 0 ? subcommand : param;
    D_println(LOG_CMD, "Setting ET FILT: " + etFilt);
    etSetFilter(etFilt.toInt());

    if (subcommand.length() > 0) {
      // subcommand held the ET value, so `param` starts with the BT value.
      int nextSplit = param.indexOf(';');
      String btFilt = nextSplit >= 0 ? param.substring(0, nextSplit) : param;
      D_println(LOG_CMD, "Setting BT FILT: " + btFilt);
      bt2SetFilter(btFilt.toInt());
    }
  } else if (command == "COOL") {
    D_println(LOG_CMD, "Setting Cool: " + param);
    handleCOOL(param.toInt()); // Cool the beans
  } else if (command == "CHAN") {
    handleCHAN(); // Handle TC4 init message
  } else if (command == "UNITS") {
    if (split1 >= 0)
      CorF = input.charAt(split1 + 1); // Set temperature units
  }
}
