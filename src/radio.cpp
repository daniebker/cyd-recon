#include "radio.h"
#include <NimBLEDevice.h>
#include <WiFi.h>
#include "capture.h"
#include "esp_wifi.h"

namespace {

QueueHandle_t s_queue = nullptr;
volatile radio::ChannelStats s_ch[WIFI_MAX_CH + 1];
volatile uint32_t s_wifiPkts = 0, s_blePkts = 0, s_dropped = 0;
volatile uint8_t s_channel = 1;
uint8_t s_lockedCh = 0;
uint8_t s_stationCh = 0;  // set while we're associated: radio must stay put
bool s_wifiOn = true, s_bleOn = true;
uint32_t s_lastHop = 0;

// ---------------------------------------------------------------------------
// Per-MAC rate limiter so a chatty device can't flood the queue.
// One instance per producer task (WiFi task / NimBLE host task).
struct RateLimiter {
  struct Slot {
    uint8_t mac[6];
    uint32_t t;
  } slots[48];
  uint8_t next = 0;
  bool allow(const uint8_t* mac, uint32_t now, uint32_t minGapMs) {
    for (auto& s : slots) {
      if (memcmp(s.mac, mac, 6) == 0) {
        if (now - s.t < minGapMs) return false;
        s.t = now;
        return true;
      }
    }
    Slot& s = slots[next];
    next = (next + 1) % 48;
    memcpy(s.mac, mac, 6);
    s.t = now;
    return true;
  }
};
RateLimiter s_wifiRL, s_bleRL;

void push(const Obs& o) {
  if (xQueueSend(s_queue, &o, 0) != pdTRUE) s_dropped = s_dropped + 1;
}

// ---------------------------------------------------------------------------
// Open Drone ID (ASTM F3411) message decoding: one 25-byte message
void decodeOdid(const uint8_t* m, RemoteId& r) {
  const uint8_t type = m[0] >> 4;
  switch (type) {
    case 0x0: {  // Basic ID
      r.uaType = m[1] & 0x0F;
      memcpy(r.serial, m + 2, 20);
      r.serial[20] = 0;
      for (int i = 0; i < 20; i++)
        if (r.serial[i] && (r.serial[i] < 32 || r.serial[i] > 126)) r.serial[i] = '?';
      break;
    }
    case 0x1: {  // Location / vector
      const uint8_t flags = m[1];
      const uint8_t spd = m[3];
      r.speedCms = (flags & 0x01) ? (uint16_t)((spd * 0.75f + 63.75f) * 100) : (uint16_t)(spd * 25);
      memcpy(&r.lat, m + 5, 4);
      memcpy(&r.lon, m + 9, 4);
      uint16_t geo;
      memcpy(&geo, m + 15, 2);
      r.altM = geo ? (int16_t)(geo * 0.5f - 1000.0f) : INT16_MIN;
      break;
    }
    case 0x4: {  // System (operator location)
      memcpy(&r.opLat, m + 2, 4);
      memcpy(&r.opLon, m + 6, 4);
      break;
    }
    default:
      break;
  }
  r.present = true;
}

// Message pack (type 0xF) or single message
void decodeOdidBlock(const uint8_t* p, int len, RemoteId& r) {
  if (len < 25) return;
  if ((p[0] >> 4) == 0xF) {
    const int size = p[1];
    const int count = p[2];
    if (size != 25) return;
    for (int i = 0; i < count && 3 + (i + 1) * 25 <= len; i++) decodeOdid(p + 3 + i * 25, r);
  } else {
    decodeOdid(p, r);
  }
}

// ---------------------------------------------------------------------------
// WiFi sniffer
void parseIEs(const uint8_t* ie, int len, Obs& o, bool& rsn, bool& wpa) {
  int i = 0;
  while (i + 2 <= len) {
    const uint8_t id = ie[i], l = ie[i + 1];
    if (i + 2 + l > len) break;
    const uint8_t* v = ie + i + 2;
    switch (id) {
      case 0:  // SSID
        if (!(o.flags & OBS_PROBE) || l) {
          int n = min<int>(l, 32);
          bool allZero = true;
          for (int k = 0; k < n; k++) allZero &= v[k] == 0;
          if (n == 0 || allZero) {
            o.flags |= OBS_HIDDEN;
          } else {
            for (int k = 0; k < n; k++) o.name[k] = (v[k] >= 32 && v[k] < 127) ? v[k] : '.';
            o.name[n] = 0;
          }
        }
        break;
      case 3:  // DS parameter set: real channel
        if (l >= 1) o.channel = v[0];
        break;
      case 48:
        rsn = true;
        break;
      case 221:  // vendor specific
        if (l >= 4 && v[0] == 0x00 && v[1] == 0x50 && v[2] == 0xF2 && v[3] == 0x01) wpa = true;
        if (l >= 6 && v[0] == 0xFA && v[1] == 0x0B && v[2] == 0xBC && v[3] == 0x0D) {
          // ASTM Remote ID: OUI, type 0x0D, msg counter, message pack
          decodeOdidBlock(v + 5, l - 5, o.rid);
        }
        break;
    }
    i += 2 + l;
  }
}

void wifiRx(void* buf, wifi_promiscuous_pkt_type_t type) {
  const auto* pkt = (const wifi_promiscuous_pkt_t*)buf;
  const int len = (int)pkt->rx_ctrl.sig_len - 4;  // strip FCS
  if (len < 24) return;
  const uint8_t* f = pkt->payload;
  const uint8_t ch = pkt->rx_ctrl.channel;
  const int8_t rssi = pkt->rx_ctrl.rssi;
  if (pkt->rx_ctrl.rx_state == 0) capture::onWifiFrame(f, len, ch, rssi);  // no-op unless recording
  s_wifiPkts = s_wifiPkts + 1;
  if (ch >= 1 && ch <= WIFI_MAX_CH) {
    s_ch[ch].packets = s_ch[ch].packets + 1;
    if (rssi > s_ch[ch].maxRssi) s_ch[ch].maxRssi = rssi;
  }

  const uint8_t ftype = (f[0] >> 2) & 0x3;
  const uint8_t subtype = (f[0] >> 4) & 0xF;
  const uint8_t* addr2 = f + 10;  // transmitter
  if (addr2[0] & 0x01) return;    // multicast source = junk

  const uint32_t now = millis();
  Obs o;
  memset(&o, 0, sizeof(o));
  o.rssi = rssi;
  o.channel = ch;
  memcpy(o.mac, addr2, 6);

  if (ftype == 0) {  // management
    if (subtype == 8 || subtype == 5) {  // beacon / probe response
      if (len < 36) return;
      if (!s_wifiRL.allow(addr2, now, 120)) return;
      o.medium = MED_WIFI_AP;
      const uint16_t cap = f[34] | (f[35] << 8);
      bool rsn = false, wpa = false;
      parseIEs(f + 36, len - 36, o, rsn, wpa);
      o.security = rsn ? 3 : wpa ? 2 : (cap & 0x0010) ? 1 : 0;
      push(o);
    } else if (subtype == 4) {  // probe request
      if (!s_wifiRL.allow(addr2, now, 150)) return;
      o.medium = MED_WIFI_STA;
      o.flags = OBS_PROBE;
      bool rsn, wpa;
      parseIEs(f + 24, len - 24, o, rsn, wpa);
      o.flags &= ~OBS_HIDDEN;
      push(o);
    } else if (subtype == 13 && len > 30) {  // action
      // NAN service discovery frames carry Remote ID on some drones
      const uint8_t* b = f + 24;
      if (b[0] == 0x04 && b[1] == 0x09 && b[2] == 0x50 && b[3] == 0x6F && b[4] == 0x9A && b[5] == 0x13) {
        static const uint8_t kOdidSvc[6] = {0x88, 0x69, 0x19, 0x9D, 0x92, 0x09};
        for (int i = 6; i + 6 <= len - 24; i++) {
          if (memcmp(b + i, kOdidSvc, 6) == 0) {
            o.medium = MED_WIFI_STA;
            o.flags = OBS_RID_NAN;
            push(o);
            break;
          }
        }
      }
    }
  } else if (ftype == 2) {  // data
    if (!s_wifiRL.allow(addr2, now, 250)) return;
    const bool toDS = f[1] & 0x01, fromDS = f[1] & 0x02;
    if (toDS && fromDS) return;  // WDS, skip
    o.medium = fromDS ? MED_WIFI_AP : MED_WIFI_STA;
    if (toDS) memcpy(o.bssid, f + 4, 6);  // STA -> AP: addr1 is the BSSID
    push(o);
  }
}

void setChannel(uint8_t ch) {
  s_channel = ch;
  esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
  if (ch <= WIFI_MAX_CH) s_ch[ch].maxRssi = -127;
}

// ---------------------------------------------------------------------------
// BLE observer
class BleCB : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* d) override {
    s_blePkts = s_blePkts + 1;
    const uint8_t* le = d->getAddress().getVal();  // little endian
    uint8_t mac[6];
    for (int i = 0; i < 6; i++) mac[i] = le[5 - i];
    if (!s_bleRL.allow(mac, millis(), 100)) return;

    Obs o;
    memset(&o, 0, sizeof(o));
    o.medium = MED_BLE;
    memcpy(o.mac, mac, 6);
    o.rssi = d->getRSSI();
    o.addrType = d->getAddressType();
    if (d->isConnectable()) o.flags |= OBS_CONNECTABLE;
    const auto& pl = d->getPayload();
    o.advLen = min<size_t>(pl.size(), sizeof(o.adv));
    memcpy(o.adv, pl.data(), o.advLen);

    // Walk AD structures for name + Remote ID
    int i = 0;
    while (i < o.advLen) {
      const uint8_t l = o.adv[i];
      if (l == 0 || i + 1 + l > o.advLen) break;
      const uint8_t t = o.adv[i + 1];
      const uint8_t* v = o.adv + i + 2;
      const int vl = l - 1;
      if ((t == 0x08 || t == 0x09) && vl > 0) {
        int n = min(vl, 32);
        for (int k = 0; k < n; k++) o.name[k] = (v[k] >= 32 && v[k] < 127) ? v[k] : '.';
        o.name[n] = 0;
      } else if (t == 0x16 && vl >= 4 && v[0] == 0xFA && v[1] == 0xFF && v[2] == 0x0D) {
        decodeOdidBlock(v + 4, vl - 4, o.rid);  // app code 0x0D, counter, message
      }
      i += 1 + l;
    }
    push(o);
  }
};
BleCB s_bleCB;

}  // namespace

namespace radio {

bool begin() {
  s_queue = xQueueCreate(96, sizeof(Obs));
  for (auto& c : s_ch) {
    c.packets = 0;
    c.maxRssi = -127;
  }

  // --- WiFi: station mode, never associates, promiscuous capture ---
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_ps(WIFI_PS_NONE);
  wifi_promiscuous_filter_t filt = {.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA};
  esp_wifi_set_promiscuous_filter(&filt);
  esp_wifi_set_promiscuous_rx_cb(&wifiRx);
  esp_wifi_set_promiscuous(true);
  setChannel(1);

  // --- BLE: passive observer, duplicates on so RSSI keeps updating ---
  NimBLEDevice::init("");
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(&s_bleCB, true);
  scan->setActiveScan(false);  // passive: we never transmit
  scan->setInterval(97);
  scan->setWindow(67);
  scan->setMaxResults(0);  // don't store, just stream callbacks
  scan->setDuplicateFilter(0);
  scan->start(0, false, true);
  return true;
}

void update() {
  // Keep the BLE scanner alive (it can stop on host resets)
  static uint32_t lastBleCheck = 0;
  const uint32_t now = millis();
  if (now - lastBleCheck > 2000) {
    lastBleCheck = now;
    NimBLEScan* scan = NimBLEDevice::getScan();
    if (s_bleOn && !scan->isScanning()) scan->start(0, false, true);
    if (!s_bleOn && scan->isScanning()) scan->stop();
  }
  if (!s_wifiOn) return;
  if (s_stationCh) {
    s_channel = s_stationCh;  // the WiFi driver owns the channel now
    return;
  }
  if (s_lockedCh) {
    if (s_channel != s_lockedCh) setChannel(s_lockedCh);
    return;
  }
  if (now - s_lastHop >= HOP_DWELL_MS) {
    s_lastHop = now;
    // Visit 1/6/11 twice as often: that's where most traffic lives
    static const uint8_t kOrder[] = {1, 6, 11, 2, 7, 1, 6, 11, 3, 8, 12, 1, 6, 11, 4, 9, 13, 1, 6, 11, 5, 10};
    static uint8_t pos = 0;
    pos = (pos + 1) % sizeof(kOrder);
    setChannel(kOrder[pos]);
  }
}

bool poll(Obs& out) { return s_queue && xQueueReceive(s_queue, &out, 0) == pdTRUE; }

void lockChannel(uint8_t ch) {
  s_lockedCh = ch <= WIFI_MAX_CH ? ch : 0;
  if (s_lockedCh && s_wifiOn && !s_stationCh) setChannel(s_lockedCh);
}
uint8_t channel() { return s_channel; }
bool hopping() { return s_lockedCh == 0 && s_stationCh == 0; }
void setStationChannel(uint8_t ch) { s_stationCh = ch; if (ch) s_channel = ch; }
uint8_t stationChannel() { return s_stationCh; }
void setPromiscuous(bool on) { esp_wifi_set_promiscuous(on && s_wifiOn); }

void setWifiEnabled(bool on) {
  s_wifiOn = on;
  esp_wifi_set_promiscuous(on);
}
void setBleEnabled(bool on) { s_bleOn = on; }
bool wifiEnabled() { return s_wifiOn; }
bool bleEnabled() { return s_bleOn; }

const volatile ChannelStats* channelStats() { return s_ch; }
uint32_t wifiPackets() { return s_wifiPkts; }
uint32_t blePackets() { return s_blePkts; }
uint32_t dropped() { return s_dropped; }

}  // namespace radio
