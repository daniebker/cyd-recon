// WiFi station mode: join a network, then map the LAN.
//  * credentials for up to 6 networks saved in NVS
//  * ARP sweep of the local /24 to find hosts, mDNS for names/services
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

struct Host {
  bool used;
  uint32_t ip;  // network byte order
  uint8_t mac[6];
  uint32_t firstSeen, lastSeen;
  char name[32];       // mDNS hostname
  char services[48];   // mDNS service tags, e.g. "http airplay"
  uint16_t ports[MAX_PORTS];
  uint8_t nPorts;
  uint8_t portScan;    // 0 never, 1 running, 2 done
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
const char* portName(uint16_t port);

void ipToStr(uint32_t ip, char* out);  // out >= 16

}  // namespace net
