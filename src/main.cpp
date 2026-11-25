// CYD//RECON - passive radio recon firmware
// Target: Freenove ESP32-S3 Display 3.5" (FNK0104N)
//
// Sniffs 2.4 GHz WiFi management/data frames and BLE advertisements, flags
// drones, cameras, surveillance gear, trackers and hacking tools, and lets you
// tap a target to hunt it down with a hot/cold RSSI tracker.
// Receive-only until you join a network from the NETWORK screen.

#include <Arduino.h>
#include <Wire.h>
#include "audio.h"
#include "battery.h"
#include "config.h"
#include "devices.h"
#include "display.h"
#include "led.h"
#include "net.h"
#include "radio.h"
#include "touch.h"
#include "ui.h"

namespace {
uint32_t s_lastFrame = 0;
uint32_t s_lastExpire = 0;
bool s_btnPrev = true;
uint32_t s_btnDownAt = 0;
bool s_btnLongFired = false;
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

  if (now - s_lastFrame >= 1000 / TARGET_FPS) {
    s_lastFrame = now;
    // BOOT button: short press = back, hold 1 s = touch test screen
    const bool btn = digitalRead(PIN_BOOT_BTN);
    bool backPressed = false;
    if (!btn && s_btnPrev) s_btnDownAt = now;
    if (!btn && !s_btnLongFired && now - s_btnDownAt > 1000) {
      s_btnLongFired = true;
      ui::openTouchTest();
    }
    if (btn && !s_btnPrev) {
      if (!s_btnLongFired) backPressed = true;
      s_btnLongFired = false;
    }
    s_btnPrev = btn;
    ui::frame(touch::read(), backPressed);
  } else {
    delay(1);
  }
}
