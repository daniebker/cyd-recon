// LiPo battery monitor (GPIO8 via 1:2 divider on the FNK0104N).
#pragma once
#include <Arduino.h>

namespace battery {

enum State : uint8_t {
  BAT_UNKNOWN = 0,  // still collecting samples
  BAT_DISCHARGING,
  BAT_CHARGING,     // voltage climbing / at charge ceiling
  BAT_FULL,         // sitting at the charge ceiling, not climbing
  BAT_ABSENT,       // no cell detected (reading out of range)
};

void begin();
void update();          // call from main loop; samples itself at ~4 Hz
float volts();          // smoothed cell voltage
int percent();          // 0..100, -1 if unknown/absent
State state();
bool low();             // <= 15 %
bool critical();        // <= 5 %

}  // namespace battery
