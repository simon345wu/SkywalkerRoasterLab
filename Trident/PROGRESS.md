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

### Possible upstream bug found while explaining the code (not fixed yet)
- [state_request_queue.cpp](src/state_request_queue.cpp): [model.h:6](src/model.h#L6) comments the source priority as "BLE=0, WS=1, USB=2", implying BLE should win conflicts. But both `enqueueStateRequest`'s eviction logic and `processStateQueue`'s selection logic (`stateQueue[i].source > highestPriority`) pick the **largest** raw enum value — so in practice **USB(2) beats WebSocket(1) beats BLE(0)**, the opposite of what the comment says. This is upstream code (predates today's session, part of the original fork), not something introduced this session. Since this repo was forked from elsewhere, worth checking whether upstream intended BLE to win and the comparison operators are backwards, or the comment is just stale — haven't fixed it, just flagging it.

## 2026-08-01

### Touch panel control added (branch: `touch-panel-control`)
- New XPT2046 resistive touch layer over the ILI9341 screen: on-screen buttons for Fan −/+, Heat −/+, and STOP (`src/touch.h`, `src/touch.cpp`), wired into the existing `enqueueStateRequest()`/`processStateQueue()` arbitration as a new `SOURCE_TOUCH` ([model.h](src/model.h)), except STOP which calls `eStop()` directly, bypassing the queue entirely (deliberate — immediate local emergency stop shouldn't wait on arbitration).
- **Pin discovery**: initially guessed the touch controller shared the display's SPI bus (`TFT_SCLK/MOSI/MISO`) with only its own `TOUCH_CS`, copying the pattern that worked for the display pins. Wrong — the on-screen "touched" indicator stayed permanently true even with no finger on the panel (floating/garbage SPI reads). Found a blog post describing this exact board (its listed TFT pins matched ours exactly, confirming it's the same board) stating the touch controller is wired to a **completely separate SPI bus**: `TOUCH_CS=1, TOUCH_CLK=42, TOUCH_MOSI=2, TOUCH_MISO=41` ([display.h](src/display.h)). Since the `XPT2046_Touchscreen` library always talks to the global `SPI` object (no way to inject a different `SPIClass`), fixed by giving the **display** its own dedicated `SPIClass tftSPI(HSPI)` instead ([display.cpp](src/display.cpp)) and reserving the global `SPI` for touch (`touch.cpp`'s `touchInit()`).
- **Coordinate calibration**: default XPT2046 raw ADC range (200–3900) was a generic placeholder. Measured actual 4-corner raw values on hardware and found the raw X/Y axes are **not swapped** (original code guessed a swap — wrong) but **both run inverted** relative to screen coordinates. Recalibrated `TS_MINX/MAXX/MINY/MAXY` in `touch.cpp` from real corner readings; confirmed via a temporary on-screen raw/mapped-coordinate readout (reused the status text area, since WebSerial requires a WiFi connection mid-hardware-test) that taps on STOP and Fan+ land inside their correct button rectangles.
- **Verified on real hardware**: Fan+ button changes `F:` by the expected step; STOP immediately forces `H:0, F:100` regardless of queue state, bypassing arbitration as designed.
- Emergency-stop UX tweak per user feedback: `BTN_STOP_WIDTH` shrunk from full-width (312) to a more proportionate size — no longer spans the entire screen.

### Watchdog auto-shutdown has likely never actually worked (found while testing touch's watchdog bypass, not fixed)
- The touch feature added a `touchSessionActive` bypass to `itsbeentoolong()` ([SkiCMD.h](src/SkiCMD.h)) so a touch session isn't killed by the 10s inactivity timeout. Testing that bypass (STOP clears the flag, then wait >10s idle, expect auto-shutdown) showed `shutdown()` never fires **regardless of `touchSessionActive`**.
- Root cause: `main.cpp`'s `loop()` calls `handlePIDControl()` unconditionally every iteration ([main.cpp:172](src/main.cpp#L172)), which calls `handleHEAT(...)` every single time whether or not anything actually changed. `handleHEAT()` unconditionally sets `lastEventTime = micros()` ([SkiCMD.h:98](src/SkiCMD.h#L98)) — the same timestamp `itsbeentoolong()` checks against a 10-second threshold. Since the main loop runs continuously (thousands of times/sec), `lastEventTime` is perpetually refreshed to "now," so `itsbeentoolong()`'s `duration > LAST_EVENT_TIMEOUT` can essentially never be true.
- This predates the touch feature and isn't specific to it — the 10-second auto-shutdown watchdog appears to be dead code for **every** interface (BLE/WiFi/USB/touch), not just touch. Same category as the priority-order bug above: inherited from the fork, flagging rather than fixing per current direction.

### Dashboard UI redesign + ROR + Drum toggle
- Reworked the on-screen layout based on live user feedback, several rounds:
  - Button rows changed from 2 buttons each (Fan −/+, Heat −/+) to 4 (adding a `0` and `100` quick-set per row: F0/F−/F+/F100, H0/H−/H+/H100), narrower buttons to fit.
  - Each row's live value (Fan %, Heat %) moved from a separate top tile to sit at the end of its own button row, since the adjacent buttons already give context — freed up the top area for a much bigger Temp readout.
  - Temp switched from the classic scaled bitmap font (blocky at large `setTextSize`) to the bundled `FreeSansBold24pt7b` — natively large and smooth instead of scaled-up-and-blocky. Fan/Heat/ROR values all switched to the same font for visual consistency, per user request.
  - Added a **ROR** (rate of rise, °/min) tile next to Temp. **Confirmed with the user**: the existing Temp reading is bean temp (BT) from the roaster's own NTC (not a new sensor) — ROR is derived from it, no new hardware needed. Implemented in [SkiComms.h](src/SkiComms.h)'s `updateROR()`: each new temp sample's instantaneous slope vs. the previous sample is pushed into a rolling buffer, and the displayed ROR is the **moving average of every instantaneous slope from the trailing 5 seconds** (per user's explicit request — smooths sensor jitter better than a single two-point delta). Shown to 1 decimal place. Not yet validated against a real roast (bench-tested with a static/near-zero temp only).
  - Added a **Drum on/off toggle button** next to STOP (`BTN_DRUM_*` in [display.h](src/display.h), `sendDrum()` in [touch.cpp](src/touch.cpp)) — routes through the same `SOURCE_TOUCH` arbitration as Fan/Heat, unlike STOP. Its fill color (green=on, grey=off) is redrawn every dashboard refresh since it reflects live state, unlike the other buttons which are drawn once at boot.
  - STOP button narrowed and centered (was full screen width).
  - Removed the temporary touch-calibration debug UI (indicator box, touch-point dot, on-screen raw/mapped-coordinate readout) now that touch calibration is confirmed working — see calibration values in `touch.cpp`.
- All changes verified on real hardware via the same live-feedback loop used for the earlier display work.

### Windows froze completely (hard hang, not BSOD) twice during `pio upload`
- Happened twice this session, exact point in the upload unclear. Checked: no duplicate/ghost USB-SERIAL devices in Device Manager, board is on a direct motherboard/laptop USB port (not a hub), CH340 driver is a reasonably current version (3.9.2024.9). Nothing conclusive found.
- Best guess, unconfirmed: ESP32 boards toggle DTR/RTS to reset the chip on upload, which makes the USB device briefly disconnect/reconnect — on some USB 3.0 controllers/drivers this kind of rapid re-enumeration is known to occasionally wedge the whole USB subsystem hard enough to hang the OS. This session did ~20 uploads back-to-back, which may have compounded it. Retried after the checks above and it completed fine, so not blocking, but **worth watching for** — if it recurs, worth trying a different USB port/cable or a BIOS/chipset USB driver update.

### Open items / next steps
- Decide whether to fix the priority-order bug above (flip the comparison, or fix the comment/enum values — need to confirm intended behavior first).
- Decide whether to fix the watchdog bug above (likely needs `handlePIDControl`/`handleHEAT` to stop conflating "recomputed the same value again" with "a real new command arrived").
- Touch panel: all buttons (F0/F−/F+/F100, H0/H−/H+/H100, Drum toggle, STOP) confirmed working on hardware; not yet stress-tested for mis-taps right on button edges.
- ROR's 5s moving-average window/smoothing hasn't been validated against a real roast yet — bench-tested with a static temp only (no real BT movement to smooth).
- Dashboard's TEMP tile is bean temp (BT), read from the roaster's own built-in NTC over the existing serial protocol (`calculateTemp()`/`filtTemp()` in [SkiComms.h](src/SkiComms.h)) — not a new sensor. **MAX31865** (RTD, not MAX31855/thermocouple — corrected 2026-08-01) is the planned next addition, for a *second*, independent temperature: environment/exhaust temp (ET). Deferred, not started. Was going to share the SPI bus with the display (SCK=3, MISO=46) using a new CS pin — worth double-checking against the same "shared vs. dedicated SPI bus" surprise the touch controller had (see above) before assuming that works.
- Test USB and WiFi paths through the real Artisan app (not just raw protocol pokes).
- Test against the actual Skywalker roaster hardware (TX/RX pins) — only tested on a bare ESP32-S3 dev board so far.
- Consider cleaning up the deprecation warnings above.
- README's TODO list still has: LED status signalling, general code cleanup.
