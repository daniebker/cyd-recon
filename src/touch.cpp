#include "touch.h"
#include <Wire.h>
#include "config.h"

namespace {

// Integrated controller (Sitronix-style, 16-bit register addresses)
constexpr uint16_t ST_STATUS = 0x0001;
constexpr uint16_t ST_MAX_XY = 0x0005;
constexpr uint16_t ST_MAX_TOUCHES = 0x0009;
constexpr uint16_t ST_TOUCH_INFO = 0x0010;
constexpr uint16_t ST_POINT0 = 0x0014;
constexpr size_t ST_POINT_LEN = 7;
constexpr uint8_t ST_MAX_POINTS = 10;
// FT6336
constexpr uint8_t FT_ADDR = 0x38;

touch::State s_state;
touch::Diag s_diag;
uint32_t s_lastValidMs = 0;
uint32_t s_lastRetry = 0;
int s_errStreak = 0;
uint8_t s_stPoints = 1;  // touch slots the ST controller reports (reg 0x0009)

bool probe(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

bool readST(uint16_t reg, uint8_t* buf, size_t len) {
  Wire.beginTransmission(TOUCH_I2C_ADDR);
  Wire.write((uint8_t)(reg >> 8));
  Wire.write((uint8_t)(reg & 0xFF));
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((uint16_t)TOUCH_I2C_ADDR, len) != len) return false;
  for (size_t i = 0; i < len; i++) buf[i] = Wire.read();
  return true;
}

bool readFT(uint8_t reg, uint8_t* buf, size_t len) {
  Wire.beginTransmission(FT_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((uint16_t)FT_ADDR, len) != len) return false;
  for (size_t i = 0; i < len; i++) buf[i] = Wire.read();
  return true;
}

void hwReset() {
  pinMode(PIN_TOUCH_RST, OUTPUT);
  digitalWrite(PIN_TOUCH_RST, HIGH);
  delay(10);
  digitalWrite(PIN_TOUCH_RST, LOW);
  delay(100);
  digitalWrite(PIN_TOUCH_RST, HIGH);
  delay(200);
}

bool detect() {
  s_diag.chip = touch::CHIP_NONE;
  // The controller can take a while to boot after reset: keep trying ~2.5 s
  const uint32_t t0 = millis();
  while (millis() - t0 < 2500) {
    if (probe(TOUCH_I2C_ADDR)) {
      uint8_t st = 0xFF;
      if (readST(ST_STATUS, &st, 1)) {
        s_diag.status = st;
        if ((st & 0x0F) == 0 || millis() - t0 > 1500) {
          // Ready (or at least answering - don't let a busy status lock us out)
          uint8_t r[4];
          if (readST(ST_MAX_XY, r, 4)) {
            s_diag.maxX = ((r[0] & 0x3F) << 8) | r[1];
            s_diag.maxY = ((r[2] & 0x3F) << 8) | r[3];
          }
          uint8_t n = 0;
          s_stPoints = readST(ST_MAX_TOUCHES, &n, 1) && n >= 1 && n <= ST_MAX_POINTS ? n : ST_MAX_POINTS;
          s_diag.chip = touch::CHIP_ST_55;
          return true;
        }
      }
    } else if (probe(FT_ADDR)) {
      s_diag.chip = touch::CHIP_FT6336;
      return true;
    }
    delay(20);
  }
  return false;
}

void scanBus() {
  s_diag.nFound = 0;
  for (uint8_t a = 0x08; a < 0x78 && s_diag.nFound < sizeof(s_diag.i2cFound); a++)
    if (probe(a)) s_diag.i2cFound[s_diag.nFound++] = a;
}

void setPoint(int rx, int ry, uint32_t now) {
  s_diag.rawX = rx;
  s_diag.rawY = ry;
  // Raw coords are native portrait (320 x 480); map to landscape like the display
  s_state.x = constrain(ry, 0, SCREEN_W - 1);
  s_state.y = constrain(LCD_NATIVE_W - 1 - rx, 0, SCREEN_H - 1);
  if (!s_state.down) {
    s_diag.touches++;
    Serial.printf("[touch] down raw=(%d,%d) screen=(%d,%d)\n", rx, ry, s_state.x, s_state.y);
  }
  s_state.down = true;
  s_lastValidMs = now;
}

}  // namespace

namespace touch {

bool begin() {
  pinMode(PIN_TOUCH_INT, INPUT_PULLUP);
  hwReset();
  scanBus();
  const bool ok = detect();
  Serial.printf("[touch] i2c devices:");
  for (int i = 0; i < s_diag.nFound; i++) Serial.printf(" 0x%02X", s_diag.i2cFound[i]);
  Serial.printf("\n[touch] chip=%s status=0x%02X res=%ux%u points=%u\n", chipName(), s_diag.status, s_diag.maxX, s_diag.maxY, s_stPoints);
  return ok;
}

State read() {
  const uint32_t now = millis();
  if (s_diag.chip == CHIP_NONE) {
    // Never give up: retry detection every 3 s
    if (now - s_lastRetry > 3000) {
      s_lastRetry = now;
      if (probe(TOUCH_I2C_ADDR) || probe(FT_ADDR)) {
        hwReset();
        detect();
        Serial.printf("[touch] late detect: %s\n", chipName());
      }
    }
    return s_state;
  }

  s_diag.reads++;
  bool touched = false, ok = false;
  if (s_diag.chip == CHIP_ST_55) {
    uint8_t info = 0;
    if (readST(ST_TOUCH_INFO, &info, 1)) {
      ok = true;
      s_diag.lastInfo = info;
      if (info & 0x08) {
        // Read every point slot in one transaction, as Freenove's driver does:
        // the controller only releases its report (and INT) once the whole
        // block has been read. Reading just point 0 leaves it latched on the
        // first touch forever.
        uint8_t p[ST_POINT_LEN * ST_MAX_POINTS];
        if (readST(ST_POINT0, p, ST_POINT_LEN * s_stPoints) && (p[0] & 0x80)) {
          setPoint(((p[0] & 0x3F) << 8) | p[1], ((p[2] & 0x3F) << 8) | p[3], now);
          touched = true;
        }
      }
    }
  } else {  // FT6336
    uint8_t r[5];
    if (readFT(0x02, r, sizeof(r))) {
      ok = true;
      s_diag.lastInfo = r[0];
      if ((r[0] & 0x0F) >= 1 && (r[0] & 0x0F) <= 2) {
        setPoint(((r[1] & 0x0F) << 8) | r[2], ((r[3] & 0x0F) << 8) | r[4], now);
        touched = true;
      }
    }
  }

  if (!ok) {
    s_diag.errors++;
    if (++s_errStreak > 40) {  // bus wedged or chip reset: re-init
      s_errStreak = 0;
      Serial.println("[touch] i2c errors, resetting controller");
      hwReset();
      detect();
    }
  } else {
    s_errStreak = 0;
  }
  // Release after ~60 ms without a valid point (some controllers stop
  // reporting rather than sending an explicit "up")
  if (!touched && s_state.down && now - s_lastValidMs > 60) s_state.down = false;
  return s_state;
}

const Diag& diag() { return s_diag; }

const char* chipName() {
  switch (s_diag.chip) {
    case CHIP_ST_55: return "ST77922 TP @0x55";
    case CHIP_FT6336: return "FT6336 @0x38";
    default: return "NOT FOUND";
  }
}

}  // namespace touch
