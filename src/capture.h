// WiFi packet capture to microSD as .pcap (802.11 + radiotap, opens in Wireshark).
//  * frames come from the sniffer callback (management + data, whatever
//    channel the radio is on: hopping, locked, or the joined AP's channel)
//  * copied into a PSRAM ring so the WiFi task never waits on the card;
//    a writer task drains it to /pcap/cap_NNNN.pcap
//  * files roll over at 100 MB; the card is mounted on first use
#pragma once
#include <Arduino.h>

namespace capture {

enum State : uint8_t {
  CAP_OFF,       // idle
  CAP_STARTING,  // mounting card / opening file
  CAP_REC,       // recording
  CAP_STOPPING,  // draining buffer, closing file
  CAP_ERROR,     // last start/write failed (see Status::msg)
};

struct Status {
  State state;
  char file[24];         // current/last file, e.g. "/pcap/cap_0003.pcap"
  uint32_t packets;      // written this session
  uint32_t bytes;        // file bytes this session (all parts)
  uint32_t dropped;      // frames lost because the buffer was full
  uint32_t startedAt;    // millis()
  uint64_t cardFree;     // bytes, 0 if unknown
  char msg[40];          // last error / info
};

void begin();     // allocate buffer, start writer task (card mounts lazily)
void start();     // request recording (async; watch status())
void stop();      // request stop (async)
bool active();    // starting, recording or stopping
const Status& status();

// From the WiFi promiscuous callback. 802.11 frame without FCS.
void onWifiFrame(const uint8_t* frame, uint32_t len, uint8_t channel, int8_t rssi);

}  // namespace capture
