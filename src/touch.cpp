#include "touch.h"
#include <Wire.h>
#include "config.h"

namespace {

constexpr uint16_t REG_STATUS = 0x0001;
constexpr uint16_t REG_TOUCH_INFO = 0x0010;
constexpr uint16_t REG_POINT0 = 0x0014;

touch::State s_state;
uint32_t s_lastValidMs = 0;
bool s_ok = false;

bool readRegs(uint16_t reg, uint8_t* buf, size_t len) {
  Wire.beginTransmission(TOUCH_I2C_ADDR);
  Wire.write((uint8_t)(reg >> 8));
  Wire.write((uint8_t)(reg & 0xFF));
  if (Wire.endTransmission(false) != 0) return false;
  size_t got = Wire.requestFrom((uint16_t)TOUCH_I2C_ADDR, len);
  if (got != len) return false;
  for (size_t i = 0; i < len; i++) buf[i] = Wire.read();
  return true;
}

}  // namespace

namespace touch {

bool begin() {
  pinMode(PIN_TOUCH_INT, INPUT);
  pinMode(PIN_TOUCH_RST, OUTPUT);
  digitalWrite(PIN_TOUCH_RST, LOW);
  delay(50);
  digitalWrite(PIN_TOUCH_RST, HIGH);
  delay(120);
  // Wait (bounded) for controller to report ready
  uint8_t st = 0xFF;
  for (int i = 0; i < 50; i++) {
    if (readRegs(REG_STATUS, &st, 1) && (st & 0x0F) == 0) {
      s_ok = true;
      break;
    }
    delay(10);
  }
  return s_ok;
}

State read() {
  if (!s_ok) return s_state;
  const uint32_t now = millis();
  uint8_t info = 0;
  if (readRegs(REG_TOUCH_INFO, &info, 1) && (info & 0x08)) {
    uint8_t p[7];
    if (readRegs(REG_POINT0, p, sizeof(p))) {
      if (p[0] & 0x80) {
        // Raw coords are in native portrait space (320 x 480)
        const int rx = ((p[0] & 0x3F) << 8) | p[1];
        const int ry = ((p[2] & 0x3F) << 8) | p[3];
        // Same mapping as the display (landscape, rotation 1)
        int x = ry;
        int y = LCD_NATIVE_W - 1 - rx;
        s_state.x = constrain(x, 0, SCREEN_W - 1);
        s_state.y = constrain(y, 0, SCREEN_H - 1);
        s_state.down = true;
        s_lastValidMs = now;
      } else {
        s_state.down = false;
      }
    }
  } else if (s_state.down && now - s_lastValidMs > 120) {
    // No fresh report for a while -> finger lifted
    s_state.down = false;
  }
  return s_state;
}

}  // namespace touch
