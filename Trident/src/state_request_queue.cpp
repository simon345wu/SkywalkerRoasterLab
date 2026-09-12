#include "state_request_queue.h"
#include "dlog.h"
#include "model.h"
#include <Arduino.h>
#include <cstdint>

extern void handleDRUM(uint8_t value);
extern void handleCOOL(uint8_t value);
extern void handleOT1(uint8_t value);
extern void handleVENT(uint8_t value);
void parseAndExecuteCommands(String input);
void applyStateRequest(StateRequestT req, StateSourceT source);

// All sources (BLE, WebSocket, USB, Touch) are equal -- whichever field a
// request most recently touched wins, no source ranks above another. A
// single pending request is merged into (per-field, respecting the 255 "no
// change" sentinel) rather than replaced wholesale, so a fan-only request
// from one source can't clobber a heater-only request from another that's
// still waiting to be applied.
static StateRequestT pendingRequest = {255, 255, 255, 255, ""};
static bool pendingValid = false;
static StateSourceT pendingSource = SOURCE_BLE;
static SemaphoreHandle_t stateQueueMutex;
static StateRequestT targetState = {0, 0, 0, 0, ""};

void initStateQueue() {
  stateQueueMutex = xSemaphoreCreateMutex();
  if (stateQueueMutex == NULL) {
    D_println(LOG_QUEUE, "Failed to create state queue mutex!");
    return;
  }
}

bool enqueueStateRequest(StateRequestT req, StateSourceT source) {
  if (req.cooling == 255 && req.heater == 255 && req.drum == 255 &&
      req.fan == 255 && req.pidCommand.isEmpty()) {
    // D_println("nothing to enqueue");
    return false;
  }
  if (xSemaphoreTake(stateQueueMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    if (req.cooling != 255) {
      pendingRequest.cooling = req.cooling;
    }
    if (req.heater != 255) {
      pendingRequest.heater = req.heater;
    }
    if (req.fan != 255) {
      pendingRequest.fan = req.fan;
    }
    if (req.drum != 255) {
      pendingRequest.drum = req.drum;
    }
    if (!req.pidCommand.isEmpty()) {
      pendingRequest.pidCommand = req.pidCommand;
    }
    pendingValid = true;
    pendingSource = source;
    xSemaphoreGive(stateQueueMutex);
    return true;
  }
  return false;
}

void processStateQueue() {
  if (xSemaphoreTake(stateQueueMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    if (pendingValid) {
      StateRequestT req = pendingRequest;
      StateSourceT source = pendingSource;
      pendingValid = false;
      pendingRequest = {255, 255, 255, 255, ""};
      xSemaphoreGive(stateQueueMutex); // Release before applying
      applyStateRequest(req, source);
      return;
    }
    xSemaphoreGive(stateQueueMutex);
  }
}

void applyStateRequest(StateRequestT req, StateSourceT source) {
  if (req.cooling != 255 && targetState.cooling != req.cooling) {
    handleCOOL(req.cooling);
    targetState.cooling = req.cooling;
  }
  if (req.heater != 255 && targetState.heater != req.heater) {
    handleOT1(req.heater);
    targetState.heater = req.heater;
  }
  if (req.fan != 255 && targetState.fan != req.fan) {
    handleVENT(req.fan);
    targetState.fan = req.fan;
  }
  if (req.drum != 255 && targetState.drum != req.drum) {
    handleDRUM(req.drum);
    targetState.drum = req.drum;
  }
	if (req.pidCommand.isEmpty() == false) {
		parseAndExecuteCommands(req.pidCommand);
	}

  D_printf(LOG_QUEUE,
           "Applied request from source %d: H=%d F=%d D=%d C=%d PID=%s\n",
           source, req.heater, req.fan, req.drum, req.cooling,
           req.pidCommand.c_str());
}

StateRequestT getCurrentState() {
  StateRequestT state = {0};
  if (xSemaphoreTake(stateQueueMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    state = targetState;
    xSemaphoreGive(stateQueueMutex);
  }

  return state;
}
