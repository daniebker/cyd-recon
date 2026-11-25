#include "settings.h"
#include <Preferences.h>
#include "audio.h"
#include "display.h"
#include "led.h"

namespace {

constexpr uint8_t kVersion = 1;
constexpr uint32_t kSaveDelayMs = 1500;  // coalesce slider drags into one flash write

Config s_cfg;
Config s_applied;
bool s_first = true;
bool s_dirty = false;
uint32_t s_changedAt = 0;

void apply() {
  const Config& c = s_cfg;
  audio::setEnabled(c.sound);
  if (s_first || c.volume != s_applied.volume) audio::setVolume(c.volume);  // I2C write, skip if unchanged
  if (s_first || c.brightness != s_applied.brightness) {
    display::setBacklight((uint8_t)(max<uint8_t>(c.brightness, settings::MIN_BRIGHTNESS) * 255 / 100));
  }
  led::setAmbient(c.statusLed);
  s_applied = c;
  s_first = false;
}

void load() {
  Preferences p;
  if (!p.begin("settings", true)) return;  // namespace doesn't exist yet: keep defaults
  if (p.getUChar("ver", 0) == kVersion) {
    Config& c = s_cfg;
    c.sound = p.getBool("sound", c.sound);
    c.volume = min<uint8_t>(p.getUChar("vol", c.volume), 100);
    c.clicks = p.getBool("clicks", c.clicks);
    c.geiger = p.getBool("geiger", c.geiger);
    c.alertSound = p.getBool("aSnd", c.alertSound);
    c.alertPopup = p.getBool("aPop", c.alertPopup);
    c.alertLed = p.getBool("aLed", c.alertLed);
    c.batteryWarn = p.getBool("batW", c.batteryWarn);
    c.brightness = constrain(p.getUChar("bri", c.brightness), settings::MIN_BRIGHTNESS, 100);
    c.statusLed = p.getBool("sLed", c.statusLed);
  }
  p.end();
}

void save() {
  Preferences p;
  if (!p.begin("settings", false)) return;
  const Config& c = s_cfg;
  p.putUChar("ver", kVersion);
  p.putBool("sound", c.sound);
  p.putUChar("vol", c.volume);
  p.putBool("clicks", c.clicks);
  p.putBool("geiger", c.geiger);
  p.putBool("aSnd", c.alertSound);
  p.putBool("aPop", c.alertPopup);
  p.putBool("aLed", c.alertLed);
  p.putBool("batW", c.batteryWarn);
  p.putUChar("bri", c.brightness);
  p.putBool("sLed", c.statusLed);
  p.end();
}

}  // namespace

namespace settings {

void begin() {
  load();
  apply();
}

Config& get() { return s_cfg; }

void changed() {
  apply();
  s_dirty = true;
  s_changedAt = millis();
}

void reset() {
  s_cfg = Config();
  changed();
}

void update() {
  if (s_dirty && millis() - s_changedAt >= kSaveDelayMs) {
    save();
    s_dirty = false;
  }
}

bool dirty() { return s_dirty; }

}  // namespace settings
