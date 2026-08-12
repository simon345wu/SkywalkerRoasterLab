#include "display.h"
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
// even though the underlying calculation ([SkiComms.h](src/SkiComms.h)'s
// updateROR()) is a first pass and hasn't been validated against a real
// roast yet. Small classic-font caption ("ROR"), but the value uses the same
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
  tft.fillScreen(ILI9341_BLUE);
  drawButtons();
  tempCanvas.setFont(&FreeSansBold24pt7b);
  fanNumCanvas.setFont(&FreeSansBold24pt7b);
  heatNumCanvas.setFont(&FreeSansBold24pt7b);
  displayDashboard(0, 0, 0, 0, false, false, "--", "--", "--", "--");
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
#endif
