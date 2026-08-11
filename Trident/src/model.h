#include <Arduino.h>
#pragma once
#ifndef TRIDENT_MODEL
#define TRIDENT_MODEL

// The four control sources are equal -- state_request_queue.cpp applies
// whichever source most recently touched a given field (last write wins),
// no source ranks above another. This enum exists for logging/identifying
// which source a request came from, not for priority ordering.
typedef enum {
  SOURCE_BLE = 0,
  SOURCE_WEBSOCKET = 1,
  SOURCE_USB = 2,
  SOURCE_TOUCH = 3
} StateSourceT;

typedef struct {
  unsigned char heater;
  unsigned char fan;
  unsigned char cooling;
  unsigned char drum;
  String pidCommand = "";
} StateRequestT;

typedef struct {
  double temp;
  StateRequestT request;
} StateDataT;

typedef enum {
  CMDType_UNKNOWN,
  CMDType_READ,
  CMDType_PID,
  CMDType_STATE_REQUEST,
  CMDType_CHAN,
} CommandTypeT;

typedef struct {
  StateRequestT request;
  StateSourceT source;
  bool valid;
} StateRequestEntryT;

CommandTypeT classifyCommandType(String input);
StateRequestT parseCommandToStateRequest(String input);
#endif
