// CYD//RECON - passive radio recon firmware
// Target: Freenove ESP32-S3 Display 3.5" (FNK0104N)
//
// Sniffs 2.4 GHz WiFi management/data frames and BLE advertisements, flags
// drones, cameras, surveillance gear, trackers and hacking tools, and lets you
// tap a target to hunt it down with a hot/cold RSSI tracker.
// Receive-only until you join a network from the NETWORK screen.

#include <Arduino.h>
#include <Wire.h>
#include "esp_sleep.h"
#include "audio.h"
#include "battery.h"
#include "capture.h"
#include "config.h"
#include "devices.h"
#include "display.h"
#include "led.h"
#include "net.h"
#include "radio.h"
#include "settings.h"
#include "touch.h"
#include "ui.h"

namespace {
uint32_t s_lastFrame = 0;
uint32_t s_lastExpire = 0;
bool s_btnPrev = true;
uint32_t s_btnDownAt = 0;

// BOOT button powers the device down; the same button wakes it (which reboots).
void enterDeepSleep() {
  Serial.println("[pwr] entering deep sleep");
  if (capture::active()) {
    capture::stop();
    for (int i = 0; i < 200 && capture::active(); i++) delay(10);  // let the file close
  }
  settings::update();   // flush any pending NVS write
  net::disconnect();
  ui::showPowerOff();
  delay(900);
  display::setBacklight(0);
  led::flash(0);        // LED off
  led::update();
  // Wait for release so we don't immediately wake on the press that sent us here
  while (digitalRead(PIN_BOOT_BTN) == LOW) delay(10);
  delay(80);
  esp_sleep_enable_ext1_wakeup(1ULL << PIN_BOOT_BTN, ESP_EXT1_WAKEUP_ANY_LOW);  // wake on BOOT press
  esp_deep_sleep_start();  // never returns; wake triggers a fresh boot
}
}  // namespace

void setup() {
  Serial.begin(115200);
  delay(50);
  Serial.println("\n" FW_NAME " v" FW_VERSION);

  pinMode(PIN_BOOT_BTN, INPUT_PULLUP);
  battery::begin();
  led::begin();

  if (!display::begin()) {
    Serial.println("display init failed");
  }
  if (!ui::begin()) {
    Serial.println("framebuffer alloc failed (PSRAM?)");
    for (;;) delay(1000);
  }
  display::setBacklight(255);
  ui::bootLine("> qspi display st77922 320x480", 1);
  devices::begin();
  ui::bootLine("> psram target table", 1);

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 100000);  // touch IC is specced for 100 kHz
  ui::bootLine("> touch controller @0x55", 0);
  ui::bootLine("> touch controller @0x55", touch::begin() ? 1 : -1);

  ui::bootLine("> es8311 audio codec", 0);
  ui::bootLine("> es8311 audio codec", audio::begin() ? 1 : -1);
  settings::begin();  // volume, brightness, LED, alert prefs from NVS
  ui::bootLine("> user settings (nvs)", 1);

  capture::begin();  // before the sniffer starts feeding it

  ui::bootLine("> wifi promiscuous 2.4ghz + ble observer", 0);
  ui::bootLine("> wifi promiscuous 2.4ghz + ble observer", radio::begin() ? 1 : -1);
  net::begin();
  ui::bootLine("> uplink manager (saved networks)", 1);
  ui::bootLine("> signature db: drones cams flock tags", 1);
  ui::bootLine("> rx only until you join a network", 1);
  ui::bootLine("> jacking in...", 0);
  delay(300);
  ui::bootDone();
}

void loop() {
  radio::update();

  // Drain radio observations into the device table
  Obs o;
  int budget = 64;
  while (budget-- > 0 && radio::poll(o)) {
    const int before = devices::count();
    const int idx = devices::ingest(o);
    if (idx >= 0 && devices::count() != before) ui::onNewDevice(idx);
  }

  const uint32_t now = millis();
  if (now - s_lastExpire > 5000) {
    s_lastExpire = now;
    devices::expire(ui::trackedIndex());
  }

  led::update();
  battery::update();
  net::update();
  settings::update();

  if (now - s_lastFrame >= 1000 / TARGET_FPS) {
    s_lastFrame = now;
    // BOOT button: a press powers the device off (wake = press = reboot).
    // Trigger on release of a debounced press so a brush doesn't sleep it.
    const bool btn = digitalRead(PIN_BOOT_BTN);
    if (!btn && s_btnPrev) s_btnDownAt = now;        // falling edge
    if (btn && !s_btnPrev && now - s_btnDownAt >= 40)  // released after a real press
      enterDeepSleep();
    s_btnPrev = btn;
    ui::frame(touch::read(), false);
  } else {
    delay(1);
  }
}
