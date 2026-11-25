// Capacitive touch controller on the FNK0104N (I2C @0x55, 16-bit registers).
#pragma once
#include <Arduino.h>

namespace touch {

struct State {
  bool down = false;     // finger currently on glass
  int16_t x = 0, y = 0;  // landscape screen coords
};

bool begin();     // Wire must already be started
State read();     // poll; call once per frame

}  // namespace touch
