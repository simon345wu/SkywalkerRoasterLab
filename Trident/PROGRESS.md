# Trident — Progress Log

## 2026-07-31

### Dev environment setup (this machine)
- Python is managed via `uv` (not a system `python` on PATH). PlatformIO installed as an isolated `uv` tool:
  ```
  uv tool install platformio --with pip
  ```
  (`--with pip` is required — PlatformIO shells out to `pip install` for some package extras, and a bare `uv tool install` venv has no `pip` module.)
- `pio` lives at `C:\Users\simon\.local\bin` — not on PATH by default. Prefix commands with:
  ```
  $env:PATH = "C:\Users\simon\.local\bin;$env:PATH"
  ```
- **Must build with native PowerShell, not Git Bash.** pioarduino's toolchain installer (`idf_tools.py`) detects MSYS/MinGW shells and refuses to install (`ERROR: MSys/Mingw is not supported`), leaving `toolchain-xtensa-esp-elf` with only metadata and no compiler binary. Run `pio` from PowerShell/cmd.
- Windows long path support had to be enabled (some pioarduino package paths, e.g. under ESP-Matter/connectedhomeip headers, exceed 260 chars):
  ```
  Set-ItemProperty -Path "HKLM:\SYSTEM\CurrentControlSet\Control\FileSystem" -Name "LongPathsEnabled" -Value 1 -Type DWord
  ```
  (admin PowerShell, already done on this machine.)

### Code fixes (committed: `51c81b3`)
- `platformio.ini`: `platform = espressif32` pointed at the official PlatformIO registry, which only resolves to an old Arduino-ESP32 2.0.17-based build — too old for APIs this codebase uses (`rgbLedWrite`, newer `ledcAttach`, BLE `String` overloads). Repinned to the **pioarduino** fork (needed anyway for `env:c6` / ESP32-C6 support, which the official registry never had):
  ```
  platform = https://github.com/pioarduino/platform-espressif32/releases/download/stable/platform-espressif32.zip
  ```
- `src/ble.cpp`: `MyServerCallbacks::onConnect` used the old Bluedroid signature (`esp_ble_gatts_cb_param_t*`). The framework's BLE library is now NimBLE-based, so it was silently not overriding the base callback. Fixed to `onConnect(BLEServer*, ble_gap_conn_desc*)`, using `desc->conn_handle` instead of `param->connect.remote_bda` for `updateConnParams(...)`.

### Verified on real hardware (ESP32-S3 devkitc board, env `s3`)
- Build + flash via `pio run -e s3 -t upload --upload-port COM5` — success.
- **USB (TC4/Artisan protocol)**: sent raw `READ` over the serial port, got back `0, 0.0,0.0,0,0` — matches the expected format in `main.cpp`'s `serialLoop()`. **Not yet tested through the actual Artisan app.**
- **WiFi**: device (no saved credentials) broadcasts open AP `trident` as expected. Connected a laptop to it, confirmed `192.168.4.1` responds to HTTP, and `/api/wifi` returns 400 when called without `ssid`/`pass` params (didn't POST real credentials, to avoid dirtying device state). **Not yet tested through the actual Artisan app.**
- **BLE**: device advertises as `Skywalker-Trident`. HiBean pairing initially got stuck on "connecting" and timed out — turned out to be a HiBean-side issue (need to pick device type "Skywalker HB" in the app, not the generic/"comm" option). Once selected correctly, pairing succeeded and control worked.

### Known non-blocking warnings (not yet cleaned up)
- `BLEServer::updateConnParams()` is deprecated in favor of `requestConnParams()`.
- Several `new BLE2902()` calls in `ble.cpp` are deprecated — NimBLE auto-adds the CCCD descriptor when notify/indicate is enabled, manual `addDescriptor(new BLE2902())` is a no-op now.

### Open items / next steps
- Test USB and WiFi paths through the real Artisan app (not just raw protocol pokes).
- Test against the actual Skywalker roaster hardware (TX/RX pins) — only tested on a bare ESP32-S3 dev board so far.
- Consider cleaning up the deprecation warnings above.
- README's TODO list still has: LED status signalling, general code cleanup.
