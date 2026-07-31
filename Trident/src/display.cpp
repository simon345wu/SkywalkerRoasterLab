#include "display.h"
#ifdef NO_DISPLAY
void displayInit() {}
void displayMessage(const char *message) {}
#else
#include <Adafruit_ILI9341.h>
#include <Fonts/FreeSans9pt7b.h>
#include <SPI.h>

Adafruit_ILI9341 tft = Adafruit_ILI9341(TFT_CS, TFT_DC, TFT_RST);

// Custom fonts position the cursor at the text baseline, not the top-left
// corner, so the first line needs a downward offset to stay on-screen.
#define TEXT_LEFT_MARGIN 5
#define TEXT_TOP_BASELINE 20
#define STATUS_AREA_WIDTH 320
#define STATUS_AREA_HEIGHT 80

// Off-screen 1bpp buffer for the status text. Each update is fully composed
// here (invisible) and then blitted to the panel in one shot, so the panel
// never shows an intermediate blank/cleared frame the way clear-then-redraw
// directly on the TFT does.
GFXcanvas1 statusCanvas(STATUS_AREA_WIDTH, STATUS_AREA_HEIGHT);

void displayInit() {
  SPI.begin(TFT_SCLK, TFT_MISO, TFT_MOSI);
  tft.begin();
  tft.setRotation(1);
  tft.fillScreen(ILI9341_BLUE);
  statusCanvas.setFont(&FreeSans9pt7b);
  statusCanvas.setTextWrap(true);
  displayMessage("Ready");
}
void displayMessage(const char *message) {
  statusCanvas.fillScreen(0);
  statusCanvas.setCursor(TEXT_LEFT_MARGIN, TEXT_TOP_BASELINE);
  statusCanvas.print(message);
  tft.drawBitmap(0, 0, statusCanvas.getBuffer(), STATUS_AREA_WIDTH,
                 STATUS_AREA_HEIGHT, ILI9341_WHITE, ILI9341_BLUE);
}
#endif
