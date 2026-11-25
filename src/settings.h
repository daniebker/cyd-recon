// User settings: sound, alerts, display. Persisted to NVS ("settings" namespace).
#pragma once
#include <Arduino.h>

struct Config {
  // Audio
  bool sound = true;        // master speaker enable
  uint8_t volume = 70;      // 0..100
  bool clicks = true;       // touch blips
  bool geiger = true;       // tracker proximity ticker
  // Alerts (new-threat detections)
  bool alertSound = true;   // chirp
  bool alertPopup = true;   // on-screen toast
  bool alertLed = true;     // RGB LED flash
  bool batteryWarn = true;  // 15 % / 5 % banner
  // Display
  uint8_t brightness = 100;  // backlight 5..100
  bool statusLed = true;     // idle breathing + tracker heat on the LED
};

namespace settings {

constexpr uint8_t MIN_BRIGHTNESS = 5;

void begin();     // load from NVS and apply (after audio/display are up)
Config& get();    // edit fields, then call changed()
void changed();   // apply to hardware and schedule a save
void reset();     // restore defaults (applied + saved)
void update();    // call from main loop: flushes pending saves
bool dirty();     // a save is pending

}  // namespace settings
