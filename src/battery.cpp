#include "battery.h"
#include "config.h"

namespace {

constexpr int AVG_N = 16;      // 4 s window at 4 Hz
float s_buf[AVG_N];
int s_n = 0, s_head = 0;
float s_volts = 0;
int s_pct = -1;
battery::State s_state = battery::BAT_UNKNOWN;
uint32_t s_lastSample = 0;

// Charge detection. Plugging a charger in makes the cell voltage jump up by
// tens of mV almost instantly (charge current through the cell's internal
// resistance); unplugging makes it drop by the same amount. We watch for those
// steps, and also for a slow sustained rise as a fallback.
constexpr int STEP_N = 6;     // 4 s averages, one every 2 s -> 12 s history
float s_stepHist[STEP_N];
int s_stepCount = 0, s_stepHead = 0;
uint32_t s_lastStep = 0;
float s_trendRef = 0;         // slow trend: compared every 2 minutes
uint32_t s_trendT = 0;
bool s_charging = false;

// Typical single-cell LiPo resting curve (volts -> percent)
struct Pt {
  float v;
  int p;
};
const Pt kCurve[] = {
    {4.20f, 100}, {4.10f, 92}, {4.00f, 83}, {3.92f, 74}, {3.85f, 64}, {3.80f, 55}, {3.75f, 45},
    {3.71f, 36},  {3.67f, 27}, {3.62f, 18}, {3.55f, 10}, {3.45f, 5},  {3.30f, 0},
};

int curvePercent(float v) {
  if (v >= kCurve[0].v) return 100;
  for (size_t i = 1; i < sizeof(kCurve) / sizeof(kCurve[0]); i++) {
    if (v >= kCurve[i].v) {
      const Pt& a = kCurve[i];
      const Pt& b = kCurve[i - 1];
      return a.p + (int)((v - a.v) / (b.v - a.v) * (b.p - a.p) + 0.5f);
    }
  }
  return 0;
}

}  // namespace

namespace battery {

void begin() {
  pinMode(PIN_BAT_ADC, INPUT);
  analogSetPinAttenuation(PIN_BAT_ADC, ADC_11db);
}

void update() {
  const uint32_t now = millis();
  if (s_n && now - s_lastSample < 250) return;
  s_lastSample = now;

  const float v = analogReadMilliVolts(PIN_BAT_ADC) * 2.0f / 1000.0f;
  s_buf[s_head] = v;
  s_head = (s_head + 1) % AVG_N;
  if (s_n < AVG_N) s_n++;
  float sum = 0, mn = 99, mx = 0;
  for (int i = 0; i < s_n; i++) {
    sum += s_buf[i];
    mn = min(mn, s_buf[i]);
    mx = max(mx, s_buf[i]);
  }
  s_volts = sum / s_n;
  if (s_n < 4) return;  // need a few samples first

  // No cell: the divider floats near 0, or the charger output hunts around
  // with no load to stabilise it (large ripple between samples).
  if (s_volts < 2.8f || s_volts > 4.6f || (mx - mn) > 0.35f) {
    s_state = BAT_ABSENT;
    s_pct = -1;
    return;
  }

  // Charge detection: step response
  if (now - s_lastStep >= 2000) {
    s_lastStep = now;
    if (s_stepCount == STEP_N) {
      const float old = s_stepHist[s_stepHead];  // ~12 s ago
      const float dv = s_volts - old;
      if (dv > 0.035f) s_charging = true;
      else if (dv < -0.035f) s_charging = false;
    }
    s_stepHist[s_stepHead] = s_volts;
    s_stepHead = (s_stepHead + 1) % STEP_N;
    if (s_stepCount < STEP_N) s_stepCount++;
  }
  // ...and slow trend over 2 minutes
  if (!s_trendT) {
    s_trendRef = s_volts;
    s_trendT = now;
  } else if (now - s_trendT > 120000) {
    const float dv = s_volts - s_trendRef;
    if (dv > 0.010f) s_charging = true;
    else if (dv < -0.005f) s_charging = false;
    s_trendRef = s_volts;
    s_trendT = now;
  }
  if (s_volts >= 4.17f) s_state = s_charging ? BAT_CHARGING : BAT_FULL;
  else s_state = s_charging ? BAT_CHARGING : BAT_DISCHARGING;

  // While charging the cell reads high, so take a little off before mapping
  const float rest = s_state == BAT_CHARGING ? s_volts - 0.08f : s_volts;
  const int p = curvePercent(rest);
  // Hysteresis so the number doesn't flicker between two values
  if (s_pct < 0 || abs(p - s_pct) >= 2 || p == 100 || p == 0) s_pct = p;
}

float volts() { return s_volts; }
int percent() { return s_pct; }
State state() { return s_state; }
bool low() { return s_pct >= 0 && s_pct <= 15 && s_state == BAT_DISCHARGING; }
bool critical() { return s_pct >= 0 && s_pct <= 5 && s_state == BAT_DISCHARGING; }

}  // namespace battery
