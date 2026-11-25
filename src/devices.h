// Device model + table of everything heard on the air.
#pragma once
#include <Arduino.h>
#include "config.h"

enum Medium : uint8_t { MED_WIFI_AP = 0, MED_WIFI_STA = 1, MED_BLE = 2 };

enum Category : uint8_t {
  CAT_NONE = 0,   // generic, not interesting
  CAT_AP,         // generic access point
  CAT_CLIENT,     // generic wifi client
  CAT_BLE,        // generic BLE
  CAT_PHONE,      // phones/laptops/wearables (vendor known)
  // ---- flagged categories (>= CAT_FLAGGED) ----
  CAT_FLAGGED,
  CAT_DRONE = CAT_FLAGGED,
  CAT_CAMERA,     // action cams / wifi cameras
  CAT_SURVEIL,    // ALPR / bodycam / surveillance infra
  CAT_TRACKER,    // AirTag / Tile / SmartTag ...
  CAT_GLASSES,    // camera glasses
  CAT_HACKTOOL,   // Flipper, Pineapple ...
  CAT_COUNT
};

enum Confidence : uint8_t { CONF_NONE = 0, CONF_LOW = 1, CONF_MED = 2, CONF_HIGH = 3 };

// Open Drone ID (ASTM F3411 / ASD-STAN) data decoded from WiFi or BLE
struct RemoteId {
  bool present = false;
  uint8_t uaType = 0;
  char serial[21] = {0};
  int32_t lat = 0, lon = 0;     // 1e-7 deg
  int16_t altM = INT16_MIN;     // geodetic altitude, metres
  uint16_t speedCms = 0;        // horizontal speed
  int32_t opLat = 0, opLon = 0; // operator / pilot location
};

// One raw observation from a radio callback -> main task
struct Obs {
  uint8_t medium;
  uint8_t mac[6];
  int8_t rssi;
  uint8_t channel;
  uint8_t security;  // wifi: 0 open,1 wep,2 wpa,3 wpa2/3
  uint8_t addrType;  // ble address type
  uint8_t flags;     // OBS_*
  char name[33];     // SSID / BLE local name / probed SSID
  uint8_t adv[31];
  uint8_t advLen;
  RemoteId rid;
};
enum : uint8_t {
  OBS_PROBE = 1,       // name is a probed SSID, not our own
  OBS_RID_NAN = 2,     // WiFi NAN Remote ID frame
  OBS_HIDDEN = 4,      // AP with hidden SSID
  OBS_CONNECTABLE = 8, // BLE connectable adv
};

struct Device {
  bool used;
  uint8_t medium;
  uint8_t mac[6];
  char name[33];
  char probe[33];        // last SSID a client probed for
  uint8_t category;
  uint8_t confidence;
  char label[14];        // short badge text e.g. "GOPRO"
  char vendor[20];
  char reason[48];       // why it was flagged
  int8_t rssi;           // last
  int8_t rssiMax;
  float rssiFast;        // EMA, fast
  float rssiSlow;        // EMA, slow
  uint8_t channel;
  uint8_t security;
  uint8_t addrType;
  bool connectable;
  bool randomMac;
  uint32_t firstSeen;
  uint32_t lastSeen;
  uint32_t packets;
  uint16_t company;      // BLE company id, 0xFFFF none
  uint8_t adv[31];
  uint8_t advLen;
  RemoteId rid;
  bool alerted;
};

namespace devices {

void begin();
// Merge an observation. Returns index of device (or -1).
int ingest(const Obs& o);
// Remove stale entries (never removes `keep`).
void expire(int keep);

int capacity();
Device& at(int i);
int count();
int countMedium(uint8_t medium);
int countFlagged();

// Returns indices of devices passing a filter, sorted.
enum Filter : uint8_t { F_ALL = 0, F_FLAGGED, F_WIFI, F_BLE, F_COUNT };
enum Sort : uint8_t { S_SIGNAL = 0, S_RECENT, S_THREAT, S_COUNT };
int list(Filter f, Sort s, int* out, int maxOut);

// Newly flagged devices that haven't been announced yet (-1 if none)
int popAlert();

const char* categoryName(uint8_t c);
uint16_t categoryColor(uint8_t c);
// Flagged = interesting category with at least medium confidence.
// Low-confidence hits keep their badge (shown with '?') but don't alert.
inline bool isFlagged(const Device& d) { return d.category >= CAT_FLAGGED && d.confidence >= CONF_MED; }
void macToStr(const uint8_t* mac, char* out);  // out >= 18 bytes

}  // namespace devices
