// Sound: ES8311 codec + I2S. Runs a synth task that produces UI blips,
// alert chirps and a Geiger-counter style proximity ticker.
#pragma once
#include <Arduino.h>

namespace audio {

bool begin();               // Wire must already be started
void setEnabled(bool on);
bool enabled();
void setVolume(uint8_t v);  // 0..100 (0 = mute)

void blip();                // short UI click
void alert();               // new-threat chirp
// Proximity ticker. strength 0..1 (0 = far, 1 = on top of it); <0 disables.
void setGeiger(float strength);

}  // namespace audio
