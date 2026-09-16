#pragma once
#include <cstdint>

// Which wireless stack the board brings up at boot. WiFi (for the Artisan
// WebSocket device + WebSerial + weather) and BLE (NimBLE, for the HiBean app)
// both live only in *internal* RAM (neither can use PSRAM), and running both at
// once leaves internal RAM critically low (~13KB free on this board) -- enough
// that a momentary WiFi/AsyncTCP allocation spike fails and the WebSocket
// connection is dropped mid-run while the display/touch tasks (backed by
// already-allocated / PSRAM memory) keep going. So only one radio is
// initialised per boot; the other's ~30-40KB of internal RAM stays free.
//
// USB/TC4 serial is cheap (plain UART) and stays available in both modes.
//
// The choice is persisted in NVS and read once at boot -- switching takes
// effect on the next reboot (deinitialising a live NimBLE/WiFi stack at runtime
// is fiddly and fragments the heap; a clean reboot is simpler and reliable).
enum CommsMode : uint8_t {
  COMMS_WEBSOCKET = 0, // WiFi + Artisan WebSocket + WebSerial + weather; no BLE
  COMMS_BLE = 1,       // NimBLE only; WiFi fully off (max internal RAM for BLE)
};

// Reads the persisted mode from NVS. Defaults to COMMS_WEBSOCKET when unset.
CommsMode commsModeGet();

// Persists the mode to NVS. Does NOT change the running stacks -- the caller is
// expected to reboot (ESP.restart()) for it to take effect.
void commsModeSet(CommsMode mode);

const char *commsModeName(CommsMode mode);
