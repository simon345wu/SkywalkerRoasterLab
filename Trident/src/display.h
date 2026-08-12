#pragma once
#include <Arduino.h>

// Goouuu ESP32-S3 expansion board + 2.8" ILI9341 TFT (pins confirmed working
// on real hardware)
#define TFT_MISO  46
#define TFT_MOSI  45
#define TFT_SCLK  3
#define TFT_CS    14
#define TFT_DC    47
#define TFT_RST   21

// XPT2046 touch controller. On this board it does NOT share the TFT's SPI
// pins -- it's wired to its own completely separate SPI bus. Confirmed
// against a source describing this exact board (its TFT pin list matches
// ours above exactly), after the shared-bus assumption caused touch to read
// a constant false-positive (floating MISO on the wrong pins).
#define TOUCH_CS   1
#define TOUCH_CLK  42
#define TOUCH_MOSI 2
#define TOUCH_MISO 41

// On-screen button layout, landscape 320x240 (tft.setRotation(1)). Shared
// between display.cpp (drawing) and touch.cpp (hit-testing) so they can't
// drift out of sync. Two rows of 4 (Fan: 0/-/+/100, Heat: 0/-/+/100) each
// followed by a live readout of that row's value, then STOP below.
#define BTN_WIDTH   54
#define BTN_HEIGHT  48
#define BTN_COL0_X  2
#define BTN_COL1_X  59
#define BTN_COL2_X  116
#define BTN_COL3_X  173

// Live value readout at the end of each button row.
#define ROW_NUM_X     230
#define ROW_NUM_WIDTH 88

#define BTN_FAN_ROW_Y  84
#define BTN_HEAT_ROW_Y 138

#define BTN_FAN_ZERO_X  BTN_COL0_X
#define BTN_FAN_MINUS_X BTN_COL1_X
#define BTN_FAN_PLUS_X  BTN_COL2_X
#define BTN_FAN_MAX_X   BTN_COL3_X

#define BTN_HEAT_ZERO_X  BTN_COL0_X
#define BTN_HEAT_MINUS_X BTN_COL1_X
#define BTN_HEAT_PLUS_X  BTN_COL2_X
#define BTN_HEAT_MAX_X   BTN_COL3_X

#define BTN_STOP_WIDTH  160
#define BTN_STOP_X      ((320 - BTN_STOP_WIDTH) / 2)
#define BTN_STOP_Y      192
#define BTN_STOP_HEIGHT 40

// Drum/Cool on/off toggles, same row as STOP (which is centered, leaving
// room on both sides). handleCOOL() actually takes a 0-100 percentage, but
// there's no screen room left for a full C0/C-/C+/C100 row, so Cool is a
// toggle (0/100) like Drum, not a percentage control, per user's choice.
// Both buttons' fill color changes with state, so (unlike the static
// buttons) they're redrawn every dashboard refresh, not just once at boot.
#define BTN_DRUM_X      4
#define BTN_DRUM_WIDTH  70
#define BTN_DRUM_Y      BTN_STOP_Y
#define BTN_DRUM_HEIGHT BTN_STOP_HEIGHT

#define BTN_COOL_WIDTH  70
#define BTN_COOL_X      (320 - 4 - BTN_COOL_WIDTH)
#define BTN_COOL_Y      BTN_STOP_Y
#define BTN_COOL_HEIGHT BTN_STOP_HEIGHT

void displayInit();

// Minimal LVGL bring-up (lvgl-ui branch). Reuses the same tft object/SPI
// bus displayInit() already sets up -- call lvglInit() after displayInit(),
// then lvglLoop() periodically (e.g. every 5ms) instead of displayDashboard().
void lvglInit();
void lvglLoop();

// Dashboard: Temp / ROR / Heat / Fan tiles, Drum/Cool toggles, plus a small
// connection-status line.
void displayDashboard(float temp, float ror, uint8_t heat, uint8_t fan,
                       bool drumOn, bool coolOn, const char *wifiStatus,
                       const char *wsStatus, const char *bleStatus,
                       const char *usbStatus);
