#include "ui.h"
#include <LovyanGFX.hpp>
#include <math.h>
#include "audio.h"
#include "battery.h"
#include "config.h"
#include "devices.h"
#include "display.h"
#include "led.h"
#include "radio.h"
#include "signatures.h"
#include "theme.h"

using namespace theme;

namespace {

LGFX_Sprite g;  // 480x320 frame buffer in PSRAM

enum Screen : uint8_t { SCR_BOOT, SCR_LIST, SCR_DETAIL, SCR_SPECTRUM };
Screen s_screen = SCR_BOOT;

// ---------------- hit testing (immediate mode) ----------------
enum HitId : uint8_t {
  H_NONE,
  H_FILTER,
  H_SORT,
  H_SPECTRUM,
  H_ROW,
  H_BACK,
  H_SOUND,
  H_LOCK,
  H_MARK,
  H_TOAST,
  H_HOP,
  H_WIFI,
  H_BLE,
  H_CHBAR,
  H_CLEAR,
};
struct Hit {
  int16_t x, y, w, h;
  uint8_t id;
  int16_t arg;
};
Hit s_hits[48];
int s_nhits = 0;
void addHit(int x, int y, int w, int h, uint8_t id, int arg = 0) {
  if (s_nhits < 48) s_hits[s_nhits++] = {(int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h, id, (int16_t)arg};
}

// ---------------- state ----------------
devices::Filter s_filter = devices::F_ALL;
devices::Sort s_sort = devices::S_SIGNAL;
int s_scroll = 0;
int s_listIdx[MAX_DEVICES];
int s_listN = 0;

int s_sel = -1;           // tracked device slot
uint8_t s_selMac[6];
bool s_autoLocked = false;  // we locked the channel for tracking
bool s_userLock = false;    // user locked from spectrum view
float s_mark = NAN;         // reference RSSI

// RSSI history for the tracker graph (4 Hz, 40 s)
constexpr int HIST = 160;
int8_t s_hist[HIST];
int s_histHead = 0;
uint32_t s_lastHist = 0;
uint32_t s_lastPkts = 0;

// Spectrum: packets/s per channel + waterfall
float s_pps[WIFI_MAX_CH + 1];
uint32_t s_prevPkts[WIFI_MAX_CH + 1];
constexpr int WF_ROWS = 30;
uint8_t s_wf[WF_ROWS][WIFI_MAX_CH + 1];
int s_wfHead = 0;
float s_blePps = 0, s_wifiPps = 0;
uint32_t s_prevBle = 0, s_prevWifi = 0, s_lastRate = 0;

// Toast
int s_toastIdx = -1;
uint32_t s_toastUntil = 0;

// Event log for ticker
char s_log[6][56];
int s_logHead = 0;

// Touch gesture state
bool s_wasDown = false;
int16_t s_downX = 0, s_downY = 0;
int s_downScroll = 0;
bool s_dragging = false;

// Boot console
char s_boot[12][48];
int8_t s_bootStatus[12];
int s_bootN = 0;

// Glitch
uint32_t s_nextGlitch = 0, s_glitchUntil = 0;
uint32_t s_frameNo = 0;

// ---------------- helpers ----------------
void logEvent(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(s_log[s_logHead], sizeof(s_log[0]), fmt, ap);
  va_end(ap);
  s_logHead = (s_logHead + 1) % 6;
}

uint16_t lerp565(uint16_t a, uint16_t b, float t) {
  t = constrain(t, 0.f, 1.f);
  int ar = (a >> 11) & 31, ag = (a >> 5) & 63, ab = a & 31;
  int br = (b >> 11) & 31, bg = (b >> 5) & 63, bb = b & 31;
  return ((int)(ar + (br - ar) * t) << 11) | ((int)(ag + (bg - ag) * t) << 5) | (int)(ab + (bb - ab) * t);
}

// cold (blue) -> cyan -> yellow -> red (hot)
uint16_t heat(float t) {
  t = constrain(t, 0.f, 1.f);
  if (t < 0.33f) return lerp565(BLUE, CYAN, t / 0.33f);
  if (t < 0.66f) return lerp565(CYAN, YELLOW, (t - 0.33f) / 0.33f);
  return lerp565(YELLOW, RED, (t - 0.66f) / 0.34f);
}

float rssiNorm(float rssi) { return constrain((rssi + 100.f) / 65.f, 0.f, 1.f); }

void text(const char* s, int x, int y, uint16_t color, const lgfx::IFont* font = &fonts::Font0,
          textdatum_t datum = textdatum_t::top_left) {
  g.setFont(font);
  g.setTextDatum(datum);
  g.setTextColor(color);
  g.drawString(s, x, y);
}

// RGB-split "chromatic aberration" text
void neonText(const char* s, int x, int y, uint16_t color, const lgfx::IFont* font, textdatum_t datum, int split) {
  if (split) {
    text(s, x - split, y, MAGENTA_DIM, font, datum);
    text(s, x + split, y, CYAN_DIM, font, datum);
  }
  text(s, x, y, color, font, datum);
}

// Angular cut-corner frame used for buttons/panels
void cutRect(int x, int y, int w, int h, uint16_t color, bool fill, uint16_t fillColor = PANEL) {
  const int c = min(6, h / 3);
  if (fill) {
    g.fillRect(x + c, y, w - c, h, fillColor);
    g.fillRect(x, y + c, c, h - c, fillColor);
    g.fillTriangle(x, y + c, x + c, y, x + c, y + c, fillColor);
  }
  g.drawLine(x + c, y, x + w - 1, y, color);
  g.drawLine(x + w - 1, y, x + w - 1, y + h - 1, color);
  g.drawLine(x + w - 1, y + h - 1, x, y + h - 1, color);
  g.drawLine(x, y + h - 1, x, y + c, color);
  g.drawLine(x, y + c, x + c, y, color);
}

void button(int x, int y, int w, int h, const char* label, bool active, uint16_t color, uint8_t id, int arg = 0) {
  cutRect(x, y, w, h, color, true, active ? color : PANEL);
  text(label, x + w / 2, y + h / 2 + 1, active ? BLACK : color, &fonts::Font0, textdatum_t::middle_center);
  addHit(x, y, w, h, id, arg);
}

void signalBars(int x, int y, float rssi, uint16_t color) {
  const int lvl = (int)ceilf(rssiNorm(rssi) * 5.f);
  for (int i = 0; i < 5; i++) {
    const int h = 4 + i * 3;
    g.fillRect(x + i * 6, y + 16 - h, 4, h, i < lvl ? color : DARK);
  }
}

void ageStr(uint32_t ms, char* out, size_t n) {
  const uint32_t s = ms / 1000;
  if (s < 60) snprintf(out, n, "%lus", (unsigned long)s);
  else if (s < 3600) snprintf(out, n, "%lum", (unsigned long)(s / 60));
  else snprintf(out, n, "%luh", (unsigned long)(s / 3600));
}

void background() {
  g.fillScreen(BG);
  for (int y = 0; y < SCREEN_H; y += 3) g.drawFastHLine(0, y, SCREEN_W, SCAN);
}

// Battery widget: 4-segment cell icon with % underneath (fits the 28px header)
void drawBattery(int x) {
  const int pct = battery::percent();
  const auto st = battery::state();
  const uint32_t now = millis();
  const int w = 28, h = 13, y = 2;
  uint16_t col = GREEN;
  if (pct >= 0 && pct <= 15) col = RED;
  else if (pct >= 0 && pct <= 35) col = YELLOW;
  if (st == battery::BAT_CHARGING || st == battery::BAT_FULL) col = CYAN;
  const bool blinkOff = battery::low() && ((now / 400) & 1);

  g.drawRect(x, y, w, h, blinkOff ? RED : TEXT);
  g.fillRect(x + w, y + 4, 2, h - 8, blinkOff ? RED : TEXT);
  char buf[8];
  if (st == battery::BAT_ABSENT) {
    text("USB", x + w / 2, y + h / 2 + 1, CYAN, &fonts::Font0, textdatum_t::middle_center);
    text("PWR", x + w / 2, y + h + 2, DIM, &fonts::Font0, textdatum_t::top_center);
    return;
  }
  if (pct < 0) {
    text("..", x + w / 2, y + h / 2, DIM, &fonts::Font0, textdatum_t::middle_center);
    return;
  }
  // Segments; while charging, animate them filling up
  int segs = (pct + 24) / 25;
  if (st == battery::BAT_CHARGING) segs = max(segs - 1, 0) + (int)((now / 450) % (5 - max(segs - 1, 0)));
  segs = constrain(segs, (pct > 0) ? 1 : 0, 4);
  for (int i = 0; i < 4; i++) {
    if (i < segs && !blinkOff) g.fillRect(x + 2 + i * 6, y + 2, 5, h - 4, col);
  }
  if (st == battery::BAT_CHARGING) {
    // lightning bolt
    const int cx = x + w / 2, cy = y + h / 2;
    g.fillTriangle(cx + 2, cy - 6, cx - 4, cy + 1, cx, cy + 1, BLACK);
    g.fillTriangle(cx, cy - 1, cx + 4, cy - 1, cx - 2, cy + 6, BLACK);
    g.drawLine(cx + 2, cy - 5, cx - 3, cy, YELLOW);
    g.drawLine(cx - 3, cy, cx + 3, cy, YELLOW);
    g.drawLine(cx + 3, cy, cx - 1, cy + 5, YELLOW);
  }
  snprintf(buf, sizeof(buf), "%d%%", pct);
  text(st == battery::BAT_FULL ? "FULL" : buf, x + w / 2, y + h + 2, blinkOff ? RED : col, &fonts::Font0,
       textdatum_t::top_center);
}

// Low battery banner (shown once at 15 % and again at 5 %)
uint32_t s_batWarnUntil = 0;
int s_batWarnLevel = 0;
void checkBatteryWarning() {
  const int lvl = battery::critical() ? 2 : battery::low() ? 1 : 0;
  if (lvl > s_batWarnLevel) {
    s_batWarnUntil = millis() + 5000;
    audio::alert();
    led::flash(RED);
    logEvent("!! battery %s %d%% (%.2fV)", lvl == 2 ? "CRITICAL" : "low", battery::percent(), battery::volts());
  }
  // Re-arm once charged back up
  if (lvl < s_batWarnLevel && battery::percent() > 25) s_batWarnLevel = lvl;
  else s_batWarnLevel = max(s_batWarnLevel, lvl);
}
void drawBatteryWarning() {
  if (millis() > s_batWarnUntil) return;
  const bool crit = battery::critical();
  const bool blink = (millis() / 250) & 1;
  const int x = 90, y = 250, w = 300, h = 36;
  g.fillRect(x + 3, y + 3, w, h, BLACK);
  cutRect(x, y, w, h, blink ? RED : ORANGE, true, BG2);
  char buf[48];
  snprintf(buf, sizeof(buf), "!! %s BATTERY  %d%%  %.2fV", crit ? "CRITICAL" : "LOW", battery::percent(), battery::volts());
  neonText(buf, x + w / 2, y + h / 2, crit ? RED : ORANGE, &fonts::Font2, textdatum_t::middle_center, 1);
}

void header(const char* title) {
  const bool glitch = millis() < s_glitchUntil;
  const int split = glitch ? 2 + (esp_random() % 3) : 1;
  if (title) {
    neonText(title, 82, 6, CYAN, &fonts::Font2, textdatum_t::top_left, split > 1 ? split : 0);
  } else {
    neonText("CYD", 8, 2, MAGENTA, &fonts::Orbitron_Light_24, textdatum_t::top_left, split);
    neonText("//RECON", 60, 2, CYAN, &fonts::Orbitron_Light_24, textdatum_t::top_left, split);
  }
  // Right status block
  char buf[40];
  drawBattery(446);

  const int nf = devices::countFlagged();
  snprintf(buf, sizeof(buf), "WIFI %03d  BLE %03d", devices::countMedium(MED_WIFI_AP), devices::countMedium(MED_BLE));
  text(buf, 436, 4, TEXT, &fonts::Font0, textdatum_t::top_right);
  if (radio::hopping()) snprintf(buf, sizeof(buf), "CH%02d HOP", radio::channel());
  else snprintf(buf, sizeof(buf), "CH%02d LOCK", radio::channel());
  text(buf, 436, 15, radio::hopping() ? DIM : YELLOW, &fonts::Font0, textdatum_t::top_right);
  snprintf(buf, sizeof(buf), "ALERT %02d", nf);
  const bool blink = nf && ((millis() / 400) & 1);
  text(buf, 330, 15, nf ? (blink ? RED : MAGENTA) : DIM, &fonts::Font0, textdatum_t::top_right);

  g.drawFastHLine(0, 28, SCREEN_W, CYAN);
  g.drawFastHLine(0, 29, SCREEN_W, CYAN_DIM);
  // little tick marks on the divider
  for (int x = 4; x < SCREEN_W; x += 40) g.drawFastVLine(x, 26, 2, CYAN);
}

void footerTicker() {
  const int y = 298;
  g.drawFastHLine(0, y - 2, SCREEN_W, GREEN_DIM);
  char buf[96];
  const uint32_t up = millis() / 1000;
  const int idx = (s_frameNo / (TARGET_FPS * 3)) % 2;
  if (idx == 0) {
    snprintf(buf, sizeof(buf), "> wifi %4.0f pkt/s  ble %3.0f adv/s  devs %d  bat %.2fV  up %02lu:%02lu:%02lu",
             s_wifiPps, s_blePps, devices::count(), battery::volts(), (unsigned long)(up / 3600),
             (unsigned long)(up / 60 % 60), (unsigned long)(up % 60));
  } else {
    const char* last = s_log[(s_logHead + 5) % 6];
    snprintf(buf, sizeof(buf), "> %s", last[0] ? last : "listening...");
  }
  text(buf, 6, y + 4, GREEN, &fonts::Font0);
  if ((millis() / 500) & 1) {
    g.setFont(&fonts::Font0);
    g.fillRect(8 + g.textWidth(buf), y + 4, 6, 8, GREEN);
  }
}

// ---------------- toast ----------------
void drawToast() {
  if (s_toastIdx < 0 || millis() > s_toastUntil) {
    s_toastIdx = -1;
    return;
  }
  const Device& d = devices::at(s_toastIdx);
  if (!d.used) {
    s_toastIdx = -1;
    return;
  }
  const uint16_t c = devices::categoryColor(d.category);
  const int x = 30, y = 120, w = 420, h = 64;
  const bool blink = (millis() / 180) & 1;
  g.fillRect(x + 4, y + 4, w, h, BLACK);
  cutRect(x, y, w, h, blink ? RED : c, true, BG2);
  cutRect(x + 3, y + 3, w - 6, h - 6, c, false);
  char buf[64];
  snprintf(buf, sizeof(buf), "!! %s DETECTED", devices::categoryName(d.category));
  neonText(buf, x + 14, y + 10, c, &fonts::Font2, textdatum_t::top_left, 1);
  snprintf(buf, sizeof(buf), "[%s]  %s  %ddBm", d.label, d.name[0] ? d.name : "", d.rssi);
  text(buf, x + 14, y + 30, TEXT, &fonts::Font0);
  text(d.reason, x + 14, y + 42, DIM, &fonts::Font0);
  text("TAP TO TRACK >", x + w - 12, y + h - 12, blink ? YELLOW : c, &fonts::Font0, textdatum_t::middle_right);
  addHit(x, y, w, h, H_TOAST, s_toastIdx);
}

// ---------------- LIST SCREEN ----------------
constexpr int kListY = 58, kListH = 236, kRowH = 29;

void drawRow(int y, const Device& d, int idx) {
  const bool flagged = devices::isFlagged(d);
  const uint16_t c = devices::categoryColor(d.category);
  const uint32_t now = millis();
  const bool stale = now - d.lastSeen > 15000;

  g.fillRect(0, y, 472, kRowH - 1, flagged ? BG2 : ((idx & 1) ? BG : PANEL));
  g.fillRect(0, y, 3, kRowH - 1, c);
  if (flagged) {
    g.drawFastHLine(3, y, 469, lerp565(BG, c, 0.5f));
    g.drawFastHLine(3, y + kRowH - 2, 469, lerp565(BG, c, 0.5f));
  }

  // badge
  char lab[16];
  snprintf(lab, sizeof(lab), "%s%s", d.label, (d.category >= CAT_FLAGGED && d.confidence < CONF_MED) ? "?" : "");
  if (flagged) {
    g.fillRect(8, y + 6, 62, 16, c);
    text(lab, 39, y + 14, BLACK, &fonts::Font0, textdatum_t::middle_center);
  } else {
    g.drawRect(8, y + 6, 62, 16, lerp565(BG, c, 0.7f));
    text(lab, 39, y + 14, c, &fonts::Font0, textdatum_t::middle_center);
  }

  // name + subline
  char mac[18];
  devices::macToStr(d.mac, mac);
  const char* nm = d.name[0] ? d.name : (d.probe[0] ? d.probe : mac);
  char nbuf[40];
  snprintf(nbuf, sizeof(nbuf), "%s%s", (!d.name[0] && d.probe[0]) ? "?" : "", nm);
  g.setFont(&fonts::Font2);
  while (g.textWidth(nbuf) > 270 && strlen(nbuf) > 4) nbuf[strlen(nbuf) - 1] = 0;
  text(nbuf, 78, y + 1, stale ? DIM : TEXT, &fonts::Font2);

  char sub[64];
  const char* med = d.medium == MED_BLE ? "BT" : d.medium == MED_WIFI_AP ? "AP" : "ST";
  if (d.medium == MED_BLE)
    snprintf(sub, sizeof(sub), "%s %s %s", med, mac, d.vendor[0] ? d.vendor : (d.randomMac ? "rnd" : ""));
  else
    snprintf(sub, sizeof(sub), "%s %s ch%d %s", med, mac, d.channel, d.vendor[0] ? d.vendor : (d.randomMac ? "rnd" : ""));
  text(sub, 78, y + 18, DIM, &fonts::Font0);

  // RSSI + bars + age
  char r[8];
  snprintf(r, sizeof(r), "%d", (int)roundf(d.rssiFast));
  text(r, 398, y + 7, stale ? DIM : (flagged ? c : CYAN), &fonts::Font2, textdatum_t::top_right);
  signalBars(404, y + 6, stale ? -120 : d.rssiFast, flagged ? c : CYAN);
  char age[8];
  ageStr(now - d.lastSeen, age, sizeof(age));
  text(age, 468, y + 10, DIM, &fonts::Font0, textdatum_t::top_right);
  // trend arrow
  const float tr = d.rssiFast - d.rssiSlow;
  if (!stale && fabsf(tr) > 3.f) {
    const int ax = 440, ay = y + 14;
    if (tr > 0) g.fillTriangle(ax - 3, ay + 2, ax + 3, ay + 2, ax, ay - 3, GREEN);
    else g.fillTriangle(ax - 3, ay - 2, ax + 3, ay - 2, ax, ay + 3, BLUE);
  }
}

void drawList() {
  header(nullptr);
  // Tabs
  static const char* kTabs[] = {"ALL", "ALERTS", "WIFI", "BLE"};
  char lab[16];
  for (int i = 0; i < 4; i++) {
    if (i == 1) snprintf(lab, sizeof(lab), "ALERTS %d", devices::countFlagged());
    else snprintf(lab, sizeof(lab), "%s", kTabs[i]);
    button(4 + i * 78, 33, 74, 20, lab, s_filter == i, i == 1 ? MAGENTA : CYAN, H_FILTER, i);
  }
  static const char* kSort[] = {"SORT:SIGNAL", "SORT:NEWEST", "SORT:THREAT"};
  button(318, 33, 86, 20, kSort[s_sort], false, YELLOW, H_SORT);
  button(408, 33, 68, 20, "RF SCOPE", false, GREEN, H_SPECTRUM);

  s_listN = devices::list(s_filter, s_sort, s_listIdx, MAX_DEVICES);
  const int maxScroll = max(0, s_listN * kRowH - kListH);
  s_scroll = constrain(s_scroll, 0, maxScroll);

  g.setClipRect(0, kListY, SCREEN_W, kListH);
  const int first = s_scroll / kRowH;
  for (int i = first; i < s_listN; i++) {
    const int y = kListY + i * kRowH - s_scroll;
    if (y >= kListY + kListH) break;
    drawRow(y, devices::at(s_listIdx[i]), i);
    addHit(0, max(y, kListY), 472, min(kRowH, kListY + kListH - max(y, kListY)), H_ROW, s_listIdx[i]);
  }
  g.clearClipRect();

  if (s_listN == 0) {
    const char* msg = s_filter == devices::F_FLAGGED ? "NO THREATS IN RANGE" : "SCANNING THE ETHER...";
    neonText(msg, 240, 160, CYAN, &fonts::Font4, textdatum_t::middle_center, (millis() / 700) & 1);
    // spinner
    const float a = millis() / 4.f;
    g.fillArc(240, 220, 18, 14, a, a + 90, MAGENTA);
    g.fillArc(240, 220, 18, 14, a + 180, a + 270, CYAN);
  }

  // scrollbar
  if (maxScroll > 0) {
    const int bh = max(16, kListH * kListH / (s_listN * kRowH));
    const int by = kListY + (kListH - bh) * s_scroll / maxScroll;
    g.drawFastVLine(476, kListY, kListH, DARK);
    g.fillRect(474, by, 5, bh, CYAN);
  }
  footerTicker();
}

// ---------------- DETAIL / TRACK SCREEN ----------------
void histReset() {
  for (auto& h : s_hist) h = 127;
  s_histHead = 0;
}

void openDetail(int idx) {
  if (idx < 0 || !devices::at(idx).used) return;
  s_sel = idx;
  memcpy(s_selMac, devices::at(idx).mac, 6);
  s_screen = SCR_DETAIL;
  s_mark = NAN;
  histReset();
  s_lastPkts = devices::at(idx).packets;
  const Device& d = devices::at(idx);
  // Park the radio on the target's channel for fast RSSI updates
  if (d.medium != MED_BLE && d.channel && !s_userLock) {
    radio::lockChannel(d.channel);
    s_autoLocked = true;
  }
  char mac[18];
  devices::macToStr(d.mac, mac);
  logEvent("tracking %s %s", d.label, mac);
}

void closeDetail() {
  if (s_autoLocked && !s_userLock) radio::lockChannel(0);
  s_autoLocked = false;
  s_sel = -1;
  s_screen = SCR_LIST;
  audio::setGeiger(-1);
  led::setHeat(-1);
}

void kv(int x, int& y, const char* k, const char* v, uint16_t vc = TEXT) {
  text(k, x, y, DIM, &fonts::Font0);
  text(v, x + 46, y, vc, &fonts::Font0);
  y += 12;
}

void fmtCoord(int32_t lat, int32_t lon, char* out, size_t n) {
  snprintf(out, n, "%.5f,%.5f", lat / 1e7, lon / 1e7);
}

void drawGauge(const Device& d, bool live) {
  const int cx = 359, cy = 122, r1 = 80, r0 = 64;
  const float lvl = rssiNorm(d.rssiFast);
  constexpr int SEG = 30;
  constexpr float A0 = 135.f, SWEEP = 270.f;
  const int lit = live ? (int)roundf(lvl * SEG) : 0;
  for (int i = 0; i < SEG; i++) {
    const float a = A0 + SWEEP * i / SEG;
    const uint16_t col = i < lit ? heat((float)i / SEG) : DARK;
    g.fillArc(cx, cy, r1, r0, a + 1, a + SWEEP / SEG - 1, col);
  }
  // peak marker
  const float pk = A0 + SWEEP * rssiNorm(d.rssiMax);
  g.fillArc(cx, cy, r1 + 5, r1 + 2, pk - 2, pk + 2, MAGENTA);
  // mark marker
  if (!isnan(s_mark)) {
    const float mk = A0 + SWEEP * rssiNorm(s_mark);
    g.fillArc(cx, cy, r0 - 2, r0 - 6, mk - 2, mk + 2, YELLOW);
  }
  g.drawCircle(cx, cy, r1 + 8, DARK);

  char buf[16];
  snprintf(buf, sizeof(buf), "%d", (int)roundf(d.rssiFast));
  const uint16_t vc = live ? heat(lvl) : DIM;
  text(buf, cx, cy - 4, vc, &fonts::Font7, textdatum_t::middle_center);
  text("dBm", cx, cy + 28, DIM, &fonts::Font2, textdatum_t::middle_center);

  // Distance guess (log-distance path loss, very rough)
  const float ref = d.medium == MED_BLE ? -59.f : -40.f;
  const float n = 2.7f;
  const float dist = powf(10.f, (ref - d.rssiFast) / (10.f * n));
  if (dist < 10) snprintf(buf, sizeof(buf), "~%.1fm", dist);
  else snprintf(buf, sizeof(buf), "~%.0fm", dist);
  text(buf, cx, cy + 48, TEXT, &fonts::Font2, textdatum_t::middle_center);

  // Hot / cold verdict
  const float tr = d.rssiFast - d.rssiSlow;
  const char* verdict;
  uint16_t tc;
  if (!live) {
    verdict = "SIGNAL LOST";
    tc = DIM;
  } else if (tr > 2.5f) {
    verdict = tr > 6 ? "HOTTER >>" : "WARMER >";
    tc = tr > 6 ? RED : ORANGE;
  } else if (tr < -2.5f) {
    verdict = tr < -6 ? "<< COLDER" : "< COOLER";
    tc = tr < -6 ? BLUE : CYAN;
  } else {
    verdict = "STEADY";
    tc = TEXT;
  }
  const bool flash = live && fabsf(tr) > 6 && ((millis() / 250) & 1);
  neonText(verdict, cx, 216, flash ? YELLOW : tc, &fonts::Font4, textdatum_t::middle_center, 1);

  if (!isnan(s_mark)) {
    snprintf(buf, sizeof(buf), "vs MARK %+d", (int)roundf(d.rssiFast - s_mark));
    text(buf, cx, cy - 46, YELLOW, &fonts::Font0, textdatum_t::middle_center);
  }
}

void drawGraph(int x, int y, int w, int h) {
  cutRect(x, y, w, h, CYAN_DIM, true, BG);
  const auto yOf = [&](float r) { return y + h - 3 - (int)(rssiNorm(r) * (h - 6)); };
  for (int r = -90; r <= -40; r += 20) {
    const int gy = yOf(r);
    for (int gx = x + 4; gx < x + w - 4; gx += 6) g.drawPixel(gx, gy, GRID);
    char b[6];
    snprintf(b, sizeof(b), "%d", r);
    text(b, x + 4, gy - 9, DARK, &fonts::Font0);
  }
  const float step = (float)(w - 8) / (HIST - 1);
  int px = -1, py = -1;
  for (int i = 0; i < HIST; i++) {
    const int8_t v = s_hist[(s_histHead + i) % HIST];
    const int sx = x + 4 + (int)(i * step);
    if (v == 127) {
      px = -1;
      continue;
    }
    const int sy = yOf(v);
    const uint16_t c = heat(rssiNorm(v));
    g.drawFastVLine(sx, sy, y + h - 3 - sy, lerp565(BG, c, 0.25f));
    if (px >= 0) g.drawLine(px, py, sx, sy, c);
    px = sx;
    py = sy;
  }
  if (!isnan(s_mark)) {
    const int my = yOf(s_mark);
    for (int gx = x + 4; gx < x + w - 4; gx += 4) g.drawFastHLine(gx, my, 2, YELLOW);
    text("MARK", x + w - 6, my - 9, YELLOW, &fonts::Font0, textdatum_t::top_right);
  }
  text("RSSI / 40s", x + w - 6, y + 4, DIM, &fonts::Font0, textdatum_t::top_right);
}

void drawDetail() {
  Device& d = devices::at(s_sel);
  if (!d.used || memcmp(d.mac, s_selMac, 6) != 0) {
    closeDetail();
    return;
  }
  const uint32_t now = millis();
  const bool live = now - d.lastSeen < 5000;
  const uint16_t c = devices::categoryColor(d.category);

  // History sampling at 4 Hz
  if (now - s_lastHist >= 250) {
    s_lastHist = now;
    const bool fresh = d.packets != s_lastPkts;
    s_lastPkts = d.packets;
    s_hist[s_histHead] = fresh ? (int8_t)roundf(d.rssiFast) : 127;
    s_histHead = (s_histHead + 1) % HIST;
  }

  // Header
  button(4, 3, 70, 22, "< BACK", false, CYAN, H_BACK);
  char title[48];
  snprintf(title, sizeof(title), "TARGET // %s", d.label);
  neonText(title, 84, 6, c, &fonts::Font2, textdatum_t::top_left, 1);
  const bool blink = (now / 500) & 1;
  if (live) {
    g.fillCircle(356, 14, 4, blink ? RED : MAGENTA_DIM);
    text("TRACKING", 366, 10, RED, &fonts::Font0);
  } else {
    text("NO SIGNAL", 366, 10, DIM, &fonts::Font0);
  }
  drawBattery(446);
  g.drawFastHLine(0, 28, SCREEN_W, c);
  g.drawFastHLine(0, 29, SCREEN_W, lerp565(BG, c, 0.4f));

  // Info panel
  cutRect(4, 34, 234, 196, lerp565(BG, c, 0.6f), true, BG2);
  int y = 40;
  const int x = 10;
  char buf[64], mac[18];
  devices::macToStr(d.mac, mac);
  kv(x, y, "TYPE", devices::categoryName(d.category), c);
  static const char* kConf[] = {"-", "LOW", "MEDIUM", "HIGH"};
  snprintf(buf, sizeof(buf), "%s  conf:%s", d.label, kConf[d.confidence & 3]);
  kv(x, y, "ID", buf, c);
  kv(x, y, "NAME", d.name[0] ? d.name : "-");
  snprintf(buf, sizeof(buf), "%s%s", mac, d.randomMac ? " RAND" : "");
  kv(x, y, "MAC", buf);
  const char* co = d.company != 0xFFFF ? signatures::companyName(d.company) : nullptr;
  if (d.vendor[0]) kv(x, y, "VENDOR", d.vendor);
  else if (d.company != 0xFFFF) {
    snprintf(buf, sizeof(buf), "%s (0x%04X)", co ? co : "?", d.company);
    kv(x, y, "VENDOR", buf);
  } else kv(x, y, "VENDOR", "unknown", DIM);
  if (d.medium == MED_BLE) {
    static const char* kAt[] = {"public", "random", "rpa-pub", "rpa-rnd"};
    snprintf(buf, sizeof(buf), "BLE %s%s", kAt[d.addrType & 3], d.connectable ? " conn" : "");
  } else {
    static const char* kSec[] = {"OPEN", "WEP", "WPA", "WPA2+"};
    if (d.medium == MED_WIFI_AP) snprintf(buf, sizeof(buf), "WiFi AP ch%d %s", d.channel, kSec[d.security & 3]);
    else snprintf(buf, sizeof(buf), "WiFi client ch%d", d.channel);
  }
  kv(x, y, "RADIO", buf);
  char a1[8], a2[8];
  ageStr(now - d.firstSeen, a1, sizeof(a1));
  ageStr(now - d.lastSeen, a2, sizeof(a2));
  snprintf(buf, sizeof(buf), "first %s ago, last %s", a1, a2);
  kv(x, y, "SEEN", buf);
  snprintf(buf, sizeof(buf), "%lu  peak %ddBm", (unsigned long)d.packets, d.rssiMax);
  kv(x, y, "PKTS", buf);
  if (d.probe[0]) kv(x, y, "PROBE", d.probe, YELLOW);
  if (d.reason[0]) kv(x, y, "MATCH", d.reason, c);
  if (d.rid.present) {
    kv(x, y, "RID", d.rid.serial[0] ? d.rid.serial : "(no serial yet)", RED);
    if (d.rid.lat || d.rid.lon) {
      fmtCoord(d.rid.lat, d.rid.lon, buf, sizeof(buf));
      kv(x, y, "UAV", buf, RED);
      if (d.rid.altM != INT16_MIN) {
        snprintf(buf, sizeof(buf), "%dm  %.1fm/s", d.rid.altM, d.rid.speedCms / 100.f);
        kv(x, y, "ALT", buf, RED);
      }
    }
    if (d.rid.opLat || d.rid.opLon) {
      fmtCoord(d.rid.opLat, d.rid.opLon, buf, sizeof(buf));
      kv(x, y, "PILOT", buf, ORANGE);
    }
  }
  // Raw advertisement hex
  if (d.medium == MED_BLE && d.advLen && y < 206) {
    char hex[40];
    for (int line = 0; line < 2 && y < 222; line++) {
      int p = 0;
      for (int i = line * 12; i < min<int>(d.advLen, line * 12 + 12); i++) p += snprintf(hex + p, sizeof(hex) - p, "%02X ", d.adv[i]);
      if (!p) break;
      text(line == 0 ? "ADV" : "", x, y, DIM, &fonts::Font0);
      text(hex, x + 46, y, GREEN_DIM, &fonts::Font0);
      y += 11;
    }
  }

  drawGauge(d, live);
  drawGraph(4, 234, 472, 58);

  // Footer controls
  const bool snd = audio::enabled();
  button(4, 297, 112, 21, snd ? "GEIGER: ON" : "GEIGER: OFF", snd, GREEN, H_SOUND);
  if (d.medium != MED_BLE) {
    char lb[24];
    snprintf(lb, sizeof(lb), radio::hopping() ? "CH LOCK: OFF" : "CH LOCK: %d", radio::channel());
    button(122, 297, 112, 21, lb, !radio::hopping(), YELLOW, H_LOCK);
  } else {
    text("BLE: all channels", 178, 307, DIM, &fonts::Font0, textdatum_t::middle_center);
  }
  button(240, 297, 112, 21, isnan(s_mark) ? "SET MARK" : "CLEAR MARK", !isnan(s_mark), YELLOW, H_MARK);
  button(358, 297, 118, 21, "RESET PEAK", false, MAGENTA, H_CLEAR);

  // Geiger + LED follow the target
  if (live) {
    const float s = rssiNorm(d.rssiFast);
    audio::setGeiger(s);
    led::setHeat(s);
  } else {
    audio::setGeiger(-1);
    led::setHeat(-1);
  }
}

// ---------------- SPECTRUM SCREEN ----------------
void drawSpectrum() {
  button(4, 3, 70, 22, "< BACK", false, CYAN, H_BACK);
  neonText("RF SCOPE // 2.4GHz", 84, 6, GREEN, &fonts::Font2, textdatum_t::top_left, 1);
  char buf[48];
  snprintf(buf, sizeof(buf), "WIFI %.0f/s  BLE %.0f/s", s_wifiPps, s_blePps);
  text(buf, 436, 10, TEXT, &fonts::Font0, textdatum_t::top_right);
  drawBattery(446);
  g.drawFastHLine(0, 28, SCREEN_W, GREEN);
  g.drawFastHLine(0, 29, SCREEN_W, GREEN_DIM);

  // Bars
  const int bx0 = 42, bw = 30, gap = 3, base = 162, maxH = 112;
  float peak = 10;
  for (int ch = 1; ch <= WIFI_MAX_CH; ch++) peak = max(peak, s_pps[ch]);
  const auto* st = radio::channelStats();
  for (int ch = 1; ch <= WIFI_MAX_CH; ch++) {
    const int x = bx0 + (ch - 1) * (bw + gap);
    const float t = log1pf(s_pps[ch]) / log1pf(peak);
    const int h = (int)(t * maxH);
    g.fillRect(x, base - maxH, bw, maxH, BG2);
    for (int k = 0; k < h; k += 4) g.fillRect(x + 1, base - k - 3, bw - 2, 3, heat((float)k / maxH));
    const bool cur = radio::channel() == ch;
    g.drawRect(x - 1, base - maxH - 1, bw + 2, maxH + 2, cur ? YELLOW : GRID);
    snprintf(buf, sizeof(buf), "%.0f", s_pps[ch]);
    text(buf, x + bw / 2, max(base - h - 11, base - maxH + 2), TEXT, &fonts::Font0, textdatum_t::top_center);
    snprintf(buf, sizeof(buf), "%02d", ch);
    text(buf, x + bw / 2, base + 4, cur ? YELLOW : TEXT, &fonts::Font0, textdatum_t::top_center);
    if (st[ch].maxRssi > -127) {
      snprintf(buf, sizeof(buf), "%d", st[ch].maxRssi);
      text(buf, x + bw / 2, base + 15, CYAN_DIM, &fonts::Font0, textdatum_t::top_center);
    }
    if (cur) g.fillTriangle(x + bw / 2 - 4, base - maxH - 9, x + bw / 2 + 4, base - maxH - 9, x + bw / 2, base - maxH - 3, YELLOW);
    addHit(x - 1, base - maxH - 10, bw + 2, maxH + 36, H_CHBAR, ch);
  }
  text("PKT/S", 4, base - maxH, DIM, &fonts::Font0);
  text("CH", 4, base + 4, DIM, &fonts::Font0);
  text("dBm", 4, base + 15, DIM, &fonts::Font0);

  // Waterfall (newest row on top)
  const int wy = 192, cellH = 3, rows = 28;
  text("TIME", 4, wy, DIM, &fonts::Font0);
  text("1s/", 4, wy + 12, DARK, &fonts::Font0);
  text("row", 4, wy + 22, DARK, &fonts::Font0);
  for (int r = 0; r < rows; r++) {
    const int row = (s_wfHead - 1 - r + WF_ROWS) % WF_ROWS;
    for (int ch = 1; ch <= WIFI_MAX_CH; ch++) {
      const uint8_t v = s_wf[row][ch];
      const int x = bx0 + (ch - 1) * (bw + gap);
      g.fillRect(x, wy + r * cellH, bw, cellH - 1, v ? heat(v / 255.f) : BG2);
    }
  }
  text("tap a channel to lock it, HOP to resume hopping", 240, 285, DIM, &fonts::Font0, textdatum_t::top_center);

  // Controls
  char lb[24];
  if (radio::hopping()) snprintf(lb, sizeof(lb), "HOP: AUTO");
  else snprintf(lb, sizeof(lb), "LOCKED CH %d", radio::channel());
  button(4, 297, 116, 21, lb, !radio::hopping(), YELLOW, H_HOP);
  button(126, 297, 100, 21, radio::wifiEnabled() ? "WIFI: ON" : "WIFI: OFF", radio::wifiEnabled(), CYAN, H_WIFI);
  button(232, 297, 100, 21, radio::bleEnabled() ? "BLE: ON" : "BLE: OFF", radio::bleEnabled(), BLUE, H_BLE);
  button(338, 297, 138, 21, audio::enabled() ? "SOUND: ON" : "SOUND: OFF", audio::enabled(), GREEN, H_SOUND);
}

// ---------------- BOOT ----------------
void drawBoot() {
  background();
  const bool gl = (esp_random() & 7) == 0;
  g.setFont(&fonts::Orbitron_Light_32);
  const int w1 = g.textWidth("CYD"), w2 = g.textWidth("//RECON");
  const int lx = (SCREEN_W - w1 - w2) / 2;
  neonText("CYD", lx, 40, MAGENTA, &fonts::Orbitron_Light_32, textdatum_t::top_left, gl ? 4 : 2);
  neonText("//RECON", lx + w1, 40, CYAN, &fonts::Orbitron_Light_32, textdatum_t::top_left, gl ? 4 : 2);
  text("PASSIVE RF RECON TERMINAL  v" FW_VERSION, 240, 84, DIM, &fonts::Font0, textdatum_t::top_center);
  g.drawFastHLine(60, 98, 360, CYAN_DIM);
  for (int i = 0; i < s_bootN; i++) {
    const int y = 110 + i * 14;
    text(s_boot[i], 70, y, GREEN, &fonts::Font0);
    if (s_bootStatus[i] == 1) text("[ OK ]", 410, y, CYAN, &fonts::Font0, textdatum_t::top_right);
    if (s_bootStatus[i] == -1) text("[FAIL]", 410, y, RED, &fonts::Font0, textdatum_t::top_right);
  }
  if ((millis() / 300) & 1) g.fillRect(70, 110 + s_bootN * 14, 6, 8, GREEN);
}

// Random horizontal band displacement - the "signal interference" look
void glitchPass() {
  const uint32_t now = millis();
  if (now >= s_nextGlitch) {
    s_glitchUntil = now + 90 + esp_random() % 120;
    s_nextGlitch = now + 4000 + esp_random() % 6000;
  }
  if (now >= s_glitchUntil) return;
  auto* buf = (uint16_t*)g.getBuffer();
  const int bands = 2 + esp_random() % 3;
  for (int b = 0; b < bands; b++) {
    const int y0 = esp_random() % (SCREEN_H - 12);
    const int h = 2 + esp_random() % 10;
    const int off = (int)(esp_random() % 17) - 8;
    if (!off) continue;
    for (int y = y0; y < y0 + h; y++) {
      uint16_t* row = buf + y * SCREEN_W;
      if (off > 0) memmove(row + off, row, (SCREEN_W - off) * 2);
      else memmove(row, row - off, (SCREEN_W + off) * 2);
    }
  }
}

// ---------------- input ----------------
void onTap(int x, int y) {
  for (int i = s_nhits - 1; i >= 0; i--) {  // topmost (last drawn) first
    const Hit& h = s_hits[i];
    if (x < h.x || y < h.y || x >= h.x + h.w || y >= h.y + h.h) continue;
    audio::blip();
    switch (h.id) {
      case H_FILTER:
        s_filter = (devices::Filter)h.arg;
        s_scroll = 0;
        break;
      case H_SORT:
        s_sort = (devices::Sort)((s_sort + 1) % devices::S_COUNT);
        break;
      case H_SPECTRUM:
        s_screen = SCR_SPECTRUM;
        break;
      case H_ROW:
      case H_TOAST:
        s_toastIdx = -1;
        openDetail(h.arg);
        break;
      case H_BACK:
        if (s_screen == SCR_DETAIL) closeDetail();
        else s_screen = SCR_LIST;
        led::setHeat(-1);
        break;
      case H_SOUND:
        audio::setEnabled(!audio::enabled());
        break;
      case H_LOCK:
        if (radio::hopping()) {
          const Device& d = devices::at(s_sel);
          radio::lockChannel(d.channel);
          s_autoLocked = true;
        } else {
          radio::lockChannel(0);
          s_autoLocked = s_userLock = false;
        }
        break;
      case H_MARK:
        s_mark = isnan(s_mark) && s_sel >= 0 ? devices::at(s_sel).rssiFast : NAN;
        break;
      case H_CLEAR:
        if (s_sel >= 0) devices::at(s_sel).rssiMax = devices::at(s_sel).rssi;
        histReset();
        break;
      case H_HOP:
        radio::lockChannel(0);
        s_userLock = false;
        break;
      case H_CHBAR:
        radio::lockChannel(h.arg);
        s_userLock = true;
        break;
      case H_WIFI:
        radio::setWifiEnabled(!radio::wifiEnabled());
        break;
      case H_BLE:
        radio::setBleEnabled(!radio::bleEnabled());
        break;
    }
    return;
  }
}

void handleTouch(const touch::State& t) {
  if (t.down && !s_wasDown) {
    s_downX = t.x;
    s_downY = t.y;
    s_downScroll = s_scroll;
    s_dragging = false;
  } else if (t.down && s_wasDown) {
    const int dy = t.y - s_downY;
    if (!s_dragging && (abs(dy) > 10 || abs(t.x - s_downX) > 10)) s_dragging = true;
    if (s_dragging && s_screen == SCR_LIST && s_downY >= kListY) s_scroll = s_downScroll - dy;
  } else if (!t.down && s_wasDown) {
    if (!s_dragging) onTap(s_downX, s_downY);
  }
  s_wasDown = t.down;
}

void updateRates() {
  const uint32_t now = millis();
  if (now - s_lastRate < 1000) return;
  const float dt = (now - s_lastRate) / 1000.f;
  s_lastRate = now;
  const auto* st = radio::channelStats();
  for (int ch = 1; ch <= WIFI_MAX_CH; ch++) {
    const uint32_t p = st[ch].packets;
    s_pps[ch] = s_pps[ch] * 0.4f + 0.6f * ((p - s_prevPkts[ch]) / dt);
    s_prevPkts[ch] = p;
    s_wf[s_wfHead][ch] = (uint8_t)constrain(log1pf(s_pps[ch]) / log1pf(300.f) * 255.f, 0.f, 255.f);
  }
  s_wfHead = (s_wfHead + 1) % WF_ROWS;
  const uint32_t w = radio::wifiPackets(), b = radio::blePackets();
  s_wifiPps = (w - s_prevWifi) / dt;
  s_blePps = (b - s_prevBle) / dt;
  s_prevWifi = w;
  s_prevBle = b;
}

}  // namespace

namespace ui {

bool begin() {
  g.setPsram(true);
  g.setColorDepth(16);
  if (!g.createSprite(SCREEN_W, SCREEN_H)) return false;
  histReset();
  return true;
}

void bootLine(const char* t, int status) {
  if (s_bootN > 0 && status != 0 && strcmp(s_boot[s_bootN - 1], t) == 0) {
    s_bootStatus[s_bootN - 1] = status;
  } else if (s_bootN < 12) {
    strlcpy(s_boot[s_bootN], t, sizeof(s_boot[0]));
    s_bootStatus[s_bootN] = status;
    s_bootN++;
  }
  drawBoot();
  display::pushFrame((const uint16_t*)g.getBuffer());
}

void bootDone() {
  const uint32_t t0 = millis();
  while (millis() - t0 < 900) {
    drawBoot();
    // wipe-in effect
    const int p = (millis() - t0) * SCREEN_H / 900;
    g.fillRect(0, 0, SCREEN_W, p, BG);
    g.drawFastHLine(0, p, SCREEN_W, CYAN);
    display::pushFrame((const uint16_t*)g.getBuffer());
  }
  s_screen = SCR_LIST;
  s_nextGlitch = millis() + 2500;
}

int trackedIndex() { return s_screen == SCR_DETAIL ? s_sel : -1; }

void onNewDevice(int idx) {
  const Device& d = devices::at(idx);
  char mac[18];
  devices::macToStr(d.mac, mac);
  logEvent("+%s %s %s %ddBm", d.medium == MED_BLE ? "bt" : "wf", d.label, d.name[0] ? d.name : mac, d.rssi);
}

void frame(const touch::State& t, bool backButton) {
  s_frameNo++;
  updateRates();

  if (backButton && s_screen != SCR_LIST) {
    if (s_screen == SCR_DETAIL) closeDetail();
    else s_screen = SCR_LIST;
    led::setHeat(-1);
  }

  // Alerts for newly flagged devices
  const int a = devices::popAlert();
  if (a >= 0) {
    const Device& d = devices::at(a);
    char mac[18];
    devices::macToStr(d.mac, mac);
    logEvent("!! %s %s %s", d.label, d.name[0] ? d.name : mac, d.reason);
    Serial.printf("[ALERT] %s,%s,%s,%d,%s\n", devices::categoryName(d.category), d.label, mac, d.rssi, d.reason);
    if (!(s_screen == SCR_DETAIL && s_sel == a)) {
      s_toastIdx = a;
      s_toastUntil = millis() + 4500;
    }
    audio::alert();
    led::flash(devices::categoryColor(d.category));
  }

  checkBatteryWarning();
  handleTouch(t);
  s_nhits = 0;
  background();
  switch (s_screen) {
    case SCR_LIST:
      drawList();
      break;
    case SCR_DETAIL:
      drawDetail();
      break;
    case SCR_SPECTRUM:
      drawSpectrum();
      break;
    default:
      break;
  }
  if (s_screen != SCR_DETAIL) drawToast();
  drawBatteryWarning();
  glitchPass();
  display::pushFrame((const uint16_t*)g.getBuffer());
}

}  // namespace ui
