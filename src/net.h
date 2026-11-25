// WiFi station mode: join a network, then map the LAN.
//  * credentials for up to 6 networks saved in NVS
//  * ARP sweep of the local /24 to find hosts, mDNS for names/services
//  * per-host name lookup: unicast mDNS reverse, router reverse DNS, NetBIOS
//  * gateway ping, on-demand TCP port probe of a host
// While connected the sniffer is pinned to the AP's channel.
#pragma once
#include <Arduino.h>

namespace net {

enum State : uint8_t { NET_IDLE, NET_CONNECTING, NET_CONNECTED, NET_FAILED };

struct Info {
  char ssid[33];
  uint8_t bssid[6];
  uint8_t channel;
  int8_t rssi;
  uint32_t ip, mask, gateway, dns;  // network byte order (as lwIP stores them)
  uint8_t mac[6];                   // our STA MAC
  uint32_t connectedAt;
  int gatewayPingMs;                // -1 unknown, -2 timeout
};

constexpr int MAX_HOSTS = 64;
constexpr int MAX_PORTS = 12;

// Where a host's name came from
enum NameSrc : uint8_t { NAME_NONE = 0, NAME_SELF, NAME_MDNS, NAME_DNS, NAME_NETBIOS };

struct Host {
  bool used;
  uint32_t ip;  // network byte order
  uint8_t mac[6];
  uint32_t firstSeen, lastSeen;
  char name[32];       // device name (see nameSrc), "" if unknown
  uint8_t nameSrc;     // NameSrc
  uint8_t nameTries;   // lookups attempted with no answer
  uint32_t nameAt;     // millis() of the last lookup attempt
  char services[48];   // mDNS service tags, e.g. "http airplay"
  uint16_t ports[MAX_PORTS];
  uint8_t nPorts;
  uint8_t portScan;    // 0 never, 1 running, 2 done
  // SSH posture (CHECK SSH): passive banner + KEXINIT algorithm audit, no login attempt
  uint8_t sshState;    // 0 never, 1 running, 2 done, 3 no ssh / unreachable
  uint8_t sshRisk;     // 0 modern, 1 caution, 2 weak/legacy
  char sshBanner[40];  // server version string, e.g. "OpenSSH_8.9p1"
  char sshWarn[72];    // findings, e.g. "weak: ssh-rsa hostkey, cbc cipher"
};

void begin();
void update();  // main loop: connection state machine, auto-connect

bool connect(const char* ssid, const char* pass, uint8_t channel, const uint8_t* bssid, bool save);
void disconnect();
State state();
const char* statusText();  // human readable status / failure reason
const Info& info();

// Saved networks
bool savedPassword(const char* ssid, char* passOut, size_t n);
void forget(const char* ssid);
bool autoConnect();
void setAutoConnect(bool on);

// LAN hosts (filled by the discovery task)
int hostCount();       // slots, check .used
const Host& host(int i);
void rescan();
int sweepProgress();   // 0..100, 100 = idle
void probePorts(int hostIdx);
void probeSsh(int hostIdx);   // passive SSH config audit of host:22 (no credentials tried)
const char* portName(uint16_t port);
const char* nameSrcText(uint8_t src);  // "mdns", "dns", "netbios", ...

void ipToStr(uint32_t ip, char* out);  // out >= 16

}  // namespace net
