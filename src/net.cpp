#include "net.h"
#include <ESPmDNS.h>
#include <Preferences.h>
#include <WiFi.h>
#include "config.h"
#include "devices.h"
#include "lwip/etharp.h"
#include "lwip/netif.h"
#include "lwip/sockets.h"
#include "lwip/tcpip.h"
#include "ping/ping_sock.h"
#include "radio.h"

namespace {

// ---------------- saved networks ----------------
constexpr int MAX_SAVED = 6;
struct Saved {
  char ssid[33];
  char pass[65];
};
Saved s_saved[MAX_SAVED];
int s_nSaved = 0;
bool s_auto = true;
Preferences s_prefs;

void loadSaved() {
  s_prefs.begin("netcfg", true);
  s_nSaved = min<int>(s_prefs.getUChar("cnt", 0), MAX_SAVED);
  s_auto = s_prefs.getBool("auto", true);
  for (int i = 0; i < s_nSaved; i++) {
    char k[4];
    snprintf(k, sizeof(k), "s%d", i);
    s_prefs.getString(k, s_saved[i].ssid, sizeof(s_saved[i].ssid));
    snprintf(k, sizeof(k), "p%d", i);
    s_prefs.getString(k, s_saved[i].pass, sizeof(s_saved[i].pass));
  }
  s_prefs.end();
}

void storeSaved() {
  s_prefs.begin("netcfg", false);
  s_prefs.putUChar("cnt", s_nSaved);
  s_prefs.putBool("auto", s_auto);
  for (int i = 0; i < s_nSaved; i++) {
    char k[4];
    snprintf(k, sizeof(k), "s%d", i);
    s_prefs.putString(k, s_saved[i].ssid);
    snprintf(k, sizeof(k), "p%d", i);
    s_prefs.putString(k, s_saved[i].pass);
  }
  s_prefs.end();
}

int findSaved(const char* ssid) {
  for (int i = 0; i < s_nSaved; i++)
    if (strcmp(s_saved[i].ssid, ssid) == 0) return i;
  return -1;
}

void remember(const char* ssid, const char* pass) {
  int i = findSaved(ssid);
  if (i < 0) {
    if (s_nSaved == MAX_SAVED) {  // drop the oldest
      memmove(&s_saved[0], &s_saved[1], sizeof(Saved) * (MAX_SAVED - 1));
      s_nSaved--;
    }
    i = s_nSaved++;
  }
  strlcpy(s_saved[i].ssid, ssid, sizeof(s_saved[i].ssid));
  strlcpy(s_saved[i].pass, pass, sizeof(s_saved[i].pass));
  storeSaved();
}

// ---------------- connection state ----------------
net::State s_state = net::NET_IDLE;
net::Info s_info;
char s_status[48] = "offline";
char s_pendPass[65];
bool s_pendSave = false;
uint32_t s_attemptStart = 0;
uint32_t s_lostAt = 0;
volatile int s_discReason = 0;
volatile uint32_t s_epoch = 0;  // bumps on every new connection
bool s_userDisconnected = false;
uint32_t s_lastAuto = 0;
bool s_mdnsUp = false;

void onWifiEvent(arduino_event_id_t ev, arduino_event_info_t info) {
  if (ev == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) s_discReason = info.wifi_sta_disconnected.reason;
}

const char* reasonText(int r) {
  switch (r) {
    case 2:    // AUTH_EXPIRE
    case 203:  // ASSOC_FAIL
    case 200:  // BEACON_TIMEOUT
      return "AP not responding";
    case 15:   // 4WAY_HANDSHAKE_TIMEOUT
    case 202:  // AUTH_FAIL
    case 204:  // HANDSHAKE_TIMEOUT
    case 14:   // MIC_FAILURE
      return "wrong password?";
    case 201:  // NO_AP_FOUND
    case 210:
    case 211:
    case 212:
      return "network not found";
    default:
      return nullptr;
  }
}

void releaseRadio() {
  radio::setStationChannel(0);
  radio::setPromiscuous(true);
}

// ---------------- hosts ----------------
net::Host s_hosts[net::MAX_HOSTS];
volatile int s_sweepPct = 100;
volatile bool s_rescanReq = false;
volatile int s_portReq = -1;

int findHost(uint32_t ip) {
  for (int i = 0; i < net::MAX_HOSTS; i++)
    if (s_hosts[i].used && s_hosts[i].ip == ip) return i;
  return -1;
}

void addHost(uint32_t ip, const uint8_t* mac) {
  const uint32_t now = millis();
  int i = findHost(ip);
  if (i >= 0) {
    net::Host& h = s_hosts[i];
    if (memcmp(h.mac, mac, 6) != 0) {  // IP now belongs to another device
      h.name[0] = 0;
      h.nameSrc = net::NAME_NONE;
      h.nameTries = 0;
      h.services[0] = 0;
    }
    memcpy(s_hosts[i].mac, mac, 6);
    s_hosts[i].lastSeen = now;
    return;
  }
  int slot = -1;
  uint32_t oldest = UINT32_MAX;
  for (int k = 0; k < net::MAX_HOSTS; k++) {
    if (!s_hosts[k].used) {
      slot = k;
      break;
    }
    if (s_hosts[k].lastSeen < oldest) {
      oldest = s_hosts[k].lastSeen;
      slot = k;
    }
  }
  net::Host& h = s_hosts[slot];
  h.used = false;
  memset(&h, 0, sizeof(h));
  h.ip = ip;
  memcpy(h.mac, mac, 6);
  h.firstSeen = h.lastSeen = now;
  h.used = true;  // publish last
}

// lwIP is built with core locking, so we can talk to the ARP module directly
void arpRequest(uint32_t hostOrderIp) {
  LOCK_TCPIP_CORE();
  if (netif_default) {
    ip4_addr_t a;
    a.addr = htonl(hostOrderIp);
    etharp_request(netif_default, &a);
  }
  UNLOCK_TCPIP_CORE();
}

void harvestArp() {
  struct Found {
    uint32_t ip;
    uint8_t mac[6];
  } found[ARP_TABLE_SIZE];
  int n = 0;
  LOCK_TCPIP_CORE();
  for (size_t i = 0; i < ARP_TABLE_SIZE; i++) {
    ip4_addr_t* ip;
    struct netif* nif;
    struct eth_addr* eth;
    if (etharp_get_entry(i, &ip, &nif, &eth)) {
      found[n].ip = ip->addr;
      memcpy(found[n].mac, eth->addr, 6);
      n++;
    }
  }
  UNLOCK_TCPIP_CORE();
  for (int i = 0; i < n; i++) addHost(found[i].ip, found[i].mac);
}

// ---------------- gateway ping ----------------
volatile bool s_pingBusy = false;
uint32_t s_pingSum = 0, s_pingOk = 0;

void pingSuccess(esp_ping_handle_t h, void*) {
  uint32_t t;
  esp_ping_get_profile(h, ESP_PING_PROF_TIMEGAP, &t, sizeof(t));
  s_pingSum += t;
  s_pingOk++;
}
void pingTimeout(esp_ping_handle_t, void*) {}
void pingEnd(esp_ping_handle_t h, void*) {
  s_info.gatewayPingMs = s_pingOk ? (int)(s_pingSum / s_pingOk) : -2;
  esp_ping_delete_session(h);
  s_pingBusy = false;
}

void startPing() {
  if (s_pingBusy || !s_info.gateway) return;
  esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
  const uint32_t g = s_info.gateway;
  IP_ADDR4(&cfg.target_addr, g & 0xFF, (g >> 8) & 0xFF, (g >> 16) & 0xFF, g >> 24);
  cfg.count = 3;
  cfg.interval_ms = 300;
  cfg.timeout_ms = 1000;
  cfg.task_stack_size = 3072;
  esp_ping_callbacks_t cbs = {};
  cbs.on_ping_success = pingSuccess;
  cbs.on_ping_timeout = pingTimeout;
  cbs.on_ping_end = pingEnd;
  esp_ping_handle_t h;
  s_pingSum = s_pingOk = 0;
  if (esp_ping_new_session(&cfg, &cbs, &h) == ESP_OK) {
    s_pingBusy = true;
    esp_ping_start(h);
  }
}

// ---------------- port probe ----------------
struct PortDef {
  uint16_t port;
  const char* name;
};
const PortDef kPorts[] = {
    {21, "ftp"},     {22, "ssh"},       {23, "telnet"},  {53, "dns"},    {80, "http"},
    {443, "https"},  {445, "smb"},      {554, "rtsp"},   {1883, "mqtt"}, {3389, "rdp"},
    {5000, "upnp"},  {8008, "cast"},    {8009, "cast"},  {8080, "http"}, {8443, "https"},
    {9100, "print"}, {62078, "iphone"},
};

void doPortScan(int idx) {
  net::Host& h = s_hosts[idx];
  if (!h.used) return;
  h.portScan = 1;
  h.nPorts = 0;
  const uint32_t epoch = s_epoch;
  for (const auto& p : kPorts) {
    if (s_state != net::NET_CONNECTED || epoch != s_epoch) return;
    NetworkClient c;
    if (c.connect(IPAddress(h.ip), p.port, 350)) {
      if (h.nPorts < net::MAX_PORTS) h.ports[h.nPorts++] = p.port;
      c.stop();
    }
  }
  h.portScan = 2;
}

// ---------------- mDNS ----------------
const char* kServices[][2] = {
    {"http", "http"},       {"workstation", "pc"}, {"googlecast", "cast"}, {"airplay", "airplay"},
    {"raop", "speaker"},    {"ipp", "printer"},    {"printer", "printer"}, {"ssh", "ssh"},
    {"smb", "smb"},         {"hap", "homekit"},    {"spotify-connect", "spotify"},
    {"companion-link", "apple"}, {"rtsp", "rtsp"}, {"esphomelib", "esphome"},
};

void mdnsQuery(int which) {
  const int n = MDNS.queryService(kServices[which][0], "tcp");
  for (int i = 0; i < n; i++) {
    const uint32_t ip = (uint32_t)MDNS.address(i);
    const int hi = findHost(ip);
    if (hi < 0) continue;
    net::Host& h = s_hosts[hi];
    if (!h.name[0]) {
      strlcpy(h.name, MDNS.hostname(i).c_str(), sizeof(h.name));
      if (h.name[0]) h.nameSrc = net::NAME_MDNS;
    }
    const char* tag = kServices[which][1];
    if (!strstr(h.services, tag) && strlen(h.services) + strlen(tag) + 2 < sizeof(h.services)) {
      if (h.services[0]) strlcat(h.services, " ", sizeof(h.services));
      strlcat(h.services, tag, sizeof(h.services));
    }
  }
}

// ---------------- per-host name lookup ----------------
// Most LAN devices don't advertise any of the mDNS services we browse for, so
// ask each host for its name directly, cheapest/most specific first:
//   1. unicast mDNS reverse lookup sent straight to the host (Apple, Linux
//      avahi, ESPHome, printers, Chromecasts...)
//   2. reverse DNS via the network's resolver/router, which usually knows the
//      DHCP hostname (dnsmasq, OpenWrt, Fritz!Box, Pi-hole, most ISP boxes)
//   3. NetBIOS node status (Windows, Samba / NAS boxes)

uint16_t s_qid = 0x4c00;

// Send one UDP datagram to ip:port (network byte order ip) and wait for a reply
// from that address carrying our DNS/NBNS transaction id. Returns reply length.
int udpExchange(uint32_t ip, uint16_t port, const uint8_t* q, size_t qlen, uint8_t* resp, size_t cap, int timeoutMs) {
  const int sock = lwip_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock < 0) return -1;
  sockaddr_in to = {};
  to.sin_family = AF_INET;
  to.sin_port = htons(port);
  to.sin_addr.s_addr = ip;
  int got = -1;
  if (lwip_sendto(sock, q, qlen, 0, (sockaddr*)&to, sizeof(to)) == (int)qlen) {
    const uint32_t deadline = millis() + timeoutMs;
    for (;;) {
      const int32_t left = (int32_t)(deadline - millis());
      if (left <= 0) break;
      timeval tv = {left / 1000, (left % 1000) * 1000};
      lwip_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
      sockaddr_in from = {};
      socklen_t fl = sizeof(from);
      const int n = lwip_recvfrom(sock, resp, cap, 0, (sockaddr*)&from, &fl);
      if (n < 0) break;  // timeout
      if (n >= 12 && from.sin_addr.s_addr == ip && resp[0] == q[0] && resp[1] == q[1]) {
        got = n;
        break;
      }
    }
  }
  lwip_close(sock);
  return got;
}

// Read a (possibly compressed) DNS name at off, advancing off past it.
bool dnsName(const uint8_t* m, int len, int& off, char* out, size_t cap) {
  size_t o = 0;
  int p = off, jumps = 0;
  bool jumped = false;
  if (cap) out[0] = 0;
  while (p < len) {
    const uint8_t l = m[p];
    if (l == 0) {
      if (!jumped) off = p + 1;
      return true;
    }
    if ((l & 0xC0) == 0xC0) {
      if (p + 1 >= len || ++jumps > 16) return false;
      if (!jumped) off = p + 2;
      jumped = true;
      p = ((l & 0x3F) << 8) | m[p + 1];
      continue;
    }
    if (l > 63 || p + 1 + l > len) return false;
    for (int i = 0; i < l; i++) {
      if (o + 2 >= cap) break;
      const char c = (char)m[p + 1 + i];
      out[o++] = (c >= 0x20 && c < 0x7F) ? c : '?';
    }
    if (o + 2 < cap) out[o++] = '.';
    out[o < cap ? o : cap - 1] = 0;
    p += 1 + l;
  }
  return false;
}

// Build a PTR query for d.c.b.a.in-addr.arpa
int buildPtrQuery(uint32_t ip, uint16_t id, uint8_t* q) {
  int n = 0;
  q[n++] = id >> 8;
  q[n++] = id & 0xFF;
  q[n++] = 0x01;  // RD (ignored by mDNS responders)
  q[n++] = 0x00;
  q[n++] = 0; q[n++] = 1;  // QDCOUNT
  for (int i = 0; i < 6; i++) q[n++] = 0;
  for (int i = 3; i >= 0; i--) {
    char lab[4];
    const int l = snprintf(lab, sizeof(lab), "%u", (unsigned)((ip >> (8 * i)) & 0xFF));
    q[n++] = l;
    memcpy(q + n, lab, l);
    n += l;
  }
  q[n++] = 7; memcpy(q + n, "in-addr", 7); n += 7;
  q[n++] = 4; memcpy(q + n, "arpa", 4); n += 4;
  q[n++] = 0;
  q[n++] = 0; q[n++] = 12;  // PTR
  q[n++] = 0; q[n++] = 1;   // IN
  return n;
}

// First PTR target in a DNS/mDNS response
bool parsePtr(const uint8_t* m, int len, char* out, size_t cap) {
  if (len < 12 || !(m[2] & 0x80) || (m[3] & 0x0F) != 0) return false;  // not a response / error rcode
  const int qd = (m[4] << 8) | m[5], an = (m[6] << 8) | m[7];
  int off = 12;
  char tmp[96];
  for (int i = 0; i < qd; i++) {
    if (!dnsName(m, len, off, tmp, sizeof(tmp))) return false;
    off += 4;
  }
  for (int i = 0; i < an && off < len; i++) {
    if (!dnsName(m, len, off, tmp, sizeof(tmp)) || off + 10 > len) return false;
    const uint16_t type = (m[off] << 8) | m[off + 1];
    const uint16_t rdlen = (m[off + 8] << 8) | m[off + 9];
    off += 10;
    if (off + rdlen > len) return false;
    if (type == 12) {
      int r = off;
      return dnsName(m, len, r, out, cap) && out[0];
    }
    off += rdlen;
  }
  return false;
}

// Reduce "Daniels-iPhone.lan." to "Daniels-iPhone"; reject names that are just
// the address spelled out (e.g. "192-168-1-23.isp.example").
bool cleanName(char* n, uint32_t ip) {
  char* dot = strchr(n, '.');
  if (dot) *dot = 0;
  if (!n[0] || strcasecmp(n, "localhost") == 0) return false;
  bool letter = false;
  for (const char* c = n; *c; c++)
    if (isalpha((unsigned char)*c)) letter = true;
  if (!letter) return false;
  const uint8_t* b = (const uint8_t*)&ip;
  char a1[20], a2[20];
  snprintf(a1, sizeof(a1), "%u-%u-%u-%u", b[0], b[1], b[2], b[3]);
  snprintf(a2, sizeof(a2), "%u-%u-%u-%u", b[3], b[2], b[1], b[0]);
  return !strstr(n, a1) && !strstr(n, a2);
}

bool ptrLookup(uint32_t server, uint16_t port, uint32_t ip, char* out, size_t cap, int timeoutMs) {
  static uint8_t q[64], r[512];
  const int qn = buildPtrQuery(ip, ++s_qid, q);
  const int n = udpExchange(server, port, q, qn, r, sizeof(r), timeoutMs);
  return n > 0 && parsePtr(r, n, out, cap) && cleanName(out, ip);
}

bool netbiosLookup(uint32_t ip, char* out, size_t cap) {
  static uint8_t q[50], r[512];
  int n = 0;
  const uint16_t id = ++s_qid;
  q[n++] = id >> 8;
  q[n++] = id & 0xFF;
  q[n++] = 0; q[n++] = 0;  // flags
  q[n++] = 0; q[n++] = 1;  // QDCOUNT
  for (int i = 0; i < 6; i++) q[n++] = 0;
  q[n++] = 0x20;  // "*" padded with NULs, first-level encoded
  q[n++] = 'C';
  q[n++] = 'K';
  for (int i = 0; i < 30; i++) q[n++] = 'A';
  q[n++] = 0;
  q[n++] = 0; q[n++] = 0x21;  // NBSTAT
  q[n++] = 0; q[n++] = 0x01;  // IN
  const int len = udpExchange(ip, 137, q, n, r, sizeof(r), 400);
  if (len < 12 || !(r[2] & 0x80) || ((r[6] << 8) | r[7]) == 0) return false;
  int off = 12;
  char tmp[64];
  if (!dnsName(r, len, off, tmp, sizeof(tmp)) || off + 11 > len) return false;
  off += 10;  // type, class, ttl, rdlength
  const int num = r[off++];
  int pick = -1;
  for (int i = 0; i < num && off + (i + 1) * 18 <= len; i++) {
    const uint8_t* e = r + off + i * 18;
    const bool group = e[16] & 0x80;
    if (group) continue;
    if (e[15] == 0x00) { pick = i; break; }      // workstation name
    if (e[15] == 0x20 && pick < 0) pick = i;     // file server name
  }
  if (pick < 0) return false;
  const uint8_t* e = r + off + pick * 18;
  int l = 15;
  while (l > 0 && (e[l - 1] == ' ' || e[l - 1] == 0)) l--;
  size_t o = 0;
  for (int i = 0; i < l && o + 1 < cap; i++) out[o++] = (e[i] >= 0x20 && e[i] < 0x7F) ? e[i] : '?';
  out[o] = 0;
  return o > 0;
}

// Resolve one unnamed host. Returns true if a lookup was attempted.
bool resolveNextName() {
  const uint32_t now = millis();
  int best = -1;
  for (int i = 0; i < net::MAX_HOSTS; i++) {
    const net::Host& h = s_hosts[i];
    if (!h.used || h.nameSrc != net::NAME_NONE || h.ip == s_info.ip || h.nameTries >= 4) continue;
    // back off: retry after 1, 2, 3 minutes (names appear as DHCP leases renew, devices wake...)
    if (h.nameTries && now - h.nameAt < 60000u * h.nameTries) continue;
    if (best < 0 || h.nameTries < s_hosts[best].nameTries) best = i;
  }
  if (best < 0) return false;
  net::Host& h = s_hosts[best];
  const uint32_t ip = h.ip, epoch = s_epoch;
  h.nameAt = now;
  char name[64];
  uint8_t src = net::NAME_NONE;
  if (ptrLookup(ip, 5353, ip, name, sizeof(name), 400)) {
    src = net::NAME_MDNS;
  } else {
    const uint32_t dns = s_info.dns ? s_info.dns : s_info.gateway;
    if (dns && ptrLookup(dns, 53, ip, name, sizeof(name), 700)) src = net::NAME_DNS;
    else if (s_info.gateway && s_info.gateway != dns && ptrLookup(s_info.gateway, 53, ip, name, sizeof(name), 700))
      src = net::NAME_DNS;
    else if (netbiosLookup(ip, name, sizeof(name))) src = net::NAME_NETBIOS;
  }
  if (epoch != s_epoch || !h.used || h.ip != ip) return true;  // network or slot changed under us
  if (src != net::NAME_NONE && !h.name[0]) {
    strlcpy(h.name, name, sizeof(h.name));
    h.nameSrc = src;
    Serial.printf("[net] name %u.%u.%u.%u = %s (%s)\n", ip & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF, ip >> 24,
                  h.name, net::nameSrcText(src));
  } else if (src == net::NAME_NONE) {
    h.nameTries++;
  }
  return true;
}

// ---------------- discovery task ----------------
void discoveryTask(void*) {
  uint32_t myEpoch = 0;
  uint32_t lastSweep = 0, lastPing = 0, lastMdns = 0;
  int mdnsNext = 0;
  uint32_t sweepIp = 0, sweepEnd = 0;  // host byte order; sweepIp==0 -> idle

  for (;;) {
    if (s_state != net::NET_CONNECTED) {
      sweepIp = 0;
      s_sweepPct = 100;
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }
    if (myEpoch != s_epoch) {  // fresh connection: forget old LAN
      myEpoch = s_epoch;
      for (auto& h : s_hosts) h.used = false;
      lastSweep = 0;
      lastPing = 0;
      lastMdns = millis();
      addHost(s_info.ip, s_info.mac);
      const int self = findHost(s_info.ip);
      if (self >= 0) {
        strlcpy(s_hosts[self].name, "cyd-recon", sizeof(s_hosts[self].name));
        s_hosts[self].nameSrc = net::NAME_SELF;
      }
    }
    const uint32_t now = millis();

    if (s_portReq >= 0) {
      const int r = s_portReq;
      s_portReq = -1;
      doPortScan(r);
      continue;
    }

    if (!sweepIp && (s_rescanReq || !lastSweep || now - lastSweep > 90000)) {
      s_rescanReq = false;
      lastSweep = now;
      const uint32_t me = ntohl(s_info.ip), mask = ntohl(s_info.mask);
      uint32_t base = me & mask, last = base | ~mask;
      if (~mask > 0xFF) {  // big subnet: just sweep our own /24
        base = me & 0xFFFFFF00;
        last = base | 0xFF;
      }
      sweepIp = base + 1;
      sweepEnd = last;  // exclusive (broadcast)
    }

    if (sweepIp) {
      // ARP table only holds ARP_TABLE_SIZE entries: ask in small batches
      // and harvest the replies before they get evicted.
      constexpr int BATCH = 6;
      for (int i = 0; i < BATCH && sweepIp < sweepEnd; i++, sweepIp++) arpRequest(sweepIp);
      vTaskDelay(pdMS_TO_TICKS(220));
      harvestArp();
      const uint32_t base = sweepEnd & 0xFFFFFF00;
      s_sweepPct = sweepIp >= sweepEnd ? 100 : (int)((sweepIp - base) * 100 / max<uint32_t>(1, sweepEnd - base));
      if (sweepIp >= sweepEnd) sweepIp = 0;
      continue;
    }

    harvestArp();  // pick up anyone who talked to us
    if (now - lastPing > 15000) {
      lastPing = now;
      startPing();
    }
    resolveNextName();  // one host per pass, ~0.4-2 s
    if (s_mdnsUp && now - lastMdns > 1500) {
      lastMdns = now;
      mdnsQuery(mdnsNext);  // blocks ~3 s
      mdnsNext = (mdnsNext + 1) % (sizeof(kServices) / sizeof(kServices[0]));
    }
    // Drop hosts silent for 10 minutes
    for (auto& h : s_hosts)
      if (h.used && h.ip != s_info.ip && millis() - h.lastSeen > 600000) h.used = false;
    vTaskDelay(pdMS_TO_TICKS(250));
  }
}

}  // namespace

namespace net {

void begin() {
  loadSaved();
  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);
  WiFi.onEvent(onWifiEvent);
  s_info.gatewayPingMs = -1;
  xTaskCreatePinnedToCore(discoveryTask, "netscan", 6144, nullptr, 1, nullptr, 0);
}

bool connect(const char* ssid, const char* pass, uint8_t channel, const uint8_t* bssid, bool save) {
  if (s_state == NET_CONNECTED || s_state == NET_CONNECTING) {
    WiFi.disconnect();
    delay(50);
  }
  memset(&s_info, 0, sizeof(s_info));
  s_info.gatewayPingMs = -1;
  strlcpy(s_info.ssid, ssid, sizeof(s_info.ssid));
  strlcpy(s_pendPass, pass ? pass : "", sizeof(s_pendPass));
  s_pendSave = save;
  s_userDisconnected = false;
  s_discReason = 0;

  // Stop hopping; promiscuous off while the driver does auth/assoc
  radio::setStationChannel(channel ? channel : 1);
  radio::setPromiscuous(false);
  static const uint8_t kZero[6] = {0};
  if (channel && bssid && memcmp(bssid, kZero, 6) != 0) WiFi.begin(ssid, s_pendPass, channel, bssid);
  else WiFi.begin(ssid, s_pendPass);
  s_state = NET_CONNECTING;
  s_attemptStart = millis();
  snprintf(s_status, sizeof(s_status), "connecting to %s", ssid);
  return true;
}

void disconnect() {
  s_userDisconnected = true;
  WiFi.disconnect();
  if (s_mdnsUp) {
    MDNS.end();
    s_mdnsUp = false;
  }
  s_state = NET_IDLE;
  s_epoch = s_epoch + 1;
  strlcpy(s_status, "offline", sizeof(s_status));
  releaseRadio();
}

void update() {
  const uint32_t now = millis();
  switch (s_state) {
    case NET_CONNECTING: {
      if (WiFi.status() == WL_CONNECTED && (uint32_t)WiFi.localIP() != 0) {
        s_state = NET_CONNECTED;
        s_epoch = s_epoch + 1;
        s_info.connectedAt = now;
        s_info.channel = WiFi.channel();
        memcpy(s_info.bssid, WiFi.BSSID(), 6);
        s_info.ip = (uint32_t)WiFi.localIP();
        s_info.mask = (uint32_t)WiFi.subnetMask();
        s_info.gateway = (uint32_t)WiFi.gatewayIP();
        s_info.dns = (uint32_t)WiFi.dnsIP();
        WiFi.macAddress(s_info.mac);
        s_info.rssi = WiFi.RSSI();
        radio::setStationChannel(s_info.channel);
        radio::setPromiscuous(true);  // keep sniffing, on the AP's channel
        if (s_pendSave) remember(s_info.ssid, s_pendPass);
        if (!s_mdnsUp) s_mdnsUp = MDNS.begin("cyd-recon");
        snprintf(s_status, sizeof(s_status), "linked: %s", s_info.ssid);
        break;
      }
      const char* why = s_discReason ? reasonText(s_discReason) : nullptr;
      const bool hardFail = why && strcmp(why, "wrong password?") == 0 && now - s_attemptStart > 4000;
      if (hardFail || now - s_attemptStart > 20000) {
        WiFi.disconnect();
        s_state = NET_FAILED;
        if (why) snprintf(s_status, sizeof(s_status), "failed: %s", why);
        else if (s_discReason) snprintf(s_status, sizeof(s_status), "failed (reason %d)", s_discReason);
        else strlcpy(s_status, "failed: timeout", sizeof(s_status));
        releaseRadio();
        s_lastAuto = now;
      }
      break;
    }
    case NET_CONNECTED: {
      static uint32_t lastRssi = 0;
      if (WiFi.status() != WL_CONNECTED) {
        if (!s_lostAt) {
          s_lostAt = now;
          WiFi.reconnect();
          strlcpy(s_status, "link lost, retrying", sizeof(s_status));
        } else if (now - s_lostAt > 20000) {
          s_lostAt = 0;
          WiFi.disconnect();
          s_state = NET_FAILED;
          s_epoch = s_epoch + 1;
          strlcpy(s_status, "failed: link lost", sizeof(s_status));
          releaseRadio();
          s_lastAuto = now;
        }
      } else {
        if (s_lostAt) snprintf(s_status, sizeof(s_status), "linked: %s", s_info.ssid);
        s_lostAt = 0;
        if (now - lastRssi > 1000) {
          lastRssi = now;
          s_info.rssi = WiFi.RSSI();
        }
      }
      break;
    }
    default:
      // Auto-join the strongest saved network the sniffer can hear
      if (s_auto && !s_userDisconnected && s_nSaved && now > 8000 && now - s_lastAuto > 30000) {
        s_lastAuto = now;
        int best = -1;
        float bestRssi = -200;
        for (int i = 0; i < devices::capacity(); i++) {
          const Device& d = devices::at(i);
          if (!d.used || d.medium != MED_WIFI_AP || !d.name[0]) continue;
          if (now - d.lastSeen > 30000 || findSaved(d.name) < 0) continue;
          if (d.rssiFast > bestRssi) {
            bestRssi = d.rssiFast;
            best = i;
          }
        }
        if (best >= 0) {
          const Device& d = devices::at(best);
          connect(d.name, s_saved[findSaved(d.name)].pass, d.channel, d.mac, false);
        }
      }
      break;
  }
}

State state() { return s_state; }
const char* statusText() { return s_status; }
const Info& info() { return s_info; }

bool savedPassword(const char* ssid, char* out, size_t n) {
  const int i = findSaved(ssid);
  if (i < 0) return false;
  strlcpy(out, s_saved[i].pass, n);
  return true;
}

void forget(const char* ssid) {
  const int i = findSaved(ssid);
  if (i < 0) return;
  memmove(&s_saved[i], &s_saved[i + 1], sizeof(Saved) * (s_nSaved - i - 1));
  s_nSaved--;
  storeSaved();
}

bool autoConnect() { return s_auto; }
void setAutoConnect(bool on) {
  s_auto = on;
  storeSaved();
}

int hostCount() { return MAX_HOSTS; }
const Host& host(int i) { return s_hosts[i]; }
void rescan() { s_rescanReq = true; }
int sweepProgress() { return s_sweepPct; }
void probePorts(int idx) {
  if (idx >= 0 && idx < MAX_HOSTS && s_hosts[idx].used) {
    s_hosts[idx].portScan = 1;
    s_portReq = idx;
  }
}

const char* nameSrcText(uint8_t src) {
  switch (src) {
    case NAME_SELF: return "this device";
    case NAME_MDNS: return "mdns";
    case NAME_DNS: return "dns";
    case NAME_NETBIOS: return "netbios";
    default: return "";
  }
}

const char* portName(uint16_t port) {
  for (const auto& p : kPorts)
    if (p.port == port) return p.name;
  return "?";
}

void ipToStr(uint32_t ip, char* out) {
  snprintf(out, 16, "%u.%u.%u.%u", (unsigned)(ip & 0xFF), (unsigned)((ip >> 8) & 0xFF), (unsigned)((ip >> 16) & 0xFF),
           (unsigned)(ip >> 24));
}

}  // namespace net
