#include "dlog.h"
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

static bool categoryEnabled[LOG_CATEGORY_COUNT];

// False until WebSerial.begin() has run (WebSocket mode only). Gates all
// logging so BLE mode -- which starts no WebSerial -- never writes to it.
static bool sinkReady = false;

void logSetSinkReady(bool ready) { sinkReady = ready; }

static const char *kCategoryNames[LOG_CATEGORY_COUNT] = {
    "SYS", "WIFI", "BLE",  "WS",  "ROASTER", "ET",      "BT2",
    "ROR", "CMD",  "PID",  "TOUCH", "QUEUE", "WEATHER", "DIAG",
};

void logInit() {
  logSetAllEnabled(true);
  // High-frequency, per-sample categories start quiet.
  categoryEnabled[LOG_ROASTER] = false;
  categoryEnabled[LOG_ET] = false;
  categoryEnabled[LOG_BT2] = false;
  categoryEnabled[LOG_ROR] = false;
  // On-demand diagnostics off by default -- turn on with "LOG;DIAG;ON".
  categoryEnabled[LOG_DIAG] = false;
}

bool logEnabled(LogCategory cat) {
  return sinkReady && cat < LOG_CATEGORY_COUNT && categoryEnabled[cat];
}

void logSetEnabled(LogCategory cat, bool on) {
  if (cat < LOG_CATEGORY_COUNT) {
    categoryEnabled[cat] = on;
  }
}

void logSetAllEnabled(bool on) {
  for (int i = 0; i < LOG_CATEGORY_COUNT; i++) {
    categoryEnabled[i] = on;
  }
}

const char *logCategoryName(LogCategory cat) {
  return cat < LOG_CATEGORY_COUNT ? kCategoryNames[cat] : "?";
}

int logCategoryFromName(const String &name) {
  for (int i = 0; i < LOG_CATEGORY_COUNT; i++) {
    if (name.equalsIgnoreCase(kCategoryNames[i])) {
      return i;
    }
  }
  return -1;
}

void logPrintStatus() {
  WebSerial.println("[LOG] category status:");
  for (int i = 0; i < LOG_CATEGORY_COUNT; i++) {
    WebSerial.println(String("  ") + kCategoryNames[i] + ": " +
                       (categoryEnabled[i] ? "ON" : "OFF"));
  }
}

bool logHandleCommand(const String &inputRaw) {
  String input = inputRaw;
  input.trim();
  String upper = input;
  upper.toUpperCase();
  if (upper != "LOG" && !upper.startsWith("LOG;")) {
    return false;
  }

  int split1 = upper.indexOf(';');
  String catToken = split1 >= 0 ? upper.substring(split1 + 1) : "";
  int split2 = catToken.indexOf(';');
  String stateToken;
  if (split2 >= 0) {
    stateToken = catToken.substring(split2 + 1);
    catToken = catToken.substring(0, split2);
  }
  catToken.trim();
  stateToken.trim();

  if (catToken.length() == 0 || catToken == "LIST") {
    logPrintStatus();
    return true;
  }

  bool on;
  if (stateToken == "ON") {
    on = true;
  } else if (stateToken == "OFF") {
    on = false;
  } else {
    WebSerial.println(
        "[LOG] usage: LOG | LOG;LIST | LOG;<CATEGORY>;ON|OFF | LOG;ALL;ON|OFF");
    return true;
  }

  if (catToken == "ALL") {
    logSetAllEnabled(on);
    WebSerial.println(String("[LOG] all categories: ") + (on ? "ON" : "OFF"));
    return true;
  }

  int cat = logCategoryFromName(catToken);
  if (cat < 0) {
    WebSerial.println("[LOG] unknown category: " + catToken);
    logPrintStatus();
    return true;
  }

  logSetEnabled((LogCategory)cat, on);
  WebSerial.println(String("[LOG] ") + logCategoryName((LogCategory)cat) +
                     ": " + (on ? "ON" : "OFF"));
  return true;
}

void D_printf(LogCategory cat, const char *fmt, ...) {
  if (!logEnabled(cat)) {
    return;
  }

  va_list args;
  va_start(args, fmt);
  va_list argsCopy;
  va_copy(argsCopy, args);
  int len = vsnprintf(nullptr, 0, fmt, args);
  va_end(args);
  if (len < 0) {
    va_end(argsCopy);
    return;
  }

  char stackBuf[128];
  char *buf = stackBuf;
  bool heap = false;
  if ((size_t)len >= sizeof(stackBuf)) {
    buf = (char *)malloc(len + 1);
    if (!buf) {
      va_end(argsCopy);
      return;
    }
    heap = true;
  }
  vsnprintf(buf, len + 1, fmt, argsCopy);
  va_end(argsCopy);

  WebSerial.print(buf);
  if (heap) {
    free(buf);
  }
}
