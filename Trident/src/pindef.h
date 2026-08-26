#ifndef PINDEF
#define PINDEF

// Roaster RX/TX physical layer: RMT hardware handles the pulse-width timing
// instead of CPU-timed pulseIn()/delayMicroseconds(), so it isn't thrown off
// by WiFi/BLE interrupt jitter. Comment either out to fall back to the old
// bit-banged version. See docs from the sibling TEST_SkyCommand_Node32s
// project's rmt-roaster-rx branch, where this was validated on real hardware.
#define _ROASTER_RX_RMT_
#define _ROASTER_TX_RMT_

#if defined(S3MINI) || defined(S3)
const int RX_PIN = 20;
const int TX_PIN = 19;
const int RGB_PIN = 48;
#else
const int RX_PIN = 13;
const int TX_PIN = 12;
const int RGB_PIN = 8;
#endif

// MAX31865 (ET probe) chip-select. Shares the touch controller's SPI bus
// (SCLK=42, MISO=41, MOSI=2 -- see display.h); this is its own dedicated CS,
// physically next to the touch MISO/CLK pins on the header. S3 board only.
#if defined(S3)
const int ET_CS_PIN = 40;
#endif
#endif
