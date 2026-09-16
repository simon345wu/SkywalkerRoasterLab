#include "display.h"
#include "dlog.h"
#ifdef NO_DISPLAY
void displayInit() {}
void displayDashboard(float temp, float ror, uint8_t heat, uint8_t fan,
                       bool drumOn, bool coolOn, const char *wifiStatus,
                       const char *wsStatus, const char *bleStatus,
                       const char *usbStatus) {}
#else
#include <Adafruit_ILI9341.h>
#include <Fonts/FreeSansBold24pt7b.h>
#include <SPI.h>

// The TFT gets its own dedicated SPI bus (separate from the global SPI
// object) because the touch controller's XPT2046_Touchscreen library always
// talks to the global `SPI` -- so that one is reserved for touch.cpp instead.
SPIClass tftSPI(HSPI);
Adafruit_ILI9341 tft = Adafruit_ILI9341(&tftSPI, TFT_DC, TFT_CS, TFT_RST);

// "lightgreen" (#90EE90) as RGB565 -- Adafruit_ILI9341's predefined colors
// don't include a light green.
#define DASH_LIGHTGREEN 0x9772

// Temp gets its own tile at the top (black background, red text, big font --
// it's the only thing up there now that Fan/Heat moved into their button
// rows below, and no "TEMP" label anymore, just the number). Uses the
// bundled FreeSansBold24pt7b font instead of the classic scaled bitmap font
// -- the classic font blown up with setTextSize() just draws bigger square
// pixels ("dot-matrix" look); a font that's natively drawn at a large point
// size stays smooth. Composed off-screen in a GFXcanvas1 and blitted in one
// shot -- drawing straight onto the panel on every refresh was the flicker
// problem solved earlier for the old status text, same fix applies here.
#define TILE_Y             2
#define TILE_HEIGHT        60
#define TILE_TEMP_X        4
#define TILE_TEMP_WIDTH    180
#define TILE_TEMP_BASELINE 48

// ROR (rate of rise, deg/min) sits to the right of Temp -- reserved now,
// even though the underlying calculation (RorTracker in ror.cpp, fed from
// SkiComms.h's filtTemp()) is a first pass and hasn't been validated against
// a real roast yet. Small classic-font caption ("ROR"), but the value uses the same
// FreeSansBold24pt7b as Temp -- ROR/Fan/Heat values are all sized to match.
// Wide enough for "+15.3" (1 decimal place) at that font's char widths.
#define TILE_ROR_X              188
#define TILE_ROR_WIDTH          128
#define TILE_ROR_LABEL_Y        4
#define TILE_ROR_VALUE_BASELINE 52

#define CONN_STATUS_Y      66
#define CONN_STATUS_HEIGHT 14
#define CONN_STATUS_WIDTH  320

// Fan/Heat no longer get their own top tile -- their live value now sits at
// the end of their own button row (ROW_NUM_X/ROW_NUM_WIDTH, see display.h),
// same big FreeSansBold24pt7b font as Temp/ROR.
#define ROW_NUM_BASELINE 40

GFXcanvas1 tempCanvas(TILE_TEMP_WIDTH, TILE_HEIGHT);
GFXcanvas1 rorCanvas(TILE_ROR_WIDTH, TILE_HEIGHT);
GFXcanvas1 connCanvas(CONN_STATUS_WIDTH, CONN_STATUS_HEIGHT);
GFXcanvas1 fanNumCanvas(ROW_NUM_WIDTH, BTN_HEIGHT);
GFXcanvas1 heatNumCanvas(ROW_NUM_WIDTH, BTN_HEIGHT);

// Button chrome is drawn once and never redrawn, so it doesn't need the
// flicker-avoidance treatment the tiles above need.
void drawButton(int x, int y, int w, int h, const char *label,
                uint16_t color) {
  tft.fillRoundRect(x, y, w, h, 6, color);
  tft.drawRoundRect(x, y, w, h, 6, ILI9341_WHITE);
  tft.setTextSize(2);
  tft.setTextColor(ILI9341_WHITE);
  tft.setCursor(x + 3, y + h / 2 - 8);
  tft.print(label);
}

void drawButtons() {
  drawButton(BTN_FAN_ZERO_X, BTN_FAN_ROW_Y, BTN_WIDTH, BTN_HEIGHT, "F0",
             ILI9341_DARKGREY);
  drawButton(BTN_FAN_MINUS_X, BTN_FAN_ROW_Y, BTN_WIDTH, BTN_HEIGHT, "F-",
             ILI9341_DARKGREY);
  drawButton(BTN_FAN_PLUS_X, BTN_FAN_ROW_Y, BTN_WIDTH, BTN_HEIGHT, "F+",
             ILI9341_DARKGREY);
  drawButton(BTN_FAN_MAX_X, BTN_FAN_ROW_Y, BTN_WIDTH, BTN_HEIGHT, "F100",
             ILI9341_DARKGREY);

  drawButton(BTN_HEAT_ZERO_X, BTN_HEAT_ROW_Y, BTN_WIDTH, BTN_HEIGHT, "H0",
             ILI9341_DARKGREY);
  drawButton(BTN_HEAT_MINUS_X, BTN_HEAT_ROW_Y, BTN_WIDTH, BTN_HEIGHT, "H-",
             ILI9341_DARKGREY);
  drawButton(BTN_HEAT_PLUS_X, BTN_HEAT_ROW_Y, BTN_WIDTH, BTN_HEIGHT, "H+",
             ILI9341_DARKGREY);
  drawButton(BTN_HEAT_MAX_X, BTN_HEAT_ROW_Y, BTN_WIDTH, BTN_HEIGHT, "H100",
             ILI9341_DARKGREY);

  drawButton(BTN_STOP_X, BTN_STOP_Y, BTN_STOP_WIDTH, BTN_STOP_HEIGHT, "STOP",
             ILI9341_RED);
}

void drawTempTile(const char *value) {
  tempCanvas.fillScreen(0);
  tempCanvas.setCursor(8, TILE_TEMP_BASELINE);
  tempCanvas.print(value);
  tft.drawBitmap(TILE_TEMP_X, TILE_Y, tempCanvas.getBuffer(), TILE_TEMP_WIDTH,
                 TILE_HEIGHT, ILI9341_RED, ILI9341_BLACK);
}

void drawRorTile(const char *value) {
  rorCanvas.fillScreen(0);
  rorCanvas.setFont(); // classic font for the small caption
  rorCanvas.setTextSize(1);
  rorCanvas.setCursor(4, TILE_ROR_LABEL_Y);
  rorCanvas.print("ROR");
  rorCanvas.setFont(&FreeSansBold24pt7b); // back to the big font for the value
  rorCanvas.setCursor(4, TILE_ROR_VALUE_BASELINE);
  rorCanvas.print(value);
  tft.drawBitmap(TILE_ROR_X, TILE_Y, rorCanvas.getBuffer(), TILE_ROR_WIDTH,
                 TILE_HEIGHT, ILI9341_YELLOW, ILI9341_BLACK);
}

// Live Fan/Heat value at the end of their button row -- no label (the row's
// own buttons already say which one it is), just a big number.
void drawRowNum(GFXcanvas1 &canvas, int x, int y, const char *value,
                uint16_t fg) {
  canvas.fillScreen(0);
  canvas.setCursor(4, ROW_NUM_BASELINE);
  canvas.print(value);
  tft.drawBitmap(x, y, canvas.getBuffer(), ROW_NUM_WIDTH, BTN_HEIGHT, fg,
                 ILI9341_BLACK);
}

// Drum/Cool toggles -- unlike the static buttons, their fill color reflects
// on/off state, so they're redrawn (directly, no canvas needed -- it's a
// solid fill plus a short fixed label, nothing to flicker against) every
// dashboard refresh instead of once at boot.
void drawToggleButton(int x, int y, int w, int h, const char *label,
                      bool on) {
  uint16_t color = on ? ILI9341_GREEN : ILI9341_DARKGREY;
  tft.fillRoundRect(x, y, w, h, 6, color);
  tft.drawRoundRect(x, y, w, h, 6, ILI9341_WHITE);
  tft.setTextSize(2);
  tft.setTextColor(ILI9341_WHITE);
  tft.setCursor(x + 3, y + h / 2 - 8);
  tft.print(label);
}

void displayInit() {
  tftSPI.begin(TFT_SCLK, TFT_MISO, TFT_MOSI, TFT_CS);
  tft.begin();
  tft.setRotation(1);
  // lvgl-ui branch: skip the old dashboard's one-time draw (drawButtons() +
  // displayDashboard()) so the screen stays blank until lvglInit() paints
  // it -- otherwise this leftover draw is indistinguishable on the panel
  // from a genuine LVGL failure (both would just sit there showing nothing
  // new happened). tft.begin()/setRotation() above still run since
  // lvglInit()'s flush callback writes through this same tft object.
  tft.fillScreen(ILI9341_BLACK);
}

void displayDashboard(float temp, float ror, uint8_t heat, uint8_t fan,
                       bool drumOn, bool coolOn, const char *wifiStatus,
                       const char *wsStatus, const char *bleStatus,
                       const char *usbStatus) {
  char buf[8];
  snprintf(buf, sizeof(buf), "%.1f", temp);
  drawTempTile(buf);
  snprintf(buf, sizeof(buf), "%+.1f", ror);
  drawRorTile(buf);
  snprintf(buf, sizeof(buf), "%d", fan);
  drawRowNum(fanNumCanvas, ROW_NUM_X, BTN_FAN_ROW_Y, buf, DASH_LIGHTGREEN);
  snprintf(buf, sizeof(buf), "%d", heat);
  drawRowNum(heatNumCanvas, ROW_NUM_X, BTN_HEAT_ROW_Y, buf, ILI9341_RED);
  drawToggleButton(BTN_DRUM_X, BTN_DRUM_Y, BTN_DRUM_WIDTH, BTN_DRUM_HEIGHT,
                    "DRUM", drumOn);
  drawToggleButton(BTN_COOL_X, BTN_COOL_Y, BTN_COOL_WIDTH, BTN_COOL_HEIGHT,
                    "COOL", coolOn);

  char connBuf[64];
  snprintf(connBuf, sizeof(connBuf), "WiFi:%s WS:%s BLE:%s USB:%s", wifiStatus,
           wsStatus, bleStatus, usbStatus);
  connCanvas.fillScreen(0);
  connCanvas.setTextSize(1);
  connCanvas.setCursor(3, 4);
  connCanvas.print(connBuf);
  tft.drawBitmap(0, CONN_STATUS_Y, connCanvas.getBuffer(), CONN_STATUS_WIDTH,
                 CONN_STATUS_HEIGHT, ILI9341_WHITE, ILI9341_BLUE);
}

// -----------------------------------------------------------------------------
// Minimal LVGL bring-up (lvgl-ui branch) -- proves the library links, its
// config resolves, and it can push real pixels through the existing tft
// object, before any of the dashboard gets rebuilt as LVGL widgets.
// -----------------------------------------------------------------------------
#include "ble.h"
#include "bt_probe.h"
#include "et_probe.h"
#include "comms_mode.h"
#include "temp_smoothing.h"
#include "touch.h"
#include "weather.h"
#include "wifi_setup.h"
#include <Preferences.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <lvgl.h>

// Cross-file state the dashboard reads (read-only, nothing here writes any
// of it) -- declared directly rather than pulling in each owning module's
// full header, same pattern main.cpp already uses for ble.cpp's flags.
extern double temp;                        // main.cpp
extern double ror;                         // main.cpp
extern unsigned long lastUsbActivityTime;  // main.cpp
extern bool artisanHandshakeDone;          // main.cpp
extern bool deviceConnected;                // ble.cpp
extern bool hibeanHandshakeDone;            // ble.cpp
extern bool wsHandshakeDone;                // CommandLoop.cpp
bool wsClientConnected();                   // CommandLoop.cpp

// sendBuffer's real definition lives in SkiComms.h, included once by
// main.cpp -- not re-included here, since it's a genuine array definition
// (not `extern`) and a second #include would duplicate-define it at link
// time. Byte indices copied from SkiComms.h's enum (VENT_BYTE/COOL_BYTE/
// DRUM_BYTE/HEAT_BYTE) -- only used to seed the new sliders/toggles with
// the current real output so they start truthful.
extern uint8_t sendBuffer[];
static const int DISP_VENT_BYTE = 0;
static const int DISP_COOL_BYTE = 2;
static const int DISP_DRUM_BYTE = 3;
static const int DISP_HEAT_BYTE = 4;

#define LVGL_SCREEN_W 320
#define LVGL_SCREEN_H 240
// LVGL's own recommendation: draw buffer >= ~1/10th of the screen. Sized in
// pixel rows so it scales automatically with LV_COLOR_DEPTH.
#define LVGL_DRAW_BUF_LINES 40

static lv_display_t *lvglDisplay = nullptr;

static void lvglFlushCb(lv_display_t *disp, const lv_area_t *area,
                        uint8_t *px_map) {
  uint32_t w = area->x2 - area->x1 + 1;
  uint32_t h = area->y2 - area->y1 + 1;
  tft.startWrite();
  tft.setAddrWindow(area->x1, area->y1, w, h);
  tft.writePixels((uint16_t *)px_map, w * h);
  tft.endWrite();
  lv_display_flush_ready(disp);
}

static uint32_t lvglTickCb() { return millis(); }

// touchGetPoint() (touch.cpp) is level-based ("touched right now, and
// where") -- exactly what an LVGL indev read callback wants. LVGL's own
// widget/event system does its own press/release/click edge-detection on
// top of this continuous state, so no debouncing needed here.
//
// touchGetPoint() reports raw panel coordinates in the same 0..320/0..240
// space the old (non-LVGL) dashboard's button hit-testing used, and (now
// that lv_display_set_rotation() isn't used -- see lvglInit()) LVGL's own
// widget layout uses that identical space, so this is a direct pass-
// through, no transform. (An earlier version of this code applied a
// 180-degree flip here to match lv_display_set_rotation(180)'s *rendered*
// output; both got reverted together once that rotation call turned out to
// also flip every widget's position, which was wrong -- see lvglInit().)
static void lvglTouchReadCb(lv_indev_t *indev, lv_indev_data_t *data) {
  int x, y;
  if (touchGetPoint(&x, &y)) {
    data->point.x = x;
    data->point.y = y;
    data->state = LV_INDEV_STATE_PRESSED;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

// ---- Status LEDs (WiFi/WS/BLE/USB) -----------------------------------
// Replaces the old dashboard's `--`/`C-`/`CH` text codes with an lv_led
// per interface: grey = no link, yellow = linked but no handshake, green =
// handshake done. Same two-state model as before, just shown as color
// instead of text.
enum LedState { LED_OFF, LED_LINK, LED_HANDSHAKE };

static void setLedState(lv_obj_t *led, LedState state) {
  lv_palette_t palette;
  switch (state) {
  case LED_HANDSHAKE:
    palette = LV_PALETTE_GREEN;
    break;
  case LED_LINK:
    palette = LV_PALETTE_YELLOW;
    break;
  default:
    palette = LV_PALETTE_GREY;
    break;
  }
  lv_led_set_color(led, lv_palette_main(palette));
  lv_led_on(led);
}

// LED + name label, stacked in a small unstyled container -- keeps the 4
// status cells identical without repeating the same 6 lines 4 times.
// LED + name side by side (not stacked) -- a stacked layout needed enough
// cell height for the LED's own top glow/halo (bigger than its 14x14 core,
// clipped against the cell's top edge when placed flush against it) plus a
// name label below without the two touching, and kept overflowing into
// neighboring rows on real hardware. Side-by-side needs far less height,
// and vertically centering the LED (instead of pinning it to the very top)
// gives the glow headroom on both sides for free.
static lv_obj_t *createStatusCell(lv_obj_t *parent, int x, int y,
                                  const char *name, lv_obj_t **ledOut) {
  lv_obj_t *cell = lv_obj_create(parent);
  lv_obj_remove_style_all(cell);
  lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(cell, 60, 14);
  lv_obj_set_pos(cell, x, y);

  lv_obj_t *led = lv_led_create(cell);
  lv_obj_set_size(led, 8, 8);
  lv_obj_align(led, LV_ALIGN_LEFT_MID, 2, 0);
  lv_led_set_brightness(led, 255);

  lv_obj_t *label = lv_label_create(cell);
  lv_label_set_text(label, name);
  lv_obj_align(label, LV_ALIGN_LEFT_MID, 14, 0);

  *ledOut = led;
  return cell;
}

// Three screens: a brief splash at boot, the live-monitoring main dashboard,
// and a Config screen for the low-frequency-change stuff (network address,
// BLE name) that doesn't belong mixed into a live control surface -- moving
// it there also keeps Main's critical controls (Temp/STOP) reachable without
// ever needing to scroll during an actual roast.
static lv_obj_t *splashScreen = nullptr;
static lv_obj_t *mainScreen = nullptr;
static lv_obj_t *configScreen = nullptr;
static lv_obj_t *smoothingScreen = nullptr; // ET/BT smoothing sub-screen

static lv_obj_t *btLabel = nullptr;    // external MAX31865 #2 probe
static lv_obj_t *btRorLabel = nullptr; // BT rate-of-rise
static lv_obj_t *etLabel = nullptr;    // external MAX31865 #1 probe
static lv_obj_t *etRorLabel = nullptr; // ET rate-of-rise
static lv_obj_t *ntcLabel = nullptr;   // roaster's own built-in probe, raw
static lv_obj_t *wifiIpLabel = nullptr; // now lives on configScreen
static lv_obj_t *bleNameLabel = nullptr; // configScreen
static lv_obj_t *ambientLabel = nullptr; // configScreen: online ambient reading
static lv_obj_t *wifiLed = nullptr;
static lv_obj_t *wsLed = nullptr;
static lv_obj_t *bleLed = nullptr;
static lv_obj_t *usbLed = nullptr;

// ---- Dark/light theme (selected on the Config screen) --------------------
// Only the LVGL default theme's own colors (screen/button/slider/label
// backgrounds) change with this -- the black temp tiles and colored numbers
// are drawn with their own explicit lv_color_black()/lv_palette_main() calls
// (see createReadoutTile()), independent of the theme, so they stay the same
// in both modes by design.
static Preferences displayPrefs;
static const char *kDisplayPrefsNamespace = "display";
static const char *kDarkThemeKey = "dark";
static bool darkTheme = false;
static lv_obj_t *lightThemeBtn = nullptr;
static lv_obj_t *darkThemeBtn = nullptr;

static void applyTheme(lv_display_t *disp, bool dark) {
  lv_theme_t *theme =
      lv_theme_default_init(disp, lv_palette_main(LV_PALETTE_BLUE),
                            lv_palette_main(LV_PALETTE_RED), dark,
                            &lv_font_montserrat_14);
  lv_display_set_theme(disp, theme);
}

// Manual two-button "radio group" (LVGL has no built-in one) -- the active
// choice gets a blue highlight, the inactive one falls back to the theme's
// default button color. Called once after both buttons exist to set their
// initial state, then again on every selection change.
static void updateThemeButtonStyles() {
  if (!lightThemeBtn || !darkThemeBtn)
    return;
  lv_obj_set_style_bg_color(
      lightThemeBtn,
      lv_palette_main(darkTheme ? LV_PALETTE_GREY : LV_PALETTE_BLUE), 0);
  lv_obj_set_style_bg_color(
      darkThemeBtn,
      lv_palette_main(darkTheme ? LV_PALETTE_BLUE : LV_PALETTE_GREY), 0);
}

static void setTheme(bool dark) {
  if (dark == darkTheme)
    return;
  darkTheme = dark;
  displayPrefs.putBool(kDarkThemeKey, darkTheme);
  applyTheme(lv_display_get_default(), darkTheme);
  updateThemeButtonStyles();
}

static void lightThemeBtnCb(lv_event_t *e) { setTheme(false); }
static void darkThemeBtnCb(lv_event_t *e) { setTheme(true); }

// ---- Comms mode (WebSocket vs BLE), selected on the Config screen ---------
// WiFi and BLE can't both fit in this board's internal RAM (see comms_mode.h),
// so only one radio is brought up per boot. These two buttons pick which; the
// choice is written to NVS immediately and applied on the next reboot (the
// Reboot button below, or any power cycle). Same manual "radio group" styling
// as the theme buttons -- the button matching the *saved* mode is highlighted.
static lv_obj_t *wsModeBtn = nullptr;
static lv_obj_t *bleModeBtn = nullptr;

static void updateModeButtonStyles() {
  if (!wsModeBtn || !bleModeBtn)
    return;
  CommsMode saved = commsModeGet();
  lv_obj_set_style_bg_color(
      wsModeBtn,
      lv_palette_main(saved == COMMS_WEBSOCKET ? LV_PALETTE_BLUE : LV_PALETTE_GREY),
      0);
  lv_obj_set_style_bg_color(
      bleModeBtn,
      lv_palette_main(saved == COMMS_BLE ? LV_PALETTE_BLUE : LV_PALETTE_GREY), 0);
}

static void setCommsModeUI(CommsMode mode) {
  commsModeSet(mode); // persisted only -- takes effect on the next reboot
  updateModeButtonStyles();
}

static void wsModeBtnCb(lv_event_t *e) { setCommsModeUI(COMMS_WEBSOCKET); }
static void bleModeBtnCb(lv_event_t *e) { setCommsModeUI(COMMS_BLE); }
static void rebootBtnCb(lv_event_t *e) { ESP.restart(); }

// ---- Smoothing (ET/BT) sub-screen ----------------------------------------
// Two independent stages, each a "cycle" button (tap advances to the next
// option). Global for ET+BT, applied live via temp_smoothing.h. Median window
// 1(off)/3/5/7/9; EMA weight *100 0(off)/50/70/80/90 shown as 0.5/0.7/0.8/0.9.
static lv_obj_t *btSrcCycleLabel = nullptr;
static lv_obj_t *medianCycleLabel = nullptr;
static lv_obj_t *emaCycleLabel = nullptr;

static const int kMedianOpts[] = {1, 3, 5, 7, 9};
static const char *kMedianLabels[] = {"Off", "3", "5", "7", "9"};
static const int kMedianCount = 5;
static const int kEmaOpts[] = {0, 50, 70, 80, 90};
static const char *kEmaLabels[] = {"Off", "0.5", "0.7", "0.8", "0.9"};
static const int kEmaCount = 5;

// Index of the option nearest to `value` (the stored value may not match an
// option exactly -- e.g. Artisan FILT can set an arbitrary EMA weight).
static int nearestOptIndex(const int *opts, int count, int value) {
  int best = 0, bestDiff = 1 << 30;
  for (int i = 0; i < count; i++) {
    int d = opts[i] > value ? opts[i] - value : value - opts[i];
    if (d < bestDiff) {
      bestDiff = d;
      best = i;
    }
  }
  return best;
}

static void refreshSmoothingLabels() {
  if (btSrcCycleLabel) {
    lv_label_set_text(btSrcCycleLabel,
                      btGetSource() == BT_SRC_NTC ? "NTC" : "31865");
  }
  if (medianCycleLabel) {
    lv_label_set_text(
        medianCycleLabel,
        kMedianLabels[nearestOptIndex(kMedianOpts, kMedianCount,
                                      tempSmoothingMedian())]);
  }
  if (emaCycleLabel) {
    lv_label_set_text(emaCycleLabel,
                      kEmaLabels[nearestOptIndex(kEmaOpts, kEmaCount,
                                                 tempSmoothingEmaX100())]);
  }
}

static void medianCycleCb(lv_event_t *e) {
  int i = nearestOptIndex(kMedianOpts, kMedianCount, tempSmoothingMedian());
  tempSmoothingSetMedian(kMedianOpts[(i + 1) % kMedianCount]);
  refreshSmoothingLabels();
}

static void emaCycleCb(lv_event_t *e) {
  int i = nearestOptIndex(kEmaOpts, kEmaCount, tempSmoothingEmaX100());
  tempSmoothingSetEmaX100(kEmaOpts[(i + 1) % kEmaCount]);
  refreshSmoothingLabels();
}

static void btSrcCycleCb(lv_event_t *e) {
  btSetSource(btGetSource() == BT_SRC_MAX31865 ? BT_SRC_NTC : BT_SRC_MAX31865);
  refreshSmoothingLabels();
}

// Temp readout tile: black background box + small caption + colored number,
// matching the old (non-LVGL) dashboard's drawTempTile()/drawRorTile() look
// rather than LVGL's default light theme. Five of these now share one row
// (BT / BT RoR / ET / ET RoR / NTC) -- font dropped again, from montserrat_24
// to montserrat_18, to fit the 5th (NTC) tile into the same 320px-wide row.
static lv_obj_t *createReadoutTile(lv_obj_t *parent, int x, int y, int w,
                                   int h, const char *caption,
                                   lv_palette_t textColor) {
  lv_obj_t *tile = lv_obj_create(parent);
  lv_obj_remove_style_all(tile);
  lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(tile, w, h);
  lv_obj_set_pos(tile, x, y);
  lv_obj_set_style_bg_color(tile, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(tile, 6, 0);

  lv_obj_t *captionLabel = lv_label_create(tile);
  lv_label_set_text(captionLabel, caption);
  lv_obj_set_style_text_color(captionLabel, lv_color_white(), 0);
  lv_obj_align(captionLabel, LV_ALIGN_TOP_MID, 0, 1);

  lv_obj_t *label = lv_label_create(tile);
  lv_obj_set_style_text_font(label, &lv_font_montserrat_18, 0);
  lv_obj_set_style_text_color(label, lv_palette_main(textColor), 0);
  lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -2);
  return label;
}

// ---- Fan/Heat sliders --------------------------------------------------
// Now live (confirmed with user): every value change -- drag or -/+ tap --
// calls straight through to touch.cpp's sendFan()/sendHeat(), the same
// enqueueStateRequest()-based dispatch the old pointInRect() buttons used.
// One shared context + callback set for both rows instead of duplicating
// near-identical code twice; sendValue is a function pointer so this same
// callback set works for both without an if/else on ctx->name.
struct SliderCtx {
  lv_obj_t *slider;
  lv_obj_t *label;
  const char *name;
  void (*sendValue)(int);
};

static SliderCtx fanCtx;
static SliderCtx heatCtx;

static void updateSliderLabel(SliderCtx *ctx) {
  char buf[24];
  snprintf(buf, sizeof(buf), "%s  %d%%", ctx->name,
           (int)lv_slider_get_value(ctx->slider));
  lv_label_set_text(ctx->label, buf);
}

// Dragging snaps to multiples of 5, same step as the -/+ buttons -- LVGL's
// base slider widget has no built-in step, so this rounds by hand.
// lv_slider_set_value() doesn't itself re-fire LV_EVENT_VALUE_CHANGED (that
// event is only sent for user interaction), so calling it here to correct
// the value doesn't recurse.
static int snapToStep5(int v) {
  int snapped = ((v + 2) / 5) * 5;
  if (snapped < 0)
    snapped = 0;
  if (snapped > 100)
    snapped = 100;
  return snapped;
}

static void sliderChangedCb(lv_event_t *e) {
  SliderCtx *ctx = (SliderCtx *)lv_event_get_user_data(e);
  int raw = lv_slider_get_value(ctx->slider);
  int snapped = snapToStep5(raw);
  if (snapped != raw) {
    lv_slider_set_value(ctx->slider, snapped, LV_ANIM_OFF);
  }
  updateSliderLabel(ctx);
  ctx->sendValue(snapped);
}

static void sliderMinusCb(lv_event_t *e) {
  SliderCtx *ctx = (SliderCtx *)lv_event_get_user_data(e);
  int v = lv_slider_get_value(ctx->slider) - 5;
  if (v < 0)
    v = 0;
  lv_slider_set_value(ctx->slider, v, LV_ANIM_ON);
  updateSliderLabel(ctx);
  ctx->sendValue(v);
}

static void sliderPlusCb(lv_event_t *e) {
  SliderCtx *ctx = (SliderCtx *)lv_event_get_user_data(e);
  int v = lv_slider_get_value(ctx->slider) + 5;
  if (v > 100)
    v = 100;
  lv_slider_set_value(ctx->slider, v, LV_ANIM_ON);
  updateSliderLabel(ctx);
  ctx->sendValue(v);
}

// A row: [-] button, slider, [+] button, with a "NAME  NN%" label above.
static void createSliderRow(lv_obj_t *parent, int y, SliderCtx *ctx,
                            const char *name, int initialValue,
                            void (*sendValue)(int)) {
  ctx->sendValue = sendValue;
  // Same 4px left/right margin used for the status/temp rows above -- the
  // row used to stop well short of the right edge (plusBtn ended at x=292
  // on a 320px screen, a 28px dead gap) while the other rows ran flush to
  // 0/320; widening the slider to close that gap up to the shared margin
  // both fixes the visual inconsistency and gives the slider more usable
  // drag width.
  lv_obj_t *minusBtn = lv_button_create(parent);
  lv_obj_set_size(minusBtn, 32, 30);
  lv_obj_set_pos(minusBtn, 4, y);
  lv_obj_t *minusLabel = lv_label_create(minusBtn);
  lv_label_set_text(minusLabel, LV_SYMBOL_MINUS);
  lv_obj_center(minusLabel);

  // Shorter than the row's full available width, with a bigger gap on each
  // side than the button width alone would need -- the slider knob draws
  // as a circle centered on the current value and bulges past the track's
  // own bounds at the 0/100 extremes, which was overlapping the +/- buttons
  // when the track ran edge-to-edge right up against them. 16px gap on each
  // side (same as the original, pre-margin-change layout) is what actually
  // clears the knob -- the previous widening pass tightened this to 8px to
  // reach the shared 4px screen margin, which wasn't enough and let the
  // knob overlap the buttons again at the 0/100 extremes.
  ctx->slider = lv_slider_create(parent);
  lv_obj_set_size(ctx->slider, 216, 16);
  lv_obj_set_pos(ctx->slider, 52, y + 7);
  lv_slider_set_range(ctx->slider, 0, 100);
  lv_slider_set_value(ctx->slider, initialValue, LV_ANIM_OFF);

  lv_obj_t *plusBtn = lv_button_create(parent);
  lv_obj_set_size(plusBtn, 32, 30);
  lv_obj_set_pos(plusBtn, 284, y);
  lv_obj_t *plusLabel = lv_label_create(plusBtn);
  lv_label_set_text(plusLabel, LV_SYMBOL_PLUS);
  lv_obj_center(plusLabel);

  ctx->label = lv_label_create(parent);
  lv_obj_set_pos(ctx->label, 4, y - 16);
  ctx->name = name;
  updateSliderLabel(ctx);

  lv_obj_add_event_cb(ctx->slider, sliderChangedCb, LV_EVENT_VALUE_CHANGED,
                      ctx);
  lv_obj_add_event_cb(minusBtn, sliderMinusCb, LV_EVENT_CLICKED, ctx);
  lv_obj_add_event_cb(plusBtn, sliderPlusCb, LV_EVENT_CLICKED, ctx);
}

// ---- Drum/Cool/Stop icon buttons ---------------------------------------
static lv_obj_t *drumBtn = nullptr;
static lv_obj_t *coolBtn = nullptr;

// LV_EVENT_VALUE_CHANGED fires when a checkable button's checked state
// flips via user interaction -- lv_obj_has_state() reads back the state
// LVGL already toggled for us, same on/off semantics touch.cpp's old
// pointInRect() dispatch used (current.drum/cooling != 0 ? 0 : 100).
static void drumToggleCb(lv_event_t *e) {
  lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);
  sendDrum(lv_obj_has_state(btn, LV_STATE_CHECKED) ? 100 : 0);
}

static void coolToggleCb(lv_event_t *e) {
  lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);
  sendCool(lv_obj_has_state(btn, LV_STATE_CHECKED) ? 100 : 0);
}

static void lvglStopBtnCb(lv_event_t *e) {
  Serial.println("[LVGL] Stop tapped");
  sendStop();
}

// Plain lv_screen_load(), no animation -- Config is a low-frequency detour
// from the live dashboard, not something that needs to feel polished yet.
static void configOpenBtnCb(lv_event_t *e) { lv_screen_load(configScreen); }
static void configBackBtnCb(lv_event_t *e) { lv_screen_load(mainScreen); }
static void smoothingOpenBtnCb(lv_event_t *e) { lv_screen_load(smoothingScreen); }
static void smoothingBackBtnCb(lv_event_t *e) { lv_screen_load(configScreen); }

// One-shot (lv_timer_set_repeat_count(timer, 1)) -- LVGL deletes a
// non-repeating timer itself right after this callback returns, so there's
// nothing to clean up here beyond switching screens.
static void splashTimeoutCb(lv_timer_t *timer) { lv_screen_load(mainScreen); }

// ---- Splash fade in/hold/out ---------------------------------------------
static lv_obj_t *splashContent = nullptr;
static const uint32_t kSplashFadeMs = 500;
static const uint32_t kSplashHoldMs = 800;

// lv_obj_fade_in()/lv_obj_fade_out() both go through lv_anim_start(), which
// (by design, via remove_concurrent_anims()) deletes any existing animation
// already running on the same object+style-property before starting a new
// one. Calling fade_out() immediately after fade_in() -- as a first version
// of this did -- deleted fade_in's animation the instant fade_out was
// created, and captured fade_out's own "start" opacity as whatever fade_in
// had *just* set it to (0, since fade_in's early-apply runs synchronously
// at creation time) -- so the whole thing animated 0->0: invisible for the
// entire splash, no visible effect at all. Fix: don't create the fade-out
// animation until fade-in's animation has already finished and removed
// itself from LVGL's animation list on its own -- this one-shot timer,
// scheduled for fade-in's duration plus the hold time, is what delays that
// creation instead of firing it back-to-back in the same function.
static void splashFadeOutCb(lv_timer_t *timer) {
  lv_obj_fade_out(splashContent, kSplashFadeMs, 0);
}

// Periodic refresh -- an lv_timer registered once in lvglInit(), fires from
// inside the existing lv_timer_handler() call in lvglLoop(), so no separate
// task/polling needed. Mirrors the old displayLoop()'s status logic (same
// link/handshake flags, same USB link-timeout heuristic), just paints LEDs
// instead of building a string.
//
// Also the *only* place that syncs Fan/Heat/Drum/Cool back from the real,
// authoritative state (sendBuffer) -- these widgets are two-way: dragging a
// slider or tapping a toggle sends a command out, but commands can also
// arrive from USB/WebSocket/BLE (e.g. Artisan driving OT1/OT2 directly),
// and without this the dashboard would just sit showing whatever the last
// *local* interaction left it at, ignoring every external change. Skips
// resyncing a control that's actively being pressed right now, so this
// doesn't fight a live drag/tap out from under the person doing it.
static void lvglRefreshCb(lv_timer_t *timer) {
  char buf[16];

  // BT / BT RoR from the selected BT source (Config -> Smoothing: MAX31865 #2
  // probe, or the roaster's own NTC). btReport()/btRor already resolve the
  // choice and the auto-fallback to NTC when the probe faults, so the tile
  // always shows the effective BT rather than "--.-".
  snprintf(buf, sizeof(buf), "%.1f", btReport());
  lv_label_set_text(btLabel, buf);
  snprintf(buf, sizeof(buf), "%+.1f", btRor);
  lv_label_set_text(btRorLabel, buf);

  // ET / ET RoR from the external MAX31865 #1 probe -- "--.-" until it has
  // produced a fault-free reading (unplugged, faulted, or a non-S3 build).
  if (etSensorHealthy()) {
    snprintf(buf, sizeof(buf), "%.1f", etTemp);
    lv_label_set_text(etLabel, buf);
    snprintf(buf, sizeof(buf), "%+.1f", etRor);
    lv_label_set_text(etRorLabel, buf);
  } else {
    lv_label_set_text(etLabel, "--.-");
    lv_label_set_text(etRorLabel, "--.-");
  }

  // NTC: the roaster's own built-in probe, unconditionally (no "probe
  // present" concept for it -- it's always whatever the roaster last sent).
  snprintf(buf, sizeof(buf), "%.1f", temp);
  lv_label_set_text(ntcLabel, buf);

  // Same AP/STA-IP text the old (non-LVGL) dashboard showed -- the LED
  // alone tells you "connected or not" at a glance, but Artisan/HiBean
  // setup needs the actual IP (and now port, e.g. for Artisan's WebSocket
  // device config), which a colored dot can't show.
  if (WiFi.getMode() == WIFI_AP) {
    lv_label_set_text_fmt(wifiIpLabel, "AP:%s:%d",
                          WiFi.softAPIP().toString().c_str(), WEB_SERVER_PORT);
    setLedState(wifiLed, LED_HANDSHAKE);
  } else if (WiFi.status() == WL_CONNECTED) {
    lv_label_set_text_fmt(wifiIpLabel, "STA:%s:%d",
                          WiFi.localIP().toString().c_str(), WEB_SERVER_PORT);
    setLedState(wifiLed, LED_HANDSHAKE);
  } else {
    lv_label_set_text(wifiIpLabel, "--");
    setLedState(wifiLed, LED_OFF);
  }

  // initBLE() (main.cpp) runs several seconds after lvglInit() (there's a
  // deliberate delay(5000) between them), so the real name isn't known yet
  // when this screen is first built -- refreshing it here instead of only
  // once at creation means the Config screen shows the real name as soon as
  // it's set, whatever the exact boot timing ends up being.
  lv_label_set_text_fmt(bleNameLabel, "BLE: %s", getBleDeviceName().c_str());

  // Online ambient reading (weather.cpp). LVGL's set_text_fmt has no %f, so
  // format the floats with the C library's snprintf first.
  WeatherData wnow = weatherGet();
  if (weatherGetActiveBase().length() == 0) {
    lv_label_set_text(ambientLabel, "(finding proxy)");
  } else if (!wnow.valid) {
    lv_label_set_text(ambientLabel, "--");
  } else {
    char abuf[48];
    snprintf(abuf, sizeof(abuf), "%.1fC  %.0fhPa  %.0f%%", wnow.tempC,
             wnow.pressureHpa, wnow.humidity);
    lv_label_set_text(ambientLabel, abuf);
  }

  if (wsHandshakeDone) {
    setLedState(wsLed, LED_HANDSHAKE);
  } else if (wsClientConnected()) {
    setLedState(wsLed, LED_LINK);
  } else {
    setLedState(wsLed, LED_OFF);
  }

  if (hibeanHandshakeDone) {
    setLedState(bleLed, LED_HANDSHAKE);
  } else if (deviceConnected) {
    setLedState(bleLed, LED_LINK);
  } else {
    setLedState(bleLed, LED_OFF);
  }

  // Same "byte seen in the last 5s" heuristic main.cpp's old displayLoop()
  // used -- USB has no hardware connect/disconnect signal, so link is
  // approximated, and a stale handshake flag is cleared once it goes quiet.
  bool usbLinkActive = (millis() - lastUsbActivityTime < 5000);
  if (!usbLinkActive) {
    artisanHandshakeDone = false;
  }
  if (artisanHandshakeDone) {
    setLedState(usbLed, LED_HANDSHAKE);
  } else if (usbLinkActive) {
    setLedState(usbLed, LED_LINK);
  } else {
    setLedState(usbLed, LED_OFF);
  }

  if (!lv_obj_has_state(fanCtx.slider, LV_STATE_PRESSED)) {
    lv_slider_set_value(fanCtx.slider, sendBuffer[DISP_VENT_BYTE], LV_ANIM_OFF);
    updateSliderLabel(&fanCtx);
  }
  if (!lv_obj_has_state(heatCtx.slider, LV_STATE_PRESSED)) {
    lv_slider_set_value(heatCtx.slider, sendBuffer[DISP_HEAT_BYTE], LV_ANIM_OFF);
    updateSliderLabel(&heatCtx);
  }
  if (!lv_obj_has_state(drumBtn, LV_STATE_PRESSED)) {
    bool drumOn = sendBuffer[DISP_DRUM_BYTE] != 0;
    if (drumOn != lv_obj_has_state(drumBtn, LV_STATE_CHECKED)) {
      if (drumOn) {
        lv_obj_add_state(drumBtn, LV_STATE_CHECKED);
      } else {
        lv_obj_remove_state(drumBtn, LV_STATE_CHECKED);
      }
    }
  }
  if (!lv_obj_has_state(coolBtn, LV_STATE_PRESSED)) {
    bool coolOn = sendBuffer[DISP_COOL_BYTE] != 0;
    if (coolOn != lv_obj_has_state(coolBtn, LV_STATE_CHECKED)) {
      if (coolOn) {
        lv_obj_add_state(coolBtn, LV_STATE_CHECKED);
      } else {
        lv_obj_remove_state(coolBtn, LV_STATE_CHECKED);
      }
    }
  }
}

void lvglInit() {
  // Checkpoint 0, on the physical USB serial (not D_println/WebSerial --
  // that only reaches the WiFi debug console, not `pio device monitor`).
  // Also paints the screen solid blue immediately, before LVGL touches
  // anything, so even a totally dead lvglInit() is visually distinct from
  // both "still booting" (black, from displayInit()) and a working LVGL
  // screen (label on default light background).
  Serial.println("[LVGL] init start");
  tft.fillScreen(ILI9341_BLUE);

  // Loaded before the display/theme exist so the very first theme init
  // below (not just a later Config-screen toggle) already uses the
  // last-saved choice instead of always booting into light mode.
  displayPrefs.begin(kDisplayPrefsNamespace);
  darkTheme = displayPrefs.getBool(kDarkThemeKey, false);

  lv_init();
  lv_tick_set_cb(lvglTickCb);
  Serial.println("[LVGL] lv_init done");

  // Draw buffers go in PSRAM, not internal SRAM: tried internal SRAM first
  // (to eliminate PSRAM as a variable while chasing an unrelated crash
  // loop), but that starved BLE's nimble stack of internal RAM it needs at
  // init time -- confirmed on hardware via `pio device monitor`: "BLE_INIT:
  // hci inits failed" / "nimble host init failed" immediately after boot,
  // every single reset. BLE/WiFi need internal RAM specifically; LVGL's
  // buffers don't need to be internal, so PSRAM is the right place for them.
  size_t bufBytes = (size_t)LVGL_SCREEN_W * LVGL_DRAW_BUF_LINES * 2;
  void *buf1 = heap_caps_malloc(bufBytes, MALLOC_CAP_SPIRAM);
  void *buf2 = heap_caps_malloc(bufBytes, MALLOC_CAP_SPIRAM);
  if (!buf1 || !buf2) {
    Serial.println("[LVGL] PSRAM draw buffer alloc failed");
    return;
  }

  // Build the display fully into a local first -- displayLoop's task is
  // already running by this point (created earlier in setup(), before this
  // function is called) and its lvglLoop() starts calling lv_timer_handler()
  // the moment it sees the global lvglDisplay go non-null. LVGL isn't
  // thread-safe/reentrant, so publishing that pointer any earlier than
  // "fully configured" lets that task's lv_timer_handler() race this
  // function's own setup calls (lv_display_set_buffers/lv_label_create) --
  // that race was a second, separate bug also found on hardware (cycling
  // blue/white-bar/text) before the BLE/RAM issue above.
  lv_display_t *disp = lv_display_create(LVGL_SCREEN_W, LVGL_SCREEN_H);
  lv_display_set_flush_cb(disp, lvglFlushCb);
  lv_display_set_buffers(disp, buf1, buf2, bufBytes,
                         LV_DISPLAY_RENDER_MODE_PARTIAL);
  // Deliberately NOT calling lv_display_set_rotation() here. The earlier
  // minimal test (a single centered label) rendered upside-down, and
  // LV_DISPLAY_ROTATION_180 "fixed" it -- but that test used a single
  // centered widget, which looks identical rotated or not (center is
  // invariant under a 180-degree flip about the screen's own center). Once
  // this real, multi-widget layout was flashed, lv_display_set_rotation
  // turned out to also flip every widget's *position* (top<->bottom,
  // left<->right) along with the text -- LVGL rotation transforms the whole
  // frame, not just glyph orientation. Position was never actually wrong:
  // the flush callback writes straight into tft.setAddrWindow() with no
  // transform, through the same tft.setRotation(1) the old (pre-LVGL)
  // dashboard drew correctly-positioned content through. So this needs
  // fixing without touching position at all -- see below.
  Serial.println("[LVGL] display created");

  // Applied before any screen/widget exists so everything is built under
  // the right theme from the start, rather than needing a retroactive
  // refresh -- see the definition of applyTheme() for how a later
  // Config-screen change re-applies this live.
  applyTheme(disp, darkTheme);

  lv_indev_t *indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, lvglTouchReadCb);
  Serial.println("[LVGL] touch indev created");

  // ---- Screens -------------------------------------------------------------
  // Three screens instead of one: a brief splash at boot, the live-monitoring
  // main dashboard, and a Config screen for the low-frequency-change stuff
  // (network address, BLE name) that doesn't belong mixed into a live
  // control surface -- keeping it off Main also means Main's critical
  // controls (Temp readout, STOP) stay reachable without ever needing to
  // navigate away during an actual roast. lv_init() already made its own
  // default screen; it's unused from here on, so it's deleted once the real
  // ones are ready rather than left as a dangling leftover.
  lv_obj_t *lvglDefaultScreen = lv_screen_active();
  splashScreen = lv_obj_create(NULL);
  mainScreen = lv_obj_create(NULL);
  configScreen = lv_obj_create(NULL);

  // Fixed-layout dashboards on a fixed-size panel -- never meant to scroll.
  // LVGL's default screen is scrollable, so any future off-by-a-few-px
  // overflow (like the coolBtn/stopBtn height mismatch just fixed, which
  // pushed 2px past the bottom edge and triggered a real scrollbar) fails
  // silently as "harmless" scrolling instead of visibly broken layout.
  lv_obj_clear_flag(mainScreen, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(configScreen, LV_OBJ_FLAG_SCROLLABLE);

  // ---- Status LEDs + Config button (top row) ------------------------------
  // Moved above the temp readouts per user request -- link/handshake state
  // and the Config shortcut are check-once-in-a-while glances, so they sit
  // above the thing that actually needs continuous attention during a roast.
  // A 4px left/right margin (matches the temp row below, and the slider
  // rows further down) instead of running flush to the screen edges --
  // cells tightened from 80px to 70px spacing (each cell is only 60px
  // wide) to free room at the row's tail for the Config button -- icon-only
  // (no "Config" text) since there isn't width left in a 14px-tall row for
  // both.
  createStatusCell(mainScreen, 4, 4, "WiFi", &wifiLed);
  createStatusCell(mainScreen, 74, 4, "WS", &wsLed);
  createStatusCell(mainScreen, 144, 4, "BLE", &bleLed);
  createStatusCell(mainScreen, 214, 4, "USB", &usbLed);

  lv_obj_t *configBtn = lv_button_create(mainScreen);
  lv_obj_set_size(configBtn, 34, 20);
  lv_obj_set_pos(configBtn, 282, 1);
  lv_obj_t *configBtnLabel = lv_label_create(configBtn);
  lv_label_set_text(configBtnLabel, LV_SYMBOL_SETTINGS);
  lv_obj_center(configBtnLabel);
  lv_obj_add_event_cb(configBtn, configOpenBtnCb, LV_EVENT_CLICKED, NULL);

  // ---- Temp readouts: BT / BT RoR / ET / ET RoR / NTC (2nd row) -----------
  // Five equal tiles with a 3px margin on both screen edges and 3px gaps
  // between them (61px tile width: (320 - 3*2 - 4*3) / 5). BT/BT RoR come
  // from the external MAX31865 #2/PT1000 probe (bt_probe.cpp); ET/ET RoR
  // from the external MAX31865 #1/PT100 probe (et_probe.cpp); both show
  // "--.-" whenever their probe is absent or faulted (see lvglRefreshCb()).
  // NTC is the roaster's own built-in probe, shown unconditionally -- no
  // RoR tile for it (per user request; it's still computed internally as
  // the global `ror`/`btRor` in SkiComms.h if ever wanted later).
  btLabel = createReadoutTile(mainScreen, 3, 26, 61, 46, "BT", LV_PALETTE_RED);
  lv_label_set_text(btLabel, "--.-");
  btRorLabel = createReadoutTile(mainScreen, 67, 26, 61, 46, "BT RoR",
                                 LV_PALETTE_ORANGE);
  lv_label_set_text(btRorLabel, "--.-");
  etLabel =
      createReadoutTile(mainScreen, 131, 26, 61, 46, "ET", LV_PALETTE_CYAN);
  lv_label_set_text(etLabel, "--.-");
  etRorLabel = createReadoutTile(mainScreen, 195, 26, 61, 46, "ET RoR",
                                 LV_PALETTE_GREEN);
  lv_label_set_text(etRorLabel, "--.-");
  ntcLabel = createReadoutTile(mainScreen, 259, 26, 61, 46, "NTC",
                               LV_PALETTE_YELLOW);
  lv_label_set_text(ntcLabel, "--.-");

  // ---- Fan / Heat sliders -------------------------------------------------
  createSliderRow(mainScreen, 94, &fanCtx, "FAN", sendBuffer[DISP_VENT_BYTE],
                  sendFan);
  createSliderRow(mainScreen, 148, &heatCtx, "HEAT",
                  sendBuffer[DISP_HEAT_BYTE], sendHeat);

  // ---- Drum / Cool / Stop -------------------------------------------------
  // Cool/Stop aligned relative to Drum (rather than each given their own
  // copy-pasted y/height) so they're guaranteed vertically identical --
  // exactly this kind of drift (one button edited, the copies not updated
  // to match) was why they were visibly misaligned on real hardware.
  drumBtn = lv_button_create(mainScreen);
  lv_obj_set_size(drumBtn, 96, 34);
  lv_obj_set_pos(drumBtn, 6, 188);
  lv_obj_add_flag(drumBtn, LV_OBJ_FLAG_CHECKABLE);
  if (sendBuffer[DISP_DRUM_BYTE] != 0) {
    lv_obj_add_state(drumBtn, LV_STATE_CHECKED);
  }
  lv_obj_t *drumLabel = lv_label_create(drumBtn);
  lv_label_set_text(drumLabel, LV_SYMBOL_REFRESH " Drum");
  lv_obj_center(drumLabel);
  lv_obj_add_event_cb(drumBtn, drumToggleCb, LV_EVENT_VALUE_CHANGED, NULL);

  coolBtn = lv_button_create(mainScreen);
  lv_obj_set_size(coolBtn, 96, 34);
  lv_obj_align_to(coolBtn, drumBtn, LV_ALIGN_OUT_RIGHT_MID, 12, 0);
  lv_obj_add_flag(coolBtn, LV_OBJ_FLAG_CHECKABLE);
  if (sendBuffer[DISP_COOL_BYTE] != 0) {
    lv_obj_add_state(coolBtn, LV_STATE_CHECKED);
  }
  lv_obj_t *coolLabel = lv_label_create(coolBtn);
  lv_label_set_text(coolLabel, LV_SYMBOL_DOWN " Cool");
  lv_obj_center(coolLabel);
  lv_obj_add_event_cb(coolBtn, coolToggleCb, LV_EVENT_VALUE_CHANGED, NULL);

  lv_obj_t *stopBtn = lv_button_create(mainScreen);
  lv_obj_set_size(stopBtn, 96, 34);
  lv_obj_align_to(stopBtn, coolBtn, LV_ALIGN_OUT_RIGHT_MID, 12, 0);
  lv_obj_set_style_bg_color(stopBtn, lv_palette_main(LV_PALETTE_RED), 0);
  lv_obj_t *stopLabel = lv_label_create(stopBtn);
  lv_label_set_text(stopLabel, LV_SYMBOL_STOP " Stop");
  lv_obj_center(stopLabel);
  lv_obj_add_event_cb(stopBtn, lvglStopBtnCb, LV_EVENT_CLICKED, NULL);

  Serial.println("[LVGL] main screen built");

  // ---- Config screen: Back button, WiFi AP/STA IP, BLE device name -------
  lv_obj_t *backBtn = lv_button_create(configScreen);
  lv_obj_set_size(backBtn, 90, 30);
  lv_obj_set_pos(backBtn, 6, 6);
  lv_obj_t *backLabel = lv_label_create(backBtn);
  lv_label_set_text(backLabel, LV_SYMBOL_LEFT " Back");
  lv_obj_center(backLabel);
  lv_obj_add_event_cb(backBtn, configBackBtnCb, LV_EVENT_CLICKED, NULL);

  // Same AP/STA-IP text the main dashboard used to show inline -- moved
  // here since it only matters for one-time Artisan/HiBean network setup,
  // not during live monitoring.
  wifiIpLabel = lv_label_create(configScreen);
  lv_obj_set_pos(wifiIpLabel, 6, 50);
  lv_label_set_text(wifiIpLabel, "--");

  bleNameLabel = lv_label_create(configScreen);
  lv_obj_set_pos(bleNameLabel, 6, 74);
  lv_label_set_text(bleNameLabel, "BLE: --");

  // ---- Control-page background: Light / Dark ------------------------------
  // Only the main dashboard's own theme colors change with this (see
  // applyTheme()'s comment) -- selecting it here rather than on Main itself
  // since it's a set-once-in-a-while preference, same reasoning as the rest
  // of this screen's contents.
  lv_obj_t *themeCaption = lv_label_create(configScreen);
  lv_obj_set_pos(themeCaption, 6, 104);
  lv_label_set_text(themeCaption, "Display:");

  lightThemeBtn = lv_button_create(configScreen);
  lv_obj_set_size(lightThemeBtn, 80, 30);
  lv_obj_set_pos(lightThemeBtn, 6, 128);
  lv_obj_t *lightThemeLabel = lv_label_create(lightThemeBtn);
  lv_label_set_text(lightThemeLabel, "Light");
  lv_obj_center(lightThemeLabel);
  lv_obj_add_event_cb(lightThemeBtn, lightThemeBtnCb, LV_EVENT_CLICKED, NULL);

  darkThemeBtn = lv_button_create(configScreen);
  lv_obj_set_size(darkThemeBtn, 80, 30);
  lv_obj_set_pos(darkThemeBtn, 94, 128);
  lv_obj_t *darkThemeLabel = lv_label_create(darkThemeBtn);
  lv_label_set_text(darkThemeLabel, "Dark");
  lv_obj_center(darkThemeLabel);
  lv_obj_add_event_cb(darkThemeBtn, darkThemeBtnCb, LV_EVENT_CLICKED, NULL);

  updateThemeButtonStyles();

  // ---- Comms mode: WebSocket / BLE (radio group) + Reboot-to-apply --------
  // Right column, mirroring the Display row on the left. Picks which radio the
  // board starts (see comms_mode.h) -- WiFi/Artisan-WebSocket or BLE/HiBean.
  // Both can't share this board's internal RAM, so switching needs a reboot;
  // the tap saves to NVS and the Reboot button applies it.
  lv_obj_t *commsCaption = lv_label_create(configScreen);
  lv_obj_set_pos(commsCaption, 182, 104);
  lv_label_set_text(commsCaption, "Comms:");

  wsModeBtn = lv_button_create(configScreen);
  lv_obj_set_size(wsModeBtn, 62, 30);
  lv_obj_set_pos(wsModeBtn, 182, 128);
  lv_obj_t *wsModeLabel = lv_label_create(wsModeBtn);
  lv_label_set_text(wsModeLabel, "WS");
  lv_obj_center(wsModeLabel);
  lv_obj_add_event_cb(wsModeBtn, wsModeBtnCb, LV_EVENT_CLICKED, NULL);

  bleModeBtn = lv_button_create(configScreen);
  lv_obj_set_size(bleModeBtn, 62, 30);
  lv_obj_set_pos(bleModeBtn, 250, 128);
  lv_obj_t *bleModeLabel = lv_label_create(bleModeBtn);
  lv_label_set_text(bleModeLabel, "BLE");
  lv_obj_center(bleModeLabel);
  lv_obj_add_event_cb(bleModeBtn, bleModeBtnCb, LV_EVENT_CLICKED, NULL);

  updateModeButtonStyles();

  lv_obj_t *rebootBtn = lv_button_create(configScreen);
  lv_obj_set_size(rebootBtn, 128, 30);
  lv_obj_set_pos(rebootBtn, 182, 170);
  lv_obj_t *rebootLabel = lv_label_create(rebootBtn);
  lv_label_set_text(rebootLabel, LV_SYMBOL_REFRESH " Reboot");
  lv_obj_center(rebootLabel);
  lv_obj_add_event_cb(rebootBtn, rebootBtnCb, LV_EVENT_CLICKED, NULL);

  // ---- Smoothing sub-screen shortcut --------------------------------------
  lv_obj_t *smoothingBtn = lv_button_create(configScreen);
  lv_obj_set_size(smoothingBtn, 128, 30);
  lv_obj_set_pos(smoothingBtn, 182, 204);
  lv_obj_t *smoothingBtnLabel = lv_label_create(smoothingBtn);
  lv_label_set_text(smoothingBtnLabel, "Smoothing " LV_SYMBOL_RIGHT);
  lv_obj_center(smoothingBtnLabel);
  lv_obj_add_event_cb(smoothingBtn, smoothingOpenBtnCb, LV_EVENT_CLICKED, NULL);

  // ---- Ambient (online weather) readout -----------------------------------
  // Temp/pressure/humidity fetched by weather.cpp (plain HTTP from the PC
  // proxy) and also sent to Artisan as AT/AP/AH. Numbers only -- the montserrat
  // font has no CJK glyphs, so no city name here. Updated by lvglRefreshCb().
  lv_obj_t *ambientCaption = lv_label_create(configScreen);
  lv_obj_set_pos(ambientCaption, 6, 170);
  lv_label_set_text(ambientCaption, "Ambient:");

  ambientLabel = lv_label_create(configScreen);
  lv_obj_set_pos(ambientLabel, 6, 194);
  lv_label_set_text(ambientLabel, "--");

  Serial.println("[LVGL] config screen built");

  // ---- BT source + Smoothing screen: three cycle selectors ----------------
  smoothingScreen = lv_obj_create(NULL);
  lv_obj_clear_flag(smoothingScreen, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *smBack = lv_button_create(smoothingScreen);
  lv_obj_set_size(smBack, 90, 30);
  lv_obj_set_pos(smBack, 6, 6);
  lv_obj_t *smBackLabel = lv_label_create(smBack);
  lv_label_set_text(smBackLabel, LV_SYMBOL_LEFT " Back");
  lv_obj_center(smBackLabel);
  lv_obj_add_event_cb(smBack, smoothingBackBtnCb, LV_EVENT_CLICKED, NULL);

  lv_obj_t *smTitle = lv_label_create(smoothingScreen);
  lv_obj_set_pos(smTitle, 6, 42);
  lv_label_set_text(smTitle, "BT source + Smoothing");

  // Row 1: BT source (MAX31865 #2 probe vs the roaster's own NTC).
  lv_obj_t *srcCap = lv_label_create(smoothingScreen);
  lv_obj_set_pos(srcCap, 6, 80);
  lv_label_set_text(srcCap, "BT source:");
  lv_obj_t *srcBtn = lv_button_create(smoothingScreen);
  lv_obj_set_size(srcBtn, 96, 36);
  lv_obj_set_pos(srcBtn, 200, 74);
  btSrcCycleLabel = lv_label_create(srcBtn);
  lv_obj_center(btSrcCycleLabel);
  lv_obj_add_event_cb(srcBtn, btSrcCycleCb, LV_EVENT_CLICKED, NULL);

  // Row 2: stage-1 median window (both ET & BT).
  lv_obj_t *medCap = lv_label_create(smoothingScreen);
  lv_obj_set_pos(medCap, 6, 126);
  lv_label_set_text(medCap, "Median:");
  lv_obj_t *medBtn = lv_button_create(smoothingScreen);
  lv_obj_set_size(medBtn, 96, 36);
  lv_obj_set_pos(medBtn, 200, 120);
  medianCycleLabel = lv_label_create(medBtn);
  lv_obj_center(medianCycleLabel);
  lv_obj_add_event_cb(medBtn, medianCycleCb, LV_EVENT_CLICKED, NULL);

  // Row 3: stage-2 EMA weight (both ET & BT).
  lv_obj_t *emaCap = lv_label_create(smoothingScreen);
  lv_obj_set_pos(emaCap, 6, 172);
  lv_label_set_text(emaCap, "EMA:");
  lv_obj_t *emaBtn = lv_button_create(smoothingScreen);
  lv_obj_set_size(emaBtn, 96, 36);
  lv_obj_set_pos(emaBtn, 200, 166);
  emaCycleLabel = lv_label_create(emaBtn);
  lv_obj_center(emaCycleLabel);
  lv_obj_add_event_cb(emaBtn, emaCycleCb, LV_EVENT_CLICKED, NULL);

  lv_obj_t *smHint = lv_label_create(smoothingScreen);
  lv_obj_set_pos(smHint, 6, 214);
  lv_label_set_text(smHint, "Live. Bigger median/EMA = smoother, slower.");

  refreshSmoothingLabels(); // seed the three buttons with current values

  // ---- Splash screen -------------------------------------------------------
  // Title + tagline, faded in then out as one unit via lv_obj_fade_in()/
  // lv_obj_fade_out() (LVGL's own built-in lv_anim-based helpers -- no
  // hand-rolled lv_anim_t needed for a plain opacity fade). Both labels sit
  // in one transparent container so a single pair of fade calls animates
  // them together instead of needing to keep two separate animations in
  // sync.
  splashContent = lv_obj_create(splashScreen);
  lv_obj_remove_style_all(splashContent);
  lv_obj_clear_flag(splashContent, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(splashContent, LVGL_SCREEN_W, LVGL_SCREEN_H);
  lv_obj_center(splashContent);

  lv_obj_t *splashTitle = lv_label_create(splashContent);
  lv_obj_set_style_text_font(splashTitle, &lv_font_montserrat_32, 0);
  lv_label_set_text(splashTitle, "Trident");
  lv_obj_align(splashTitle, LV_ALIGN_CENTER, 0, -14);

  lv_obj_t *splashSubtitle = lv_label_create(splashContent);
  lv_label_set_text(splashSubtitle, "WiFi  BLE  USB  to Skywalker");
  lv_obj_align(splashSubtitle, LV_ALIGN_CENTER, 0, 22);

  // Fade in immediately; the fade-out animation is deliberately NOT created
  // here too (see splashFadeOutCb()'s comment for why that broke the whole
  // effect) -- a separate one-shot timer creates it only once fade-in has
  // already finished. lv_obj_fade_in() always animates opa 0->COVER
  // regardless of the object's current opa, so there's no visible flash of
  // full opacity before this ever runs (this all happens before the first
  // lv_timer_handler() tick, so nothing has been rendered yet either way).
  lv_obj_fade_in(splashContent, kSplashFadeMs, 0);
  lv_timer_t *splashFadeOutTimer =
      lv_timer_create(splashFadeOutCb, kSplashFadeMs + kSplashHoldMs, NULL);
  lv_timer_set_repeat_count(splashFadeOutTimer, 1);

  // Hands off to Main right as the fade-out finishes.
  lv_timer_t *splashTimer = lv_timer_create(
      splashTimeoutCb, kSplashFadeMs * 2 + kSplashHoldMs, NULL);
  lv_timer_set_repeat_count(splashTimer, 1);

  Serial.println("[LVGL] splash screen built");

  // Periodic refresh, same 250ms cadence the old displayDashboard() loop
  // used -- fires from inside lv_timer_handler(), no separate task needed.
  lv_timer_create(lvglRefreshCb, 250, NULL);

  // No longer needed now that the real screens exist.
  lv_obj_delete(lvglDefaultScreen);
  lv_screen_load(splashScreen);

  // Publish last: only from here on can displayLoop's task legally touch
  // LVGL, and lvglInit() itself never calls lv_timer_handler(), so there's
  // exactly one caller of it from now on.
  lvglDisplay = disp;
  Serial.println("[LVGL] init complete");
}

void lvglLoop() {
  // Guards the startup window before lvglInit() has published lvglDisplay
  // (see the comment in lvglInit() -- this is what makes that safe).
  if (lvglDisplay == nullptr) {
    return;
  }
  lv_timer_handler();
}
#endif
