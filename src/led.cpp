#include "led.h"
#include "config.h"

namespace {
uint32_t s_flashUntil = 0;
uint8_t s_fr = 0, s_fg = 0, s_fb = 0;
float s_heat = -1;
uint32_t s_last = 0;
uint8_t s_cr = 255, s_cg = 255, s_cb = 255;

void out(uint8_t r, uint8_t g, uint8_t b) {
  if (r == s_cr && g == s_cg && b == s_cb) return;
  s_cr = r;
  s_cg = g;
  s_cb = b;
  rgbLedWrite(PIN_WS2812, r, g, b);
}
}  // namespace

namespace led {

void begin() { out(0, 0, 0); }

void flash(uint16_t c) {
  s_fr = ((c >> 11) & 31) << 3;
  s_fg = ((c >> 5) & 63) << 2;
  s_fb = (c & 31) << 3;
  s_flashUntil = millis() + 1200;
}

void setHeat(float h) { s_heat = h; }

void update() {
  const uint32_t now = millis();
  if (now - s_last < 40) return;
  s_last = now;
  if (now < s_flashUntil) {
    const bool on = (now / 100) & 1;
    out(on ? s_fr / 3 : 0, on ? s_fg / 3 : 0, on ? s_fb / 3 : 0);
  } else if (s_heat >= 0) {
    // blue (cold) -> red (hot), pulse faster when hot
    const float h = constrain(s_heat, 0.f, 1.f);
    const float period = 1200 - 1000 * h;
    const float pulse = 0.35f + 0.65f * (0.5f + 0.5f * sinf(now / period * 6.283f));
    out((uint8_t)(80 * h * pulse), (uint8_t)(10 * pulse), (uint8_t)(80 * (1 - h) * pulse));
  } else {
    // idle: slow dim cyan breathing
    const float b = 0.5f + 0.5f * sinf(now / 1600.f * 6.283f);
    out(0, (uint8_t)(6 * b), (uint8_t)(5 * b));
  }
}

}  // namespace led
