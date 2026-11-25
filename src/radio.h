// Passive radio capture: WiFi promiscuous sniffer (2.4 GHz) + BLE observer.
#pragma once
#include <Arduino.h>
#include "devices.h"

namespace radio {

struct ChannelStats {
  uint32_t packets;  // total since boot
  int8_t maxRssi;    // strongest seen in the current window
};

bool begin();
void update();                    // call every loop: drives channel hopping
bool poll(Obs& out);              // pop one observation (main task)

void lockChannel(uint8_t ch);     // 0 = resume hopping
// While connected to a network the radio is pinned to the AP's channel
// (hopping would drop the link). 0 = released.
void setStationChannel(uint8_t ch);
uint8_t stationChannel();
void setPromiscuous(bool on);
uint8_t channel();
bool hopping();
void setWifiEnabled(bool on);
void setBleEnabled(bool on);
bool wifiEnabled();
bool bleEnabled();

// Activity counters for the spectrum view
const volatile ChannelStats* channelStats();  // index 1..13
uint32_t wifiPackets();
uint32_t blePackets();
uint32_t dropped();

}  // namespace radio
