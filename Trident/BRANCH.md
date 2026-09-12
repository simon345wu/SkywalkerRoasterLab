# et-max31865 — What This Branch Changed vs. the Original Fork

This is Trident's active development line (its now-deleted predecessors —
`touch-panel-control`, `lvgl-ui`, `exy-my-trident` — are all fully contained
in this branch's history; nothing from them was lost). It forked from `main`
at `f3a0ada` (`main` itself has no `Trident/` folder at all — it's a
different, unrelated multi-project repo). The first commit on this line,
`51c81b3`, is effectively where the Trident project started.

**32 commits, 32 files changed, +5532/-247 lines** since that first commit.

## What the original import had

- Bit-banged roaster RX/TX (`pulseIn()` / `delayMicroseconds()`)
- PID heat control
- Three control surfaces: USB serial (TC4/Artisan text protocol), BLE (the
  ESP32 Arduino core's own `BLEDevice` classes), Artisan-over-WebSocket
- No display, no touch input, no second temperature channel

## What's been added, by area

### Roaster communication
- Ported RX/TX from bit-banging to RMT hardware
  (`_ROASTER_RX_RMT_`/`_ROASTER_TX_RMT_`) — hardware-timed pulses, immune to
  WiFi/BLE interrupt jitter; the old bit-banged path is kept as a `#else`
  fallback
- Rewrote ROR (rate-of-rise) to match Artisan's own `compute_ror_simple()`
  algorithm instead of a home-grown moving average, via a shared
  `RorTracker` class (`ror.h`/`ror.cpp`) so BT and ET use the identical
  calculation

### Display + touch UI
- Added ILI9341 touchscreen support (`display.h`/`display.cpp`,
  `touch.h`/`touch.cpp`) — SPI panel plus an XPT2046 touch controller on its
  own separate SPI bus
- First pass: a touch-driven dashboard (Fan/Heat 0/-/+/100, Drum/Cool
  toggle, STOP)
- Rebuilt entirely as LVGL widgets: three screens (splash, main dashboard,
  config), live BT / BT-RoR / ET / ET-RoR tiles, Fan/Heat sliders, and
  status LEDs for WiFi/WS/BLE/USB link + handshake state
- Config screen shows the STA/AP IP (and port) for Artisan/HiBean network
  setup, the BLE device name, and a light/dark theme toggle

### External ET (exhaust) temperature probe
- Added a MAX31865 + 4-wire PT100 probe (`et_sensor.h`/`et_sensor.cpp`),
  sharing the touch controller's SPI bus with its own chip-select pin
  instead of a new bus
- Custom register-level driver instead of the Adafruit library (its
  `readRTD()` blocks ~75ms per sample, which would stall the display task) —
  two-stage smoothing (median-3 spike guard + EMA), with the EMA level
  adjustable live via Artisan's `FILT` command
- ET is now reported over all three protocols (TC4 serial, WebSocket, BLE)
  in the TC4 `ET` field, falling back to mirroring BT when the probe is
  absent or faulted

### BLE
- Swapped the ESP32 Arduino core's own `BLEDevice`/`BLEServer` classes for
  NimBLE-Arduino, matching HiBean's official reference firmware exactly
  (the two differ in default service-UUID advertising behavior)
- Added the PID_TUNE / PID_MODE / PID_SAMPLE_TIME / PID_MAX_POWER BLE
  characteristics HiBean's "Comm" mode exposes

### Diagnostics / debugging
- Categorized WebSerial logging (`dlog.h`/`dlog.cpp`): every log call is
  tagged (`SYS`/`WIFI`/`BLE`/`WS`/`ROASTER`/`ET`/`ROR`/`CMD`/`PID`/`TOUCH`/
  `QUEUE`), toggled live from the WebSerial console with
  `LOG;<CATEGORY>;ON|OFF`
- Fixed ESP-IDF's `log_e()` writing straight to the physical Serial port and
  corrupting the Artisan TC4 stream (`esp_log_set_vprintf()` → a no-op sink,
  installed first thing in `setup()`)
- Fixed several silent failure modes in the WebSocket path
  (`CommandLoop.cpp`): a raw-frame debug dump dead-coded behind an
  `#ifdef DEBUG` that was never defined anywhere, an unchecked
  `deserializeJson()` error, and `WS_EVT_PING` mislabeled as "unhandled"
- Fixed a `MedianFilterLib` gotcha where a window size of 3 silently breaks
  `GetFiltered()`

### Hardware / build
- Corrected the board profile from a generic 8MB-flash/no-PSRAM ESP32-S3 to
  the board's actual N16R8 hardware (16MB flash + 8MB PSRAM)
- Added board reference docs (schematic, pinout) and dev-environment setup
  notes

## Stats

- 32 commits since the original import (`51c81b3`)
- 32 files changed, +5532 / -247 lines
- New files: `src/et_sensor.h`, `src/et_sensor.cpp`, `src/ror.h`,
  `src/ror.cpp`, `src/dlog.cpp`, `src/touch.h`, `src/touch.cpp`,
  `PROGRESS.md`, board reference docs, `include/lv_conf.h`,
  `skywalker_websocket.aset`

See [PROGRESS.md](PROGRESS.md) for the full, dated development log — the
rationale and hardware-verification notes behind each change above.
