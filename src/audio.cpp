#include "audio.h"
#include <ESP_I2S.h>
#include <Wire.h>
#include "config.h"

// ES8311 bring-up follows the register sequence of Espressif's es8311
// component (Apache-2.0), reduced to: slave mode, 16-bit I2S, MCLK from pin,
// MCLK = 256 * fs = 4.096 MHz, fs = 16 kHz, DAC path only.

namespace {

I2SClass s_i2s;
volatile bool s_enabled = true;
volatile bool s_ok = false;
volatile float s_geiger = -1.0f;
volatile uint32_t s_blipReq = 0;
volatile uint32_t s_alertReq = 0;

bool wr(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(ES8311_ADDR);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

uint8_t rd(uint8_t reg) {
  Wire.beginTransmission(ES8311_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return 0;
  if (Wire.requestFrom((uint16_t)ES8311_ADDR, (size_t)1) != 1) return 0;
  return Wire.read();
}

bool codecInit() {
  if (!wr(0x00, 0x1F)) return false;  // reset
  delay(20);
  wr(0x00, 0x00);
  wr(0x00, 0x80);  // power on
  wr(0x01, 0x3F);  // enable all clocks, MCLK from MCLK pin
  wr(0x06, rd(0x06) & ~0x20);  // SCLK not inverted
  // Clock dividers for 4.096 MHz / 16 kHz
  wr(0x02, (rd(0x02) & 0x07) | (0 << 5) | (0 << 3));  // pre_div=1, pre_mult=1x
  wr(0x03, 0x10);  // fs_mode single speed, adc_osr
  wr(0x04, 0x10);  // dac_osr
  wr(0x05, 0x00);  // adc_div=1, dac_div=1
  wr(0x06, (rd(0x06) & 0xE0) | (4 - 1));  // bclk_div=4
  wr(0x07, rd(0x07) & 0xC0);  // lrck_h
  wr(0x08, 0xFF);             // lrck_l
  // Format: slave, I2S, 16 bit in & out
  wr(0x00, rd(0x00) & 0xBF);
  wr(0x09, 0x0C);
  wr(0x0A, 0x0C);
  // Power up analog + DAC
  wr(0x0D, 0x01);
  wr(0x0E, 0x02);
  wr(0x12, 0x00);
  wr(0x13, 0x10);
  wr(0x1C, 0x6A);
  wr(0x37, 0x08);
  return true;
}

// ---- tiny synth ----
constexpr int BLOCK = 128;  // frames per write (8 ms)
int16_t s_buf[BLOCK * 2];

struct Voice {
  float phase = 0, freq = 0, freqSlide = 0, amp = 0, decay = 0;
  int samplesLeft = 0;
  void trigger(float f, float slide, float a, float durMs) {
    freq = f;
    freqSlide = slide;
    amp = a;
    samplesLeft = (int)(durMs * AUDIO_RATE / 1000.0f);
    decay = powf(0.001f, 1.0f / max(1, samplesLeft));
    phase = 0;
  }
  float next() {
    if (samplesLeft <= 0) return 0;
    samplesLeft--;
    phase += freq / AUDIO_RATE;
    if (phase >= 1.f) phase -= 1.f;
    freq += freqSlide;
    float v = (phase < 0.5f ? 1.f : -1.f) * amp;  // square = retro
    amp *= decay;
    return v;
  }
};

void synthTask(void*) {
  Voice tick, fx;
  uint32_t lastBlip = 0, lastAlert = 0;
  uint32_t alertStage = 0, alertNextSample = 0, sampleClock = 0;
  uint32_t silentBlocks = 0;
  bool ampOn = false;

  for (;;) {
    // Event intake
    if (s_blipReq != lastBlip) {
      lastBlip = s_blipReq;
      fx.trigger(1800, -4.0f, 0.25f, 25);
    }
    if (s_alertReq != lastAlert) {
      lastAlert = s_alertReq;
      alertStage = 1;
      alertNextSample = sampleClock;
    }
    const float g = s_geiger;
    const bool en = s_enabled;

    bool active = false;
    for (int i = 0; i < BLOCK; i++, sampleClock++) {
      // Alert = three rising chirps
      if (alertStage && sampleClock >= alertNextSample) {
        static const float kNotes[] = {880, 1320, 1760};
        fx.trigger(kNotes[alertStage - 1], 1.5f, 0.35f, 90);
        alertNextSample = sampleClock + AUDIO_RATE / 9;
        alertStage = (alertStage >= 3) ? 0 : alertStage + 1;
      }
      // Geiger: random clicks at a rate that scales with strength
      if (g >= 0) {
        // Poisson process like a real Geiger tube: P(click) = rate / fs
        const float rate = 0.8f + g * g * 30.0f;  // clicks per second
        const uint32_t threshold = (uint32_t)(rate / AUDIO_RATE * 4294967295.0f);
        if (tick.samplesLeft == 0 && esp_random() < threshold) {
          tick.trigger(300 + g * 1700, 0, 0.45f, 6);
        }
      }
      float v = (tick.next() + fx.next());
      if (!en) v = 0;
      if (v != 0) active = true;
      int16_t sv = (int16_t)(constrain(v, -1.f, 1.f) * 12000);
      s_buf[i * 2] = sv;
      s_buf[i * 2 + 1] = sv;
    }

    // Gate the speaker amp to kill idle hiss
    if (active) {
      silentBlocks = 0;
      if (!ampOn) {
        digitalWrite(PIN_AMP_EN, LOW);
        ampOn = true;
      }
    } else if (ampOn && ++silentBlocks > 250) {  // ~2 s
      digitalWrite(PIN_AMP_EN, HIGH);
      ampOn = false;
    }
    s_i2s.write((uint8_t*)s_buf, sizeof(s_buf));  // blocks -> paces the loop
  }
}

}  // namespace

namespace audio {

bool begin() {
  pinMode(PIN_AMP_EN, OUTPUT);
  digitalWrite(PIN_AMP_EN, HIGH);  // amp off until we have sound

  s_i2s.setPins(PIN_I2S_BCLK, PIN_I2S_WS, PIN_I2S_DOUT, PIN_I2S_DIN, PIN_I2S_MCLK);
  if (!s_i2s.begin(I2S_MODE_STD, AUDIO_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO)) {
    Serial.println("[audio] I2S init failed");
    return false;
  }
  if (!codecInit()) {
    Serial.println("[audio] ES8311 not responding");
    return false;
  }
  setVolume(70);
  s_ok = true;
  xTaskCreatePinnedToCore(synthTask, "synth", 4096, nullptr, 3, nullptr, 0);
  return true;
}

void setEnabled(bool on) { s_enabled = on; }
bool enabled() { return s_enabled; }

void setVolume(uint8_t v) {
  v = min<uint8_t>(v, 100);
  wr(0x32, v == 0 ? 0 : (uint8_t)(v * 256 / 100 - 1));
}

void blip() { s_blipReq = s_blipReq + 1; }
void alert() { s_alertReq = s_alertReq + 1; }
void setGeiger(float strength) { s_geiger = strength; }

}  // namespace audio
