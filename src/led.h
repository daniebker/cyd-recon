// Single WS2812 status LED: idle breathing, alert flashes, proximity heat.
#pragma once
#include <Arduino.h>

namespace led {

void begin();
void update();                // call from main loop
void flash(uint16_t color565);// alert flash burst
void setHeat(float h);        // 0..1 tracking proximity, <0 = idle

}  // namespace led
