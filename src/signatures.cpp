#include "signatures.h"
#include <string.h>
#include <ctype.h>

// ---------------------------------------------------------------------------
// Signature sources
//  * IEEE OUI registry assignments (GoPro, DJI, Parrot, Axon, Hak5 ...)
//  * Bluetooth SIG assigned numbers (company IDs, 16-bit member UUIDs)
//  * ASTM F3411 Open Drone ID (WiFi beacon vendor IE FA:0B:BC / BLE 0xFFFA)
//  * Flock Safety / Raven / SoundThinking heuristics: community research
//    collected by flock-you-esp32 (github.com/simeononsecurity/flock-you-esp32,
//    Apache-2.0) - see NOTICE. These are reverse-engineered heuristics, not
//    vendor documentation, so OUI-only hits are rated MEDIUM at best.
// ---------------------------------------------------------------------------

namespace {

struct Hit {
  uint8_t cat = CAT_NONE;
  uint8_t conf = CONF_NONE;
  const char* label = nullptr;
  char reason[48] = {0};
};

// Pick the "most interesting" match: flagged categories beat generic ones,
// then higher confidence wins.
void consider(Hit& best, uint8_t cat, uint8_t conf, const char* label, const char* reasonFmt,
              const char* arg = nullptr) {
  auto rank = [](uint8_t c, uint8_t f) { return (c >= CAT_FLAGGED ? 100 : 0) + f * 10 + (c == CAT_PHONE ? 1 : 0); };
  if (best.label && rank(cat, conf) <= rank(best.cat, best.conf)) return;
  best.cat = cat;
  best.conf = conf;
  best.label = label;
  snprintf(best.reason, sizeof(best.reason), reasonFmt, arg ? arg : "");
}

bool containsCI(const char* hay, const char* needle) {
  if (!hay || !needle || !*needle) return false;
  const size_t n = strlen(needle);
  for (; *hay; hay++) {
    if (strncasecmp(hay, needle, n) == 0) return true;
  }
  return false;
}
bool startsCI(const char* s, const char* p) { return s && strncasecmp(s, p, strlen(p)) == 0; }

// ---------------- OUI table ----------------
struct OuiSig {
  uint8_t oui[3];
  uint8_t cat;
  uint8_t conf;
  const char* label;
  const char* vendor;
};

const OuiSig kOui[] = {
    // Action cameras
    {{0x04, 0x41, 0x69}, CAT_CAMERA, CONF_HIGH, "GOPRO", "GoPro"},
    {{0xD8, 0x96, 0x85}, CAT_CAMERA, CONF_HIGH, "GOPRO", "GoPro"},
    {{0xF4, 0xDD, 0x9E}, CAT_CAMERA, CONF_HIGH, "GOPRO", "GoPro"},
    {{0xD4, 0xD9, 0x19}, CAT_CAMERA, CONF_HIGH, "GOPRO", "GoPro"},
    {{0x06, 0x41, 0x69}, CAT_CAMERA, CONF_MED, "GOPRO", "GoPro (AP)"},
    // Drones
    {{0x60, 0x60, 0x1F}, CAT_DRONE, CONF_MED, "DJI", "DJI"},
    {{0x34, 0xD2, 0x62}, CAT_DRONE, CONF_MED, "DJI", "DJI"},
    {{0x48, 0x1C, 0xB9}, CAT_DRONE, CONF_MED, "DJI", "DJI"},
    {{0x04, 0xA8, 0x5A}, CAT_DRONE, CONF_MED, "DJI", "DJI"},
    {{0x58, 0xB8, 0x58}, CAT_DRONE, CONF_MED, "DJI", "DJI"},
    {{0x90, 0x3A, 0xE6}, CAT_DRONE, CONF_MED, "PARROT", "Parrot"},
    {{0xA0, 0x14, 0x3D}, CAT_DRONE, CONF_MED, "PARROT", "Parrot"},
    {{0x00, 0x12, 0x1C}, CAT_DRONE, CONF_MED, "PARROT", "Parrot"},
    {{0x00, 0x26, 0x7E}, CAT_DRONE, CONF_MED, "PARROT", "Parrot"},
    {{0x90, 0x03, 0xB7}, CAT_DRONE, CONF_MED, "PARROT", "Parrot"},
    // Surveillance
    {{0x00, 0x25, 0xDF}, CAT_SURVEIL, CONF_MED, "AXON", "Axon/TASER"},
    {{0xB4, 0x1E, 0x52}, CAT_SURVEIL, CONF_HIGH, "FLOCK", "Flock Safety"},
    {{0xD4, 0x11, 0xD6}, CAT_SURVEIL, CONF_MED, "SHOTSPTR", "SoundThinking"},
    // Hacking gear
    {{0x00, 0x13, 0x37}, CAT_HACKTOOL, CONF_MED, "HAK5", "Hak5"},
    // Common vendors (not flagged, just labelled)
    {{0x24, 0x0A, 0xC4}, CAT_NONE, CONF_LOW, nullptr, "Espressif"},
    {{0x30, 0xAE, 0xA4}, CAT_NONE, CONF_LOW, nullptr, "Espressif"},
    {{0xA4, 0xCF, 0x12}, CAT_NONE, CONF_LOW, nullptr, "Espressif"},
    {{0x3C, 0x71, 0xBF}, CAT_NONE, CONF_LOW, nullptr, "Espressif"},
    {{0xB8, 0x27, 0xEB}, CAT_NONE, CONF_LOW, nullptr, "RaspberryPi"},
    {{0xDC, 0xA6, 0x32}, CAT_NONE, CONF_LOW, nullptr, "RaspberryPi"},
    {{0xF0, 0x9F, 0xC2}, CAT_NONE, CONF_LOW, nullptr, "Ubiquiti"},
    {{0x00, 0x1A, 0x11}, CAT_NONE, CONF_LOW, nullptr, "Google"},
    {{0x00, 0x17, 0x88}, CAT_NONE, CONF_LOW, nullptr, "Philips Hue"},
};

// Flock Safety camera WiFi OUIs (community field data, flock-you-esp32 table A).
// Several are generic module vendors, so a bare hit is MEDIUM.
const uint8_t kFlockOui[][3] = {
    {0x70, 0xC9, 0x4E}, {0x3C, 0x91, 0x80}, {0xD8, 0xF3, 0xBC}, {0x80, 0x30, 0x49}, {0xB8, 0x35, 0x32},
    {0x14, 0x5A, 0xFC}, {0x74, 0x4C, 0xA1}, {0x08, 0x3A, 0x88}, {0x9C, 0x2F, 0x9D}, {0xC0, 0x35, 0x32},
    {0x94, 0x08, 0x53}, {0xE4, 0xAA, 0xEA}, {0x24, 0xB2, 0xB9}, {0xB8, 0x1E, 0xA4}, {0x70, 0x08, 0x94},
    {0x58, 0x8E, 0x81}, {0xEC, 0x1B, 0xBD}, {0x58, 0x00, 0xE3}, {0x90, 0x35, 0xEA}, {0x5C, 0x93, 0xA2},
    {0x64, 0x6E, 0x69}, {0x48, 0x27, 0xEA}, {0xE0, 0x4F, 0x43}, {0x82, 0x6B, 0xF2}, {0x04, 0x0D, 0x84},
    {0xF0, 0x82, 0xC0}, {0x1C, 0x34, 0xF1}, {0x38, 0x5B, 0x44}, {0x94, 0x34, 0x69}, {0xB4, 0xE3, 0xF9},
};
// Factory-default QCA9377 MACs from Flock camera firmware (unprovisioned units)
const uint8_t kFlockExact[][6] = {
    {0x00, 0x03, 0x7F, 0x50, 0x00, 0x01},
    {0x00, 0x03, 0x7F, 0x4F, 0x00, 0x16},
};

// ---------------- Name / SSID patterns ----------------
enum Match : uint8_t { M_PREFIX, M_CONTAINS };
struct NameSig {
  const char* pat;
  uint8_t match;
  uint8_t cat;
  uint8_t conf;
  const char* label;
};

const NameSig kSsid[] = {
    // Surveillance
    {"flock", M_CONTAINS, CAT_SURVEIL, CONF_HIGH, "FLOCK"},
    {"penguin", M_CONTAINS, CAT_SURVEIL, CONF_MED, "FLOCK"},
    {"pigvision", M_CONTAINS, CAT_SURVEIL, CONF_HIGH, "FLOCK"},
    {"fs ext battery", M_CONTAINS, CAT_SURVEIL, CONF_HIGH, "FLOCK"},
    {"flck", M_CONTAINS, CAT_SURVEIL, CONF_MED, "FLOCK"},
    {"axon", M_PREFIX, CAT_SURVEIL, CONF_LOW, "AXON"},
    // Drones
    {"DJI", M_PREFIX, CAT_DRONE, CONF_HIGH, "DJI"},
    {"Mavic", M_CONTAINS, CAT_DRONE, CONF_HIGH, "DJI"},
    {"Phantom", M_PREFIX, CAT_DRONE, CONF_MED, "DJI"},
    {"Spark-", M_PREFIX, CAT_DRONE, CONF_MED, "DJI"},
    {"TELLO-", M_PREFIX, CAT_DRONE, CONF_HIGH, "TELLO"},
    {"RMTT-", M_PREFIX, CAT_DRONE, CONF_HIGH, "TELLO"},
    {"ANAFI", M_PREFIX, CAT_DRONE, CONF_HIGH, "PARROT"},
    {"Bebop", M_PREFIX, CAT_DRONE, CONF_HIGH, "PARROT"},
    {"Disco-", M_PREFIX, CAT_DRONE, CONF_MED, "PARROT"},
    {"Skydio", M_PREFIX, CAT_DRONE, CONF_HIGH, "SKYDIO"},
    {"Autel", M_PREFIX, CAT_DRONE, CONF_HIGH, "AUTEL"},
    {"EVO-", M_PREFIX, CAT_DRONE, CONF_LOW, "AUTEL"},
    {"HUBSAN", M_PREFIX, CAT_DRONE, CONF_HIGH, "HUBSAN"},
    {"Holy", M_PREFIX, CAT_DRONE, CONF_LOW, "DRONE"},
    {"WIFI-UAV", M_CONTAINS, CAT_DRONE, CONF_HIGH, "DRONE"},
    {"WIFI_UAV", M_CONTAINS, CAT_DRONE, CONF_HIGH, "DRONE"},
    {"UFO", M_CONTAINS, CAT_DRONE, CONF_MED, "DRONE"},
    {"FPV", M_CONTAINS, CAT_DRONE, CONF_MED, "FPV"},
    {"drone", M_CONTAINS, CAT_DRONE, CONF_MED, "DRONE"},
    // Cameras
    {"GoPro", M_CONTAINS, CAT_CAMERA, CONF_HIGH, "GOPRO"},
    {"Insta360", M_CONTAINS, CAT_CAMERA, CONF_HIGH, "INSTA360"},
    {".OSC", M_CONTAINS, CAT_CAMERA, CONF_HIGH, "360CAM"},
    {"OsmoAction", M_PREFIX, CAT_CAMERA, CONF_HIGH, "OSMO"},
    {"Osmo", M_PREFIX, CAT_CAMERA, CONF_MED, "OSMO"},
    {"AKASO", M_PREFIX, CAT_CAMERA, CONF_HIGH, "AKASO"},
    {"YI_", M_PREFIX, CAT_CAMERA, CONF_LOW, "YICAM"},
    {"Ring Setup", M_PREFIX, CAT_CAMERA, CONF_HIGH, "RING"},
    {"Wyze", M_PREFIX, CAT_CAMERA, CONF_MED, "WYZE"},
    {"Blink", M_PREFIX, CAT_CAMERA, CONF_LOW, "BLINK"},
    {"IPC", M_PREFIX, CAT_CAMERA, CONF_LOW, "IPCAM"},
    {"Dashcam", M_CONTAINS, CAT_CAMERA, CONF_MED, "DASHCAM"},
    {"70mai", M_PREFIX, CAT_CAMERA, CONF_HIGH, "DASHCAM"},
    {"VIOFO", M_PREFIX, CAT_CAMERA, CONF_HIGH, "DASHCAM"},
    // Glasses
    {"Ray-Ban", M_CONTAINS, CAT_GLASSES, CONF_HIGH, "RAYBAN"},
    // Hacking gear
    {"Pineapple", M_CONTAINS, CAT_HACKTOOL, CONF_HIGH, "PINEAPPL"},
    {"Marauder", M_CONTAINS, CAT_HACKTOOL, CONF_HIGH, "MARAUDER"},
    {"pwnagotchi", M_CONTAINS, CAT_HACKTOOL, CONF_HIGH, "PWNAGOTC"},
    {"Flipper", M_CONTAINS, CAT_HACKTOOL, CONF_MED, "FLIPPER"},
};

const NameSig kBleName[] = {
    {"GoPro", M_PREFIX, CAT_CAMERA, CONF_HIGH, "GOPRO"},
    {"Insta360", M_CONTAINS, CAT_CAMERA, CONF_HIGH, "INSTA360"},
    {"Osmo", M_PREFIX, CAT_CAMERA, CONF_MED, "OSMO"},
    {"DJI", M_PREFIX, CAT_DRONE, CONF_MED, "DJI"},
    {"Mavic", M_CONTAINS, CAT_DRONE, CONF_HIGH, "DJI"},
    {"Skydio", M_PREFIX, CAT_DRONE, CONF_HIGH, "SKYDIO"},
    {"Penguin", M_CONTAINS, CAT_SURVEIL, CONF_HIGH, "FLOCK"},
    {"FS Ext Battery", M_CONTAINS, CAT_SURVEIL, CONF_HIGH, "FLOCK"},
    {"Flock", M_CONTAINS, CAT_SURVEIL, CONF_HIGH, "FLOCK"},
    {"Pigvision", M_CONTAINS, CAT_SURVEIL, CONF_HIGH, "FLOCK"},
    {"Raven", M_PREFIX, CAT_SURVEIL, CONF_MED, "RAVEN"},
    {"DfuTarg", M_PREFIX, CAT_SURVEIL, CONF_LOW, "FLOCK"},
    {"Axon", M_PREFIX, CAT_SURVEIL, CONF_HIGH, "AXON"},
    {"Ray-Ban", M_CONTAINS, CAT_GLASSES, CONF_HIGH, "RAYBAN"},
    {"Spectacles", M_PREFIX, CAT_GLASSES, CONF_HIGH, "SPECS"},
    {"Flipper", M_PREFIX, CAT_HACKTOOL, CONF_HIGH, "FLIPPER"},
    {"Tile", M_PREFIX, CAT_TRACKER, CONF_MED, "TILE"},
    {"Chipolo", M_PREFIX, CAT_TRACKER, CONF_HIGH, "CHIPOLO"},
    {"Smart Tag", M_CONTAINS, CAT_TRACKER, CONF_HIGH, "SMARTTAG"},
    {"SmartTag", M_CONTAINS, CAT_TRACKER, CONF_HIGH, "SMARTTAG"},
    {"Pebblebee", M_PREFIX, CAT_TRACKER, CONF_HIGH, "PEBBLEBE"},
    {"iPhone", M_CONTAINS, CAT_PHONE, CONF_MED, "IPHONE"},
    {"Galaxy", M_CONTAINS, CAT_PHONE, CONF_LOW, "GALAXY"},
    {"Pixel", M_PREFIX, CAT_PHONE, CONF_LOW, "PIXEL"},
};

// ---------------- BLE company IDs ----------------
struct CompanySig {
  uint16_t id;
  const char* name;
  uint8_t cat;
  uint8_t conf;
  const char* label;
};
const CompanySig kCompany[] = {
    {0x004C, "Apple", CAT_PHONE, CONF_LOW, "APPLE"},
    {0x0006, "Microsoft", CAT_PHONE, CONF_LOW, "MSFT"},
    {0x0075, "Samsung", CAT_PHONE, CONF_LOW, "SAMSUNG"},
    {0x00E0, "Google", CAT_PHONE, CONF_LOW, "GOOGLE"},
    {0x0087, "Garmin", CAT_NONE, CONF_LOW, nullptr},
    {0x0059, "Nordic Semi", CAT_NONE, CONF_LOW, nullptr},
    {0x02E5, "Espressif", CAT_NONE, CONF_LOW, nullptr},
    {0x0171, "Amazon", CAT_NONE, CONF_LOW, nullptr},
    {0x038F, "Xiaomi", CAT_NONE, CONF_LOW, nullptr},
    {0x0157, "Huami", CAT_NONE, CONF_LOW, nullptr},
    {0x000D, "TI", CAT_NONE, CONF_LOW, nullptr},
    {0x0499, "Ruuvi", CAT_NONE, CONF_LOW, nullptr},
    {0x02F2, "GoPro", CAT_CAMERA, CONF_HIGH, "GOPRO"},
    {0x08AA, "DJI", CAT_DRONE, CONF_MED, "DJI"},
    {0x01AB, "Meta", CAT_GLASSES, CONF_MED, "META"},
    {0x058E, "Meta RL", CAT_GLASSES, CONF_MED, "META"},
    {0x03C2, "Snap", CAT_GLASSES, CONF_MED, "SNAP"},
    {0x09C8, "XUNTONG", CAT_SURVEIL, CONF_HIGH, "FLOCK"},
};

// ---------------- BLE 16-bit service UUIDs ----------------
struct UuidSig {
  uint16_t uuid;
  uint8_t cat;
  uint8_t conf;
  const char* label;
  const char* what;
};
const UuidSig kUuid[] = {
    {0xFEA6, CAT_CAMERA, CONF_HIGH, "GOPRO", "GoPro svc FEA6"},
    {0xFFFA, CAT_DRONE, CONF_HIGH, "REMOTEID", "ASTM Remote ID"},
    {0xFEED, CAT_TRACKER, CONF_HIGH, "TILE", "Tile svc FEED"},
    {0xFEEC, CAT_TRACKER, CONF_HIGH, "TILE", "Tile svc FEEC"},
    {0xFD5A, CAT_TRACKER, CONF_HIGH, "SMARTTAG", "Samsung SmartTag FD5A"},
    {0xFE33, CAT_TRACKER, CONF_HIGH, "CHIPOLO", "Chipolo svc FE33"},
    {0xFD5F, CAT_GLASSES, CONF_MED, "META", "Meta svc FD5F"},
    {0x3081, CAT_HACKTOOL, CONF_HIGH, "FLIPPER", "Flipper svc 3081"},
    {0x3082, CAT_HACKTOOL, CONF_HIGH, "FLIPPER", "Flipper svc 3082"},
    {0x3083, CAT_HACKTOOL, CONF_HIGH, "FLIPPER", "Flipper svc 3083"},
    {0x3100, CAT_SURVEIL, CONF_MED, "RAVEN", "Raven GPS svc"},
    {0x3200, CAT_SURVEIL, CONF_MED, "RAVEN", "Raven power svc"},
    {0x3300, CAT_SURVEIL, CONF_MED, "RAVEN", "Raven net svc"},
    {0x3400, CAT_SURVEIL, CONF_MED, "RAVEN", "Raven upload svc"},
    {0x3500, CAT_SURVEIL, CONF_MED, "RAVEN", "Raven error svc"},
};

bool nameMatch(const char* name, const NameSig& s) {
  return s.match == M_PREFIX ? startsCI(name, s.pat) : containsCI(name, s.pat);
}

// Penguin-NNNNNNNNNN or bare 10-digit name = Flock battery pack
bool isFlockSerialName(const char* n) {
  if (startsCI(n, "penguin-")) n += 8;
  int digits = 0;
  for (; *n; n++) {
    if (!isdigit((unsigned char)*n)) return false;
    digits++;
  }
  return digits == 10;
}

void classifyBleAdv(const Device& d, Hit& h, char* vendor, size_t vlen) {
  const uint8_t* p = d.adv;
  int len = d.advLen;
  int i = 0;
  while (i < len) {
    const uint8_t l = p[i];
    if (l == 0 || i + 1 + l > len) break;
    const uint8_t type = p[i + 1];
    const uint8_t* v = p + i + 2;
    const int vl = l - 1;
    if ((type == 0x02 || type == 0x03) && vl >= 2) {
      for (int k = 0; k + 1 < vl; k += 2) {
        const uint16_t u = v[k] | (v[k + 1] << 8);
        for (const auto& s : kUuid)
          if (s.uuid == u) consider(h, s.cat, s.conf, s.label, "%s", s.what);
      }
    } else if (type == 0x16 && vl >= 2) {  // service data
      const uint16_t u = v[0] | (v[1] << 8);
      for (const auto& s : kUuid)
        if (s.uuid == u) consider(h, s.cat, s.conf, s.label, "%s (svc data)", s.what);
      if (u == 0xFEAA && vl >= 3 && (v[2] == 0x40 || v[2] == 0x41)) {
        consider(h, CAT_TRACKER, CONF_HIGH, "GOOGFIND", "Google Find My (FMDN)");
      }
    } else if (type == 0xFF && vl >= 2) {  // manufacturer specific
      const uint16_t cid = v[0] | (v[1] << 8);
      if (cid == 0x004C && vl >= 4) {
        const uint8_t appleType = v[2];
        const uint8_t appleLen = v[3];
        if (appleType == 0x12 && appleLen == 0x19) {
          consider(h, CAT_TRACKER, CONF_HIGH, "FINDMY", "Apple Find My, separated from owner");
        } else if (appleType == 0x12) {
          consider(h, CAT_TRACKER, CONF_MED, "FINDMY", "Apple Find My (owner nearby)");
        } else if (appleType == 0x07) {
          consider(h, CAT_TRACKER, CONF_MED, "AIRTAG?", "Apple pairing/unregistered tag");
        }
      }
    }
    i += 1 + l;
  }
  (void)vendor;
  (void)vlen;
}

}  // namespace

namespace signatures {

const char* companyName(uint16_t id) {
  for (const auto& c : kCompany)
    if (c.id == id) return c.name;
  return nullptr;
}

void classify(Device& d) {
  Hit h;
  d.vendor[0] = 0;

  // Remote ID trumps everything: the drone is literally announcing itself
  if (d.rid.present) {
    consider(h, CAT_DRONE, CONF_HIGH, "REMOTEID", d.medium == MED_BLE ? "Remote ID over BLE" : "Remote ID over WiFi");
  }

  // ---- MAC based (only meaningful for globally-administered addresses) ----
  const bool publicMac = d.medium == MED_BLE ? (d.addrType == 0) : !(d.mac[0] & 0x02);
  if (publicMac) {
    for (const auto& s : kOui) {
      if (memcmp(s.oui, d.mac, 3) == 0) {
        strlcpy(d.vendor, s.vendor, sizeof(d.vendor));
        if (s.label) consider(h, s.cat, s.conf, s.label, "OUI %s", s.vendor);
      }
    }
    if (d.medium != MED_BLE) {
      for (const auto& o : kFlockOui) {
        if (memcmp(o, d.mac, 3) == 0) consider(h, CAT_SURVEIL, CONF_MED, "FLOCK?", "OUI on Flock camera list");
      }
    }
  }
  for (const auto& m : kFlockExact) {
    if (memcmp(m, d.mac, 6) == 0) consider(h, CAT_SURVEIL, CONF_HIGH, "FLOCK", "Flock factory-default MAC");
  }

  // ---- Names ----
  if (d.name[0]) {
    if (d.medium == MED_BLE) {
      for (const auto& s : kBleName)
        if (nameMatch(d.name, s)) consider(h, s.cat, s.conf, s.label, "BLE name \"%s\"", s.pat);
      if (isFlockSerialName(d.name)) consider(h, CAT_SURVEIL, CONF_MED, "FLOCK", "Flock battery serial name");
    } else {
      for (const auto& s : kSsid)
        if (nameMatch(d.name, s)) consider(h, s.cat, s.conf, s.label, "SSID matches \"%s\"", s.pat);
      // GoPro AP SSIDs look like GP + 8 digits (e.g. GP24500456)
      if (strncmp(d.name, "GP", 2) == 0 && strlen(d.name) >= 8) {
        bool digits = true;
        for (const char* c = d.name + 2; *c; c++) digits &= isdigit((unsigned char)*c) != 0;
        if (digits) consider(h, CAT_CAMERA, CONF_HIGH, "GOPRO", "GoPro SSID GPxxxxxxxx");
      }
    }
  }
  // Clients probing for camera/drone APs are usually the controller/phone
  if (d.medium == MED_WIFI_STA && d.probe[0]) {
    for (const auto& s : kSsid) {
      if (s.cat >= CAT_FLAGGED && nameMatch(d.probe, s)) {
        consider(h, s.cat, CONF_LOW, s.label, "Probing for \"%s\"", s.pat);
      }
    }
  }

  // ---- BLE payload ----
  if (d.medium == MED_BLE) {
    if (d.company != 0xFFFF) {
      for (const auto& c : kCompany) {
        if (c.id == d.company) {
          if (!d.vendor[0]) strlcpy(d.vendor, c.name, sizeof(d.vendor));
          if (c.label) consider(h, c.cat, c.conf, c.label, "BLE company %s", c.name);
        }
      }
    }
    classifyBleAdv(d, h, d.vendor, sizeof(d.vendor));
  }

  if (h.label) {
    d.category = h.cat;
    d.confidence = h.conf;
    strlcpy(d.label, h.label, sizeof(d.label));
    strlcpy(d.reason, h.reason, sizeof(d.reason));
  } else {
    d.category = d.medium == MED_WIFI_AP ? CAT_AP : d.medium == MED_WIFI_STA ? CAT_CLIENT : CAT_BLE;
    d.confidence = CONF_NONE;
    strlcpy(d.label, d.medium == MED_WIFI_AP ? "AP" : d.medium == MED_WIFI_STA ? "CLIENT" : "BLE", sizeof(d.label));
    d.reason[0] = 0;
  }
  // Generic vendor categories should not mask the medium badge
  if (d.category == CAT_NONE) {
    d.category = d.medium == MED_WIFI_AP ? CAT_AP : d.medium == MED_WIFI_STA ? CAT_CLIENT : CAT_BLE;
  }
}

}  // namespace signatures
