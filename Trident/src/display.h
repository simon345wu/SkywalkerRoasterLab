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

void displayInit();

void displayMessage(const char *);
