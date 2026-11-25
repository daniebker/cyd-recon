#include "devices.h"
#include <algorithm>
#include "esp_heap_caps.h"
#include "signatures.h"
#include "theme.h"

namespace {

Device* s_dev = nullptr;
int s_alertQueue[16];
int s_alertHead = 0, s_alertTail = 0;

bool sameDevice(const Device& d, const Obs& o) {
  if (!d.used || memcmp(d.mac, o.mac, 6) != 0) return false;
  // WiFi AP and STA with same MAC are the same radio; BLE is separate
  return (d.medium == MED_BLE) == (o.medium == MED_BLE);
}

void pushAlert(int idx) {
  int next = (s_alertHead + 1) % 16;
  if (next == s_alertTail) return;
  s_alertQueue[s_alertHead] = idx;
  s_alertHead = next;
}

void parseBleCompany(Device& d) {
  d.company = 0xFFFF;
  int i = 0;
  while (i < d.advLen) {
    const uint8_t l = d.adv[i];
    if (l == 0 || i + 1 + l > d.advLen) break;
    if (d.adv[i + 1] == 0xFF && l >= 3) d.company = d.adv[i + 2] | (d.adv[i + 3] << 8);
    i += 1 + l;
  }
}

}  // namespace

namespace devices {

void begin() {
  s_dev = (Device*)heap_caps_calloc(MAX_DEVICES, sizeof(Device), MALLOC_CAP_SPIRAM);
  if (!s_dev) s_dev = (Device*)calloc(MAX_DEVICES, sizeof(Device));
}

int capacity() { return MAX_DEVICES; }
Device& at(int i) { return s_dev[i]; }

int ingest(const Obs& o) {
  const uint32_t now = millis();
  int idx = -1, freeIdx = -1, oldest = -1;
  uint32_t oldestSeen = UINT32_MAX;
  for (int i = 0; i < MAX_DEVICES; i++) {
    Device& d = s_dev[i];
    if (sameDevice(d, o)) {
      idx = i;
      break;
    }
    if (!d.used) {
      if (freeIdx < 0) freeIdx = i;
    } else if (!isFlagged(d) && d.lastSeen < oldestSeen) {
      oldestSeen = d.lastSeen;
      oldest = i;
    }
  }

  bool fresh = false;
  if (idx < 0) {
    idx = freeIdx >= 0 ? freeIdx : oldest;
    if (idx < 0) return -1;
    Device& d = s_dev[idx];
    memset(&d, 0, sizeof(Device));
    d.used = true;
    d.medium = o.medium;
    memcpy(d.mac, o.mac, 6);
    d.firstSeen = now;
    d.rssiFast = d.rssiSlow = o.rssi;
    d.rssiMax = o.rssi;
    d.company = 0xFFFF;
    fresh = true;
  }

  Device& d = s_dev[idx];
  bool reclassify = fresh;

  // A STA that later beacons is really an AP
  if (o.medium == MED_WIFI_AP && d.medium == MED_WIFI_STA) {
    d.medium = MED_WIFI_AP;
    reclassify = true;
  }

  d.rssi = o.rssi;
  if (o.rssi > d.rssiMax) d.rssiMax = o.rssi;
  // Fast/slow exponential averages drive the warmer/colder indicator
  d.rssiFast += (o.rssi - d.rssiFast) * 0.35f;
  d.rssiSlow += (o.rssi - d.rssiSlow) * 0.04f;
  d.lastSeen = now;
  d.packets++;
  if (o.channel) d.channel = o.channel;

  if (o.medium == MED_BLE) {
    d.addrType = o.addrType;
    d.connectable = o.flags & OBS_CONNECTABLE;
    d.randomMac = o.addrType != 0;
    if (o.advLen && (o.advLen != d.advLen || memcmp(o.adv, d.adv, o.advLen) != 0)) {
      memcpy(d.adv, o.adv, o.advLen);
      d.advLen = o.advLen;
      parseBleCompany(d);
      reclassify = true;
    }
    if (o.name[0] && strcmp(o.name, d.name) != 0) {
      strlcpy(d.name, o.name, sizeof(d.name));
      reclassify = true;
    }
  } else {
    d.randomMac = d.mac[0] & 0x02;
    if (o.medium == MED_WIFI_AP) d.security = o.security;
    static const uint8_t kZero[6] = {0};
    if (o.medium == MED_WIFI_STA && memcmp(o.bssid, kZero, 6) != 0) memcpy(d.bssid, o.bssid, 6);
    if (o.flags & OBS_PROBE) {
      if (o.name[0] && strcmp(o.name, d.probe) != 0) {
        strlcpy(d.probe, o.name, sizeof(d.probe));
        reclassify = true;
      }
    } else if (o.name[0] && strcmp(o.name, d.name) != 0) {
      strlcpy(d.name, o.name, sizeof(d.name));
      reclassify = true;
    } else if ((o.flags & OBS_HIDDEN) && !d.name[0]) {
      strlcpy(d.name, "<hidden>", sizeof(d.name));
    }
  }

  if (o.rid.present) {
    // Keep fields we already learnt if this message type doesn't carry them
    RemoteId r = d.rid;
    r.present = true;
    if (o.rid.serial[0]) memcpy(r.serial, o.rid.serial, sizeof(r.serial));
    if (o.rid.uaType) r.uaType = o.rid.uaType;
    if (o.rid.lat || o.rid.lon) {
      r.lat = o.rid.lat;
      r.lon = o.rid.lon;
      r.altM = o.rid.altM;
      r.speedCms = o.rid.speedCms;
    }
    if (o.rid.opLat || o.rid.opLon) {
      r.opLat = o.rid.opLat;
      r.opLon = o.rid.opLon;
    }
    if (!d.rid.present) reclassify = true;
    d.rid = r;
  }
  if (o.flags & OBS_RID_NAN && !d.rid.present) {
    d.rid.present = true;
    reclassify = true;
  }

  if (reclassify) {
    const bool wasFlagged = isFlagged(d);
    signatures::classify(d);
    if (isFlagged(d) && !wasFlagged && !d.alerted) {
      d.alerted = true;
      pushAlert(idx);
    }
  }
  return idx;
}

void expire(int keep) {
  const uint32_t now = millis();
  for (int i = 0; i < MAX_DEVICES; i++) {
    Device& d = s_dev[i];
    if (d.used && i != keep && now - d.lastSeen > DEVICE_EXPIRE_MS) d.used = false;
  }
}

int count() {
  int n = 0;
  for (int i = 0; i < MAX_DEVICES; i++) n += s_dev[i].used;
  return n;
}

int countMedium(uint8_t medium) {
  int n = 0;
  for (int i = 0; i < MAX_DEVICES; i++) {
    const Device& d = s_dev[i];
    if (!d.used) continue;
    if (medium == MED_BLE ? d.medium == MED_BLE : d.medium != MED_BLE) n++;
  }
  return n;
}

int countFlagged() {
  int n = 0;
  for (int i = 0; i < MAX_DEVICES; i++) n += s_dev[i].used && isFlagged(s_dev[i]);
  return n;
}

int list(Filter f, Sort s, int* out, int maxOut) {
  int n = 0;
  for (int i = 0; i < MAX_DEVICES && n < maxOut; i++) {
    const Device& d = s_dev[i];
    if (!d.used) continue;
    switch (f) {
      case F_FLAGGED:
        if (!isFlagged(d)) continue;
        break;
      case F_WIFI:
        if (d.medium == MED_BLE) continue;
        break;
      case F_BLE:
        if (d.medium != MED_BLE) continue;
        break;
      default:
        break;
    }
    out[n++] = i;
  }
  auto byThreat = [](const Device& d) { return isFlagged(d) ? 1000 + d.category * 10 + d.confidence : 0; };
  std::sort(out, out + n, [&](int a, int b) {
    const Device& A = s_dev[a];
    const Device& B = s_dev[b];
    switch (s) {
      case S_RECENT:
        if (A.firstSeen != B.firstSeen) return A.firstSeen > B.firstSeen;
        break;
      case S_THREAT: {
        int ta = byThreat(A), tb = byThreat(B);
        if (ta != tb) return ta > tb;
        break;
      }
      default:
        break;
    }
    if ((int)A.rssiFast != (int)B.rssiFast) return A.rssiFast > B.rssiFast;
    return a < b;  // stable ordering prevents list jitter
  });
  return n;
}

int find(const uint8_t* mac, bool wifiOnly) {
  for (int i = 0; i < MAX_DEVICES; i++) {
    const Device& d = s_dev[i];
    if (d.used && memcmp(d.mac, mac, 6) == 0 && (!wifiOnly || d.medium != MED_BLE)) return i;
  }
  return -1;
}

int popAlert() {
  while (s_alertTail != s_alertHead) {
    int idx = s_alertQueue[s_alertTail];
    s_alertTail = (s_alertTail + 1) % 16;
    if (s_dev[idx].used && isFlagged(s_dev[idx])) return idx;
  }
  return -1;
}

const char* categoryName(uint8_t c) {
  switch (c) {
    case CAT_AP: return "ACCESS POINT";
    case CAT_CLIENT: return "WIFI CLIENT";
    case CAT_BLE: return "BLE DEVICE";
    case CAT_PHONE: return "PERSONAL DEVICE";
    case CAT_DRONE: return "DRONE / UAS";
    case CAT_CAMERA: return "CAMERA";
    case CAT_SURVEIL: return "SURVEILLANCE";
    case CAT_TRACKER: return "TRACKING TAG";
    case CAT_GLASSES: return "CAMERA GLASSES";
    case CAT_HACKTOOL: return "HACK TOOL";
    default: return "UNKNOWN";
  }
}

uint16_t categoryColor(uint8_t c) {
  switch (c) {
    case CAT_DRONE: return theme::RED;
    case CAT_CAMERA: return theme::ORANGE;
    case CAT_SURVEIL: return theme::MAGENTA;
    case CAT_TRACKER: return theme::YELLOW;
    case CAT_GLASSES: return theme::PURPLE;
    case CAT_HACKTOOL: return theme::GREEN;
    case CAT_AP: return theme::CYAN;
    case CAT_CLIENT: return theme::TEAL;
    case CAT_PHONE: return theme::BLUE;
    case CAT_BLE: return theme::BLUE_DIM;
    default: return theme::DIM;
  }
}

void macToStr(const uint8_t* m, char* out) {
  snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
}

}  // namespace devices
