#include "capture.h"
#include <FS.h>
#include <SD_MMC.h>
#include <sys/time.h>
#include <atomic>
#include "config.h"

namespace {

constexpr uint32_t RING_SIZE = 512 * 1024;           // power of two
constexpr uint32_t MAX_FRAME = 2400;                 // longest 802.11 frame we keep
constexpr uint32_t FILE_ROLL_BYTES = 100u << 20;     // start a new file after 100 MB
constexpr uint32_t WRITE_CHUNK = 16 * 1024;          // write once this much is queued...
constexpr uint32_t WRITE_MAX_WAIT_MS = 500;          // ...or this long has passed
constexpr uint32_t FLUSH_MS = 2000;                  // keep the file valid if power is cut
constexpr uint8_t RADIOTAP_LEN = 15;
constexpr uint32_t REC_HDR_LEN = 16 + RADIOTAP_LEN;  // pcap record header + radiotap

uint8_t* s_ring = nullptr;
std::atomic<uint32_t> s_head{0};  // producer (WiFi task) only
std::atomic<uint32_t> s_tail{0};  // writer task only
std::atomic<bool> s_accept{false};
std::atomic<uint32_t> s_pkts{0}, s_drop{0};

enum Req : uint8_t { REQ_NONE, REQ_START, REQ_STOP };
volatile Req s_req = REQ_NONE;

capture::Status s_st = {};
bool s_mounted = false;
File s_file;
uint32_t s_fileBytes = 0;
uint32_t s_lastFlush = 0;
int s_nextIdx = 1;

void put(uint32_t pos, const void* src, uint32_t n) {
  const uint32_t off = pos & (RING_SIZE - 1);
  const uint32_t first = min(n, RING_SIZE - off);
  memcpy(s_ring + off, src, first);
  if (n > first) memcpy(s_ring, (const uint8_t*)src + first, n - first);
}

void timestamp(uint32_t& sec, uint32_t& usec) {
  timeval tv;
  gettimeofday(&tv, nullptr);
  if (tv.tv_sec > 1600000000) {  // wall clock is set (NTP while joined to a network)
    sec = tv.tv_sec;
    usec = tv.tv_usec;
  } else {  // seconds since boot
    const int64_t us = esp_timer_get_time();
    sec = us / 1000000;
    usec = us % 1000000;
  }
}

bool mount() {
  if (s_mounted) return true;
  SD_MMC.setPins(PIN_SD_CLK, PIN_SD_CMD, PIN_SD_D0, PIN_SD_D1, PIN_SD_D2, PIN_SD_D3);
  if (!SD_MMC.begin("/sdcard", false, false, SDMMC_FREQ_DEFAULT, 2)) {
    SD_MMC.end();
    return false;
  }
  s_mounted = true;
  SD_MMC.mkdir("/pcap");
  // Continue numbering after the highest existing cap_NNNN.pcap
  File dir = SD_MMC.open("/pcap");
  if (dir) {
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
      int n = 0;
      if (sscanf(f.name(), "cap_%d.pcap", &n) == 1 && n >= s_nextIdx) s_nextIdx = n + 1;
    }
  }
  Serial.printf("[cap] sd mounted, %llu MB free\n", (SD_MMC.totalBytes() - SD_MMC.usedBytes()) >> 20);
  return true;
}

void unmount() {
  if (!s_mounted) return;
  SD_MMC.end();
  s_mounted = false;
}

bool openNext() {
  snprintf(s_st.file, sizeof(s_st.file), "/pcap/cap_%04d.pcap", s_nextIdx++);
  s_file = SD_MMC.open(s_st.file, FILE_WRITE);
  if (!s_file) return false;
  // pcap global header: microsecond timestamps, LINKTYPE_IEEE802_11_RADIOTAP
  const uint32_t hdr[6] = {0xA1B2C3D4, 0x00040002, 0, 0, 65535, 127};
  if (s_file.write((const uint8_t*)hdr, sizeof(hdr)) != sizeof(hdr)) return false;
  s_fileBytes = sizeof(hdr);
  s_st.bytes += sizeof(hdr);
  s_lastFlush = millis();
  Serial.printf("[cap] writing %s\n", s_st.file);
  return true;
}

void fail(const char* why) {
  Serial.printf("[cap] %s\n", why);
  s_accept = false;
  if (s_file) s_file.close();
  strlcpy(s_st.msg, why, sizeof(s_st.msg));
  s_st.state = capture::CAP_ERROR;
  unmount();  // card may have been pulled: remount fresh next time
}

void updateFree() {
  if (s_mounted) s_st.cardFree = SD_MMC.totalBytes() - SD_MMC.usedBytes();
}

// Write everything queued in the ring. Returns false on a write error.
bool drain() {
  const uint32_t h = s_head.load(std::memory_order_acquire);
  uint32_t t = s_tail.load(std::memory_order_relaxed);
  while (t != h) {
    const uint32_t off = t & (RING_SIZE - 1);
    const uint32_t n = min(h - t, RING_SIZE - off);
    if (s_file.write(s_ring + off, n) != n) return false;
    t += n;
    s_fileBytes += n;
    s_st.bytes += n;
    s_tail.store(t, std::memory_order_release);
  }
  s_st.packets = s_pkts;
  s_st.dropped = s_drop;
  return true;
}

void writerTask(void*) {
  uint32_t lastWrite = 0;
  for (;;) {
    const Req req = s_req;
    if (req == REQ_START && (s_st.state == capture::CAP_OFF || s_st.state == capture::CAP_ERROR)) {
      s_req = REQ_NONE;
      s_st.state = capture::CAP_STARTING;
      s_st.msg[0] = 0;
      s_st.packets = s_st.bytes = s_st.dropped = 0;
      s_pkts = 0;
      s_drop = 0;
      if (!mount()) {
        fail("no SD card (FAT32?)");
        continue;
      }
      if (!openNext()) {
        fail("can't create file on card");
        continue;
      }
      updateFree();
      s_tail.store(s_head.load());
      s_st.startedAt = millis();
      s_st.state = capture::CAP_REC;
      s_accept = true;
      lastWrite = millis();
    } else if (req == REQ_STOP) {
      s_req = REQ_NONE;
      if (s_st.state == capture::CAP_REC) {
        s_st.state = capture::CAP_STOPPING;
        s_accept = false;
        vTaskDelay(pdMS_TO_TICKS(20));  // let an in-flight frame land
        if (!drain()) {
          fail("write failed (card full?)");
          continue;
        }
        s_file.close();
        updateFree();
        snprintf(s_st.msg, sizeof(s_st.msg), "saved %lu pkts", (unsigned long)s_st.packets);
        Serial.printf("[cap] stopped: %s, %lu pkts, %lu bytes, %lu dropped\n", s_st.file,
                      (unsigned long)s_st.packets, (unsigned long)s_st.bytes, (unsigned long)s_st.dropped);
        s_st.state = capture::CAP_OFF;
      }
    }

    if (s_st.state == capture::CAP_REC) {
      const uint32_t now = millis();
      const uint32_t queued = s_head.load(std::memory_order_acquire) - s_tail.load(std::memory_order_relaxed);
      if (queued >= WRITE_CHUNK || (queued && now - lastWrite >= WRITE_MAX_WAIT_MS)) {
        if (!drain()) {
          fail("write failed (card full?)");
          continue;
        }
        lastWrite = now;
      }
      if (now - s_lastFlush >= FLUSH_MS) {
        s_file.flush();
        s_lastFlush = now;
        updateFree();
        s_st.packets = s_pkts;
        s_st.dropped = s_drop;
      }
      if (s_fileBytes >= FILE_ROLL_BYTES) {
        // Records never straddle files: the ring was just drained up to a record boundary
        s_accept = false;
        vTaskDelay(pdMS_TO_TICKS(20));
        if (!drain()) {
          fail("write failed (card full?)");
          continue;
        }
        s_file.close();
        if (!openNext()) {
          fail("can't create file on card");
          continue;
        }
        s_accept = true;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

}  // namespace

namespace capture {

void begin() {
  s_ring = (uint8_t*)heap_caps_malloc(RING_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!s_ring) {
    strlcpy(s_st.msg, "no PSRAM for buffer", sizeof(s_st.msg));
    s_st.state = CAP_ERROR;
    return;
  }
  xTaskCreatePinnedToCore(writerTask, "pcap", 4096, nullptr, 2, nullptr, 0);
}

void start() {
  if (s_ring) s_req = REQ_START;
}

void stop() { s_req = REQ_STOP; }

bool active() { return s_st.state == CAP_STARTING || s_st.state == CAP_REC || s_st.state == CAP_STOPPING; }

const Status& status() { return s_st; }

void onWifiFrame(const uint8_t* frame, uint32_t len, uint8_t channel, int8_t rssi) {
  if (!s_accept.load(std::memory_order_relaxed)) return;
  if (len > MAX_FRAME) len = MAX_FRAME;
  const uint32_t need = REC_HDR_LEN + len;
  const uint32_t h = s_head.load(std::memory_order_relaxed);
  if (RING_SIZE - (h - s_tail.load(std::memory_order_acquire)) < need) {
    s_drop.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  uint8_t hdr[REC_HDR_LEN];
  uint32_t sec, usec;
  timestamp(sec, usec);
  const uint32_t incl = RADIOTAP_LEN + len;
  memcpy(hdr + 0, &sec, 4);
  memcpy(hdr + 4, &usec, 4);
  memcpy(hdr + 8, &incl, 4);
  memcpy(hdr + 12, &incl, 4);
  // radiotap: present = flags | channel | dBm antenna signal
  uint8_t* rt = hdr + 16;
  const uint16_t freq = channel == 14 ? 2484 : 2407 + 5 * channel;
  const uint16_t chFlags = 0x0080;  // 2 GHz
  rt[0] = 0;               // version
  rt[1] = 0;               // pad
  rt[2] = RADIOTAP_LEN;    // length (LE)
  rt[3] = 0;
  rt[4] = 0x2A;            // present bits 1, 3, 5
  rt[5] = rt[6] = rt[7] = 0;
  rt[8] = 0x00;            // flags: no FCS in frame
  rt[9] = 0;               // pad: channel field is 2-byte aligned
  memcpy(rt + 10, &freq, 2);
  memcpy(rt + 12, &chFlags, 2);
  rt[14] = (uint8_t)rssi;
  put(h, hdr, REC_HDR_LEN);
  put(h + REC_HDR_LEN, frame, len);
  s_head.store(h + need, std::memory_order_release);
  s_pkts.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace capture
