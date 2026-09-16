#pragma once
#include <Arduino.h>
#include <WebSerial.h>

// Categorized WebSerial debug logging. Every D_println/D_print/D_printf call
// is tagged with a category; whether it actually prints depends on that
// category's runtime on/off state (logEnabled()), settable live from the
// WebSerial console via the "LOG;..." command -- see logHandleCommand() and
// main.cpp's WebSerial.onMessage(). This replaced a flat set of macros that
// always printed: a few of those (Artisan READ replies, ROR) turned out to
// fire fast enough (up to ~1000/sec) to make WebSerial useless and, in one
// case (ESP-IDF's own log_e(), a separate but related problem -- see
// main.cpp's droppedVprintf), corrupt the actual USB TC4 stream. Categorizing
// instead of just deleting those prints means they're still available on
// demand (LOG;ROASTER;ON etc.) instead of gone until someone edits code again.
enum LogCategory : uint8_t {
  LOG_SYS = 0, // system/boot/watchdog (main.cpp)
  LOG_WIFI,    // wifi_setup.cpp, api.cpp
  LOG_BLE,     // ble.cpp
  LOG_WS,      // Artisan-over-WebSocket (CommandLoop.cpp)
  LOG_ROASTER, // RMT roaster comm + BT reading (SkiComms.h) -- high frequency
  LOG_ET,      // MAX31865 ET probe (et_sensor.cpp) -- high frequency
  LOG_BT2,     // MAX31865 BT probe (bt2_sensor.cpp) -- high frequency
  LOG_ROR,     // rate-of-rise (ror.cpp), BT+ET combined -- high frequency
  LOG_CMD,     // TC4/serial command dispatch (SkiCMD.h)
  LOG_PID,     // PID tuning (SkiCMD.h + ble.cpp PID_* characteristics)
  LOG_TOUCH,   // touch.cpp
  LOG_QUEUE,   // state_request_queue.cpp arbitration
  LOG_WEATHER, // weather.cpp ambient fetch from PC proxy
  LOG_CATEGORY_COUNT
};

// Defaults: the three high-frequency categories start OFF, everything else
// (event-driven, not per-sample) starts ON. Not persisted -- always resets to
// this on boot. Call once, early in setup().
void logInit();

bool logEnabled(LogCategory cat);
void logSetEnabled(LogCategory cat, bool on);
void logSetAllEnabled(bool on);
const char *logCategoryName(LogCategory cat);
int logCategoryFromName(const String &name); // -1 if no match

// Prints the current ON/OFF state of every category. Unfiltered -- always
// prints regardless of any category's state, so turning things off can never
// hide the way to turn them back on.
void logPrintStatus();

// Recognizes and handles a "LOG" / "LOG;LIST" / "LOG;<CATEGORY>;ON|OFF" /
// "LOG;ALL;ON|OFF" console command (case-insensitive, same ';'-delimited
// style as the rest of the firmware's commands). Returns true if `input` was
// a LOG command (and has been fully handled); false otherwise, meaning the
// caller should treat it as ordinary input. Deliberately NOT wired into
// parseAndExecuteCommands() -- this is a WebSerial-console-only debug
// control, not part of the TC4/Artisan command surface (see main.cpp's
// WebSerial.onMessage(), which checks this before parseAndExecuteCommands()).
bool logHandleCommand(const String &input);

template <typename T> void D_println(LogCategory cat, T msg) {
  if (logEnabled(cat)) {
    WebSerial.println(msg);
  }
}

template <typename T> void D_print(LogCategory cat, T msg) {
  if (logEnabled(cat)) {
    WebSerial.print(msg);
  }
}

void D_printf(LogCategory cat, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
