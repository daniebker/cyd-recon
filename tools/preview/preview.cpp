// Desktop preview harness: renders the real UI code with fake radio data and
// writes screenshots (PPM) so the layout can be checked without hardware.
// Build/run: tools/preview/run.sh
#include <Arduino.h>
#include "../../src/audio.h"
#include "../../src/battery.h"
#include "../../src/devices.h"
#include "../../src/display.h"
#include "../../src/led.h"
#include "../../src/net.h"
#include "../../src/radio.h"
#include "../../src/ui.h"

SerialShim Serial;
EspShim ESP;
static uint32_t g_ms = 0;
uint32_t millis() { return g_ms; }
void delay(uint32_t ms) { g_ms += ms; }
uint32_t esp_random() { return (uint32_t)rand() * 2654435761u; }
static uint32_t g_batMv = 1960;  // ADC pin mV (cell = 2x)
uint32_t analogReadMilliVolts(int) { return g_batMv; }

namespace net { bool st_connected(); }
// ---- stubs for hardware modules ----
static const char* g_shot = nullptr;
namespace display {
bool begin() { return true; }
void setBacklight(uint8_t) {}
void pushFrame(const uint16_t* f) {
  g_ms += 40;  // simulated frame time
  if (!g_shot) return;
  FILE* fp = fopen(g_shot, "wb");
  fprintf(fp, "P6 %d %d 255\n", SCREEN_W, SCREEN_H);
  for (int i = 0; i < SCREEN_W * SCREEN_H; i++) {
    uint16_t c = (uint16_t)((f[i] >> 8) | (f[i] << 8));  // sprite stores byte-swapped
    uint8_t rgb[3] = {(uint8_t)(((c >> 11) & 31) * 255 / 31), (uint8_t)(((c >> 5) & 63) * 255 / 63),
                      (uint8_t)((c & 31) * 255 / 31)};
    fwrite(rgb, 1, 3, fp);
  }
  fclose(fp);
  g_shot = nullptr;
}
}  // namespace display
namespace audio {
static bool en = true;
bool begin() { return true; }
void setEnabled(bool o) { en = o; }
bool enabled() { return en; }
void setVolume(uint8_t) {}
void blip() {}
void alert() {}
void setGeiger(float) {}
}  // namespace audio
namespace led {
void begin() {}
void update() {}
void flash(uint16_t) {}
void setHeat(float) {}
}  // namespace led
namespace radio {
static ChannelStats st[WIFI_MAX_CH + 1];
static uint8_t lockCh = 0;
bool begin() { return true; }
void update() {}
bool poll(Obs&) { return false; }
void lockChannel(uint8_t c) { lockCh = c; }
uint8_t channel() { return lockCh ? lockCh : 6; }
bool hopping() { return lockCh == 0 && !net::st_connected(); }
uint8_t stationChannel() { return net::st_connected() ? 6 : 0; }
void setStationChannel(uint8_t) {}
void setPromiscuous(bool) {}
void setWifiEnabled(bool) {}
void setBleEnabled(bool) {}
bool wifiEnabled() { return true; }
bool bleEnabled() { return true; }
const volatile ChannelStats* channelStats() { return st; }
static uint32_t wp = 0, bp = 0;
uint32_t wifiPackets() { return wp; }
uint32_t blePackets() { return bp; }
uint32_t dropped() { return 0; }
}  // namespace radio

namespace net {
static State st = NET_IDLE;
static Info inf;
static Host hosts[MAX_HOSTS];
static bool autoOn = true;
void begin() {}
void update() {}
static uint32_t ip(int a, int b, int c, int d) { return a | (b << 8) | (c << 16) | ((uint32_t)d << 24); }
bool connect(const char* ssid, const char*, uint8_t ch, const uint8_t* bssid, bool) {
  st = NET_CONNECTING;
  strcpy(inf.ssid, ssid);
  inf.channel = ch;
  if (bssid) memcpy(inf.bssid, bssid, 6);
  return true;
}
void finishConnect() {
  st = NET_CONNECTED;
  inf.ip = ip(192, 168, 1, 42);
  inf.mask = ip(255, 255, 255, 0);
  inf.gateway = ip(192, 168, 1, 1);
  inf.dns = ip(192, 168, 1, 1);
  inf.rssi = -58;
  inf.connectedAt = millis() - 754000;
  inf.gatewayPingMs = 4;
  const uint8_t me[6] = {0xDC, 0xDA, 0x0C, 0x21, 0x9E, 0x40};
  memcpy(inf.mac, me, 6);
  struct { int last; const char* mac; const char* name; const char* svc; } hs[] = {
    {1, "A4:2B:B0:11:22:30", "", "http"},
    {42, "DC:DA:0C:21:9E:40", "cyd-recon", ""},
    {10, "00:11:32:AA:01:02", "nas", "http smb ssh"},
    {23, "F0:18:98:44:12:9C", "Daniels-MacBook", "apple ssh"},
    {31, "3C:5A:B4:9A:77:10", "Living-Room-TV", "cast"},
    {37, "04:41:69:5E:12:9B", "", ""},
    {54, "D8:31:34:00:FE:11", "", "airplay"},
    {77, "6E:21:9A:0D:44:3B", "", ""},
  };
  int n = 0;
  for (auto& x : hs) {
    Host& h = hosts[n++];
    h.used = true;
    h.ip = ip(192, 168, 1, x.last);
    unsigned v[6];
    sscanf(x.mac, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]);
    for (int i = 0; i < 6; i++) h.mac[i] = v[i];
    strcpy(h.name, x.name);
    strcpy(h.services, x.svc);
    h.firstSeen = millis() - 600000;
    h.lastSeen = millis() - (n * 3000);
  }
  hosts[2].portScan = 2;
  uint16_t ports[] = {22, 80, 443, 445, 5000};
  memcpy(hosts[2].ports, ports, sizeof(ports));
  hosts[2].nPorts = 5;
}
void disconnect() { st = NET_IDLE; }
bool st_connected() { return st == NET_CONNECTED; }
State state() { return st; }
const char* statusText() { return st == NET_CONNECTING ? "connecting to NETGEAR-5G-Home" : "offline"; }
const Info& info() { return inf; }
bool savedPassword(const char* ssid, char* p, size_t n) {
  if (strcmp(ssid, "TP-Link_8A2C") == 0) { if (n) p[0] = 0; return true; }
  return false;
}
void forget(const char*) {}
bool autoConnect() { return autoOn; }
void setAutoConnect(bool on) { autoOn = on; }
int hostCount() { return MAX_HOSTS; }
const Host& host(int i) { return hosts[i]; }
void rescan() {}
int sweepProgress() { return 100; }
void probePorts(int) {}
const char* portName(uint16_t p) { return p == 22 ? "ssh" : p == 80 ? "http" : p == 443 ? "https" : p == 445 ? "smb" : "upnp"; }
void ipToStr(uint32_t ip, char* out) {
  snprintf(out, 16, "%u.%u.%u.%u", ip & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF, ip >> 24);
}
}  // namespace net

// ---- fake world ----
static void mac(Obs& o, const char* s) {
  unsigned v[6];
  sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]);
  for (int i = 0; i < 6; i++) o.mac[i] = v[i];
}
static void adv(Obs& o, std::initializer_list<uint8_t> b) {
  o.advLen = 0;
  for (uint8_t x : b) o.adv[o.advLen++] = x;
}
static Obs wifi(const char* m, const char* ssid, int rssi, int ch, int sec = 3) {
  Obs o{};
  o.medium = MED_WIFI_AP;
  mac(o, m);
  strcpy(o.name, ssid);
  o.rssi = rssi;
  o.channel = ch;
  o.security = sec;
  return o;
}
static Obs ble(const char* m, const char* name, int rssi, int at = 1) {
  Obs o{};
  o.medium = MED_BLE;
  mac(o, m);
  strcpy(o.name, name);
  o.rssi = rssi;
  o.addrType = at;
  return o;
}

static int s_drone = -1, s_tag = -1;
static void populate(int t) {
  auto in = [&](Obs o) { return devices::ingest(o); };
  in(wifi("A4:2B:B0:11:22:33", "NETGEAR-5G-Home", -58 + (t % 3), 6));
  in(wifi("B0:BE:76:44:55:66", "TP-Link_8A2C", -71, 1));
  in(wifi("F0:9F:C2:01:02:03", "CORP-GUEST", -66, 11, 0));
  in(wifi("12:34:56:AA:BB:CC", "", -80, 3));
  in(wifi("04:41:69:5E:12:9A", "GP24500456", -49, 1));
  {
    Obs o = wifi("60:60:1F:A1:B2:C3", "DJI-MINI4-7F21", -62 + (t / 4) % 8, 6);
    o.rid.present = true;
    strcpy(o.rid.serial, "1581F5FJD239C00A7Q21");
    o.rid.uaType = 2;
    o.rid.lat = 515074123;
    o.rid.lon = -1278123;
    o.rid.altM = 84;
    o.rid.speedCms = 420;
    o.rid.opLat = 515069001;
    o.rid.opLon = -1281150;
    s_drone = in(o);
  }
  in(wifi("B4:1E:52:07:33:10", "Flock-3C9A12", -77, 11));
  {
    Obs o{};
    o.medium = MED_WIFI_STA;
    mac(o, "DA:A1:19:0F:00:31");
    strcpy(o.name, "TELLO-5A1B2C");
    o.flags = OBS_PROBE;
    o.rssi = -69;
    o.channel = 6;
    in(o);
  }
  {
    Obs o = ble("C2:44:19:8E:7A:01", "", -55 + (t % 5) - (t > 20 ? 0 : 8));
    adv(o, {0x1E, 0xFF, 0x4C, 0x00, 0x12, 0x19, 0x10, 0xA3, 0x9C, 0x55, 0x01, 0x20, 0x77, 0x31, 0x90, 0x4F,
             0x6A, 0x2E, 0x11, 0x08, 0x47, 0x52, 0x0C, 0x8D, 0x9A, 0xEE, 0x02, 0x33, 0x01, 0x00, 0x00});
    s_tag = in(o);
  }
  {
    Obs o = ble("E7:01:22:9B:3C:44", "", -74);
    adv(o, {0x02, 0x01, 0x06, 0x03, 0x03, 0xED, 0xFE});
    in(o);
  }
  {
    Obs o = ble("80:E1:26:4A:BB:01", "Flipper Rafiki", -63, 0);
    adv(o, {0x02, 0x01, 0x06, 0x03, 0x03, 0x82, 0x30});
    in(o);
  }
  {
    Obs o = ble("D9:3A:70:12:00:8E", "", -68);
    adv(o, {0x02, 0x01, 0x06, 0x05, 0xFF, 0xAB, 0x01, 0x02, 0x03});
    in(o);
  }
  {
    Obs o = ble("F4:DD:9E:31:00:12", "GoPro 4521", -52, 0);
    adv(o, {0x02, 0x01, 0x06, 0x03, 0x02, 0xA6, 0xFE});
    in(o);
  }
  {
    Obs o = ble("CC:12:9E:31:00:12", "Penguin-4021553001", -81);
    in(o);
  }
  {
    Obs o = ble("6A:2F:11:90:3C:D0", "", -84);
    adv(o, {0x02, 0x01, 0x1A, 0x0A, 0xFF, 0x4C, 0x00, 0x10, 0x05, 0x31, 0x1C, 0x4F, 0x99, 0x10});
    in(o);
  }
  in(ble("5B:3E:A0:77:12:10", "Mi Smart Band 8", -77));
  in(ble("4F:88:01:FE:21:77", "LE_WH-1000XM5", -64));
}

static void tick(int frames, touch::State t = {}) {
  for (int i = 0; i < frames; i++) {
    battery::update();
    radio::wp += 9;
    radio::bp += 3;
    for (int c = 1; c <= 13; c++) {
      radio::st[c].packets += (c == 1 || c == 6 || c == 11) ? 6 + rand() % 9 : rand() % 3;
      radio::st[c].maxRssi = -40 - c * 3;
    }
    ui::frame(t, false);
  }
}
static void tap(int x, int y) {
  touch::State t;
  t.down = true;
  t.x = x;
  t.y = y;
  tick(1, t);
  tick(1);
}
static void shot(const char* path) {
  g_shot = path;
  tick(1);
}

int main(int argc, char** argv) {
  const char* out = argc > 1 ? argv[1] : ".";
  char p[256];
  ui::begin();
  devices::begin();
  ui::bootLine("> qspi display st77922 320x480", 1);
  ui::bootLine("> psram target table", 1);
  ui::bootLine("> touch controller @0x55", 1);
  ui::bootLine("> es8311 audio codec", 1);
  ui::bootLine("> wifi promiscuous 2.4ghz + ble observer", 1);
  ui::bootLine("> signature db: drones cams flock tags", 1);
  snprintf(p, sizeof p, "%s/01_boot.ppm", out);
  g_shot = p;
  ui::bootLine("> jacking in...", 0);
  ui::bootDone();

  for (int t = 0; t < 30; t++) {
    populate(t);
    tick(5);
  }
  // A toast is shown for the newest alert; let it expire first for the list shot
  g_ms += 6000;
  populate(31);
  while (devices::popAlert() >= 0) {}
  snprintf(p, sizeof p, "%s/02_list.ppm", out);
  shot(p);

  tap(39 + 4, 43);  // ALERTS tab
  snprintf(p, sizeof p, "%s/03_alerts.ppm", out);
  shot(p);

  // Trigger a toast with a new drone
  {
    Obs o = wifi("34:D2:62:00:AA:01", "Mavic-Air2-9F", -71, 11);
    devices::ingest(o);
  }
  tick(2);
  snprintf(p, sizeof p, "%s/04_toast.ppm", out);
  shot(p);
  tap(240, 150);  // tap toast -> detail
  for (int t = 0; t < 120; t++) {
    Obs o = wifi("34:D2:62:00:AA:01", "Mavic-Air2-9F", -78 + t / 6 + (rand() % 5), 11);
    devices::ingest(o);
    tick(3);
  }
  snprintf(p, sizeof p, "%s/05_track_wifi.ppm", out);
  shot(p);
  tap(30, 14);  // back

  // Track the Remote ID drone
  tap(0, 0);
  for (int i = 0; i < 4; i++) tap(4 + i * 78 + 20, 43);  // cycle tabs, end on BLE
  tap(4 + 20, 43);                                       // ALL
  // open detail via direct hit on first alerts row
  tap(4 + 78 + 20, 43);  // ALERTS
  tap(200, 58 + 5);      // first row
  for (int t = 0; t < 140; t++) {
    populate(40 + t);
    tick(2);
  }
  tap(250, 305);  // set mark
  for (int t = 0; t < 30; t++) {
    populate(200 + t);
    tick(2);
  }
  snprintf(p, sizeof p, "%s/06_track_rid.ppm", out);
  shot(p);
  tap(30, 14);

  tap(440, 43);  // RF scope
  tick(80);
  snprintf(p, sizeof p, "%s/07_spectrum.ppm", out);
  shot(p);
  tap(30, 14);

  // Battery draining into the low warning
  for (int mv = 1960; mv >= 1785; mv -= 5) {
    g_batMv = mv;
    tick(25);
  }
  snprintf(p, sizeof p, "%s/08_battery_low.ppm", out);
  shot(p);
  // Plug in: step up by ~60 mV at the cell, then a slow climb
  g_batMv += 30;
  for (int i = 0; i < 20; i++) {
    g_batMv += 1;
    tick(25);
  }
  snprintf(p, sizeof p, "%s/09_battery_charging.ppm", out);
  shot(p);

  // ---- network ----
  g_batMv = 1990;
  for (int t = 0; t < 10; t++) { populate(300 + t); tick(3); }
  tap(440, 43);  // NETWORK
  snprintf(p, sizeof p, "%s/10_net_pick.ppm", out);
  shot(p);
  tap(200, 52 + 5);  // first AP (saved one sorts first) -> connects
  snprintf(p, sizeof p, "%s/11_net_connecting.ppm", out);
  shot(p);
  net::disconnect();
  tick(1);
  tap(200, 52 + 30 + 5);  // second AP: WPA2, not saved -> keyboard
  for (const char* k = "hunter2!"; *k; k++) {
    // find key position on the keyboard
    static const char* rows[] = {"1234567890", "qwertyuiop", "asdfghjkl-", "zxcvbnm._@"};
    bool hit = false;
    for (int r = 0; r < 4 && !hit; r++)
      for (int c = 0; c < 10; c++)
        if (rows[r][c] == *k) { tap(5 + c * 47 + 20, 74 + r * 44 + 20); hit = true; break; }
    if (!hit) { tap(78 + 30, 270); tap(5 + 0 * 47 + 20, 74 + 20); }  // '!' via #+= layer
  }
  snprintf(p, sizeof p, "%s/12_keyboard.ppm", out);
  shot(p);
  tap(121 + 50, 305);  // cancel
  net::finishConnect();
  {  // a wireless client heard over the air talking to our AP
    Obs o{};
    o.medium = MED_WIFI_STA;
    mac(o, "04:41:69:5E:12:9B");
    o.rssi = -61;
    o.channel = 6;
    memcpy(o.bssid, net::inf.bssid, 6);
    devices::ingest(o);
    Obs o2 = o;
    mac(o2, "9A:11:3C:00:72:EE");
    o2.rssi = -77;
    devices::ingest(o2);
  }
  tick(3);
  g_ms += 6000;  // let the GoPro alert toast time out
  tick(1);
  snprintf(p, sizeof p, "%s/13_net_linked.ppm", out);
  shot(p);
  tap(200, 136 + 28 * 2 + 5);  // a host row
  snprintf(p, sizeof p, "%s/14_host.ppm", out);
  shot(p);
  return 0;
}
