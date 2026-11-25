// ST77922 QSPI panel driver.
// The panel is natively 320x480 portrait. We render a 480x320 landscape frame
// in a PSRAM sprite and rotate it in strips into internal DMA buffers while
// the previous strip is being clocked out over QSPI.
#pragma once
#include <Arduino.h>

namespace display {

bool begin();
void setBacklight(uint8_t level);  // 0..255
// Push a full 480x320 landscape frame (RGB565, byte-swapped as stored by
// LovyanGFX/TFT_eSPI sprites) to the panel.
void pushFrame(const uint16_t* frame);

}  // namespace display
