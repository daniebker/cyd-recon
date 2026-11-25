// In-memory stand-in for the ESP32 NVS Preferences API (preview only).
#pragma once
#include <stdint.h>
#include <stddef.h>
class Preferences {
 public:
  bool begin(const char*, bool = false) { return false; }  // behave like a fresh device
  void end() {}
  bool getBool(const char*, bool d = false) { return d; }
  uint8_t getUChar(const char*, uint8_t d = 0) { return d; }
  size_t putBool(const char*, bool) { return 1; }
  size_t putUChar(const char*, uint8_t) { return 1; }
};
