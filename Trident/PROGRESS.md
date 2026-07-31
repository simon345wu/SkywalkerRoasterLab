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

### VS Code PlatformIO IDE extension (alternative to CLI)
- Installed the official PlatformIO IDE extension so builds can be done from VS Code instead of asking Claude to run `pio` in a terminal.
- `platformio.ini` now has `[platformio]` / `default_envs = s3` at the top, so both the IDE's default Build/Upload buttons and a bare `pio run` only touch `s3` (skip `c6`/`s3-mini`).
- **Gotcha**: the extension's installer and the `uv`-installed CLI both write into the same `~/.platformio` core dir, and stepped on each other's internal `penv` (pioarduino's helper Python venv used to install packages like `pyyaml`, `cryptography`, `esptool`, etc.). Symptom: `pio run` fails with "Python version mismatch... Recreating penv" then "Error: uv installation via pip failed with exit code 106" (real error is swallowed — pioarduino's `penv_setup.py` redirects it to `DEVNULL`). Root cause: `uv venv --python=<nested tool-venv python>` produces a broken venv with no `pyvenv.cfg` and no pip. Fixed by deleting `~/.platformio/penv` and recreating it directly with the real base interpreter:
  ```
  & "C:\uv-python\cpython-3.14-windows-x86_64-none\python.exe" -m venv "C:\Users\simon\.platformio\penv"
  ```
  If this resurfaces after reinstalling either tool, redo the above rather than fighting the auto-recreate logic.

### ILI9341 display added (committed: `7acbcdd`)
- Hardware: **Goouuu ESP32-S3 expansion board** with a bundled 2.8" **ILI9341** SPI TFT (240×320). Swapped out the unused ST7789 scaffold (`Display_ST7789.*` files left in place but now dead code, not deleted).
- Pins (confirmed working on real hardware): `TFT_MISO=46, TFT_MOSI=45, TFT_SCLK=3, TFT_CS=14, TFT_DC=47, TFT_RST=21` — found via a matching board's public pinout, not from Goouuu's own docs, but verified by flashing.
- `env:s3`'s `-D NO_DISPLAY` flag removed so the display actually compiles in.
- Display shows the same status string as WebSerial (`Status: heater,fan / Wifi: ip`), refreshed every ~250ms from `main.cpp`'s `webSerialLoop()`.
- Iterated through several rendering issues based on live hardware feedback:
  1. Default GFX font at `setTextSize(4)` was too large and looked blocky → switched to the bundled `FreeSans9pt7b` font at size 1 (smoother glyphs).
  2. Redrawing via full-screen `fillScreen()` every update caused visible flicker → tried opaque (fg,bg) text color to skip the clear, but custom GFX fonts (unlike the classic bitmap font) draw with a **transparent background only**, so old and new text piled on top of each other instead.
  3. Tried clearing just a bounding rect (`fillRect`) before redrawing — fixed the overlap but the clear-then-redraw was still visibly flickering.
  4. Final fix: render into an off-screen `GFXcanvas1` (1bpp, ~3.2KB RAM) each update, then `tft.drawBitmap(...)` it to the panel in one shot. No intermediate blank frame is ever shown on the physical screen. **This is the version that's committed and confirmed flicker-free on hardware.**

### Open items / next steps
- MAX31855 thermocouple (K-type) — deferred, not started. Was going to share the SPI bus with the display (SCK=3, MISO=46) using a new CS pin.
- Test USB and WiFi paths through the real Artisan app (not just raw protocol pokes).
- Test against the actual Skywalker roaster hardware (TX/RX pins) — only tested on a bare ESP32-S3 dev board so far.
- Consider cleaning up the deprecation warnings above.
- README's TODO list still has: LED status signalling, general code cleanup.
