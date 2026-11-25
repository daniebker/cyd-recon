// Capacitive touch for the Freenove ESP32-S3 3.5" (FNK0104N).
// Primary: the panel's integrated touch controller at I2C 0x55 (16-bit regs).
// Fallback: FT6336 at 0x38, as used on other Freenove FNK0104 variants.
#pragma once
#include <Arduino.h>

namespace touch {

struct State {
  bool down = false;     // finger currently on glass
  int16_t x = 0, y = 0;  // landscape screen coords
};

enum Chip : uint8_t { CHIP_NONE = 0, CHIP_ST_55, CHIP_FT6336 };

struct Diag {
  Chip chip = CHIP_NONE;
  uint8_t i2cFound[16];   // addresses seen on the bus at boot
  uint8_t nFound = 0;
  uint8_t status = 0xFF;  // controller status register at init
  uint16_t maxX = 0, maxY = 0;  // resolution reported by the controller
  uint32_t reads = 0, errors = 0, touches = 0;
  uint8_t lastInfo = 0;
  int16_t rawX = -1, rawY = -1;
};

bool begin();     // Wire must already be started
State read();     // poll; call once per frame
const Diag& diag();
const char* chipName();

}  // namespace touch
