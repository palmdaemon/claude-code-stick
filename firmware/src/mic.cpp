#include "mic.h"

#include <M5Unified.h>
#include <math.h>

static bool     _active      = false;
static uint32_t _sample_rate = 16000;
// PGA gain default ramped to mid-high — StickS3 mic + ES8311 needs boost
// for distant speech. M5Unified's hardcoded 0dB is too quiet for ASR.
static uint8_t  _pga_gain    = 5;

void micSetPgaGain(uint8_t gain) {
  if (gain > 7) gain = 7;
  _pga_gain = gain;
}
uint8_t micGetPgaGain() { return _pga_gain; }

bool micBegin(uint32_t sample_rate) {
  if (_active) return true;
  _sample_rate = sample_rate;
  // Speaker and Mic share the ES8311 I2S bus on StickS3; can't run both.
  M5.Speaker.end();

  // Override the default magnification=16 (intended for PDM mics) — the
  // ES8311 codec's ADC volume is already at MAXGAIN, so 16x multiplier
  // saturates every sample to INT16 limits. magnification=1 = no multiply.
  auto cfg = M5.Mic.config();
  cfg.sample_rate   = _sample_rate;
  cfg.magnification = 1;  // default 16 saturates with ES8311 ADC at MAXGAIN
  cfg.input_channel = m5::input_channel_t::input_only_left;
  M5.Mic.config(cfg);

  bool beginOk = M5.Mic.begin();
  if (!beginOk) {
    M5.Speaker.begin();
    return false;
  }
  // Override ES8311 PGA gain (reg 0x14). M5Unified hardcodes 0x10 (input
  // Mic1P-Mic1N + 0dB gain). Bits 2-0 add PGA gain.
  delay(5);  // let ES8311 settle after Mic.begin's I2C writes
  uint8_t val = 0x10 | (_pga_gain & 0x07);
  M5.In_I2C.writeRegister8(0x18, 0x14, val, 100000);
  Serial.printf("[mic] PGA gain set to %u (reg 0x14=0x%02X)\n", _pga_gain, val);

  _active = M5.Mic.isEnabled();
  if (!_active) {
    M5.Mic.end();
    M5.Speaker.begin();
  }
  return _active;
}

void micEnd() {
  if (!_active) return;
  M5.Mic.end();
  M5.Speaker.begin();
  M5.Speaker.setVolume(128);
  _active = false;
}

bool micIsActive() { return _active; }

bool micRecord(int16_t* buf, size_t samples) {
  if (!_active || !buf) return false;
  if (!M5.Mic.record(buf, samples, _sample_rate)) return false;
  uint32_t expected_ms = (samples * 1000U + _sample_rate - 1) / _sample_rate;
  uint32_t deadline    = millis() + expected_ms + 500;
  // M5.Mic.record() queues async; the recording task takes ~50ms to start and
  // flip _is_recording=true. Wait through that race before polling.
  delay(50);
  while (M5.Mic.isRecording() > 0) {
    if ((int32_t)(millis() - deadline) > 0) return false;
    delay(5);
  }
  return true;
}

MicStats micAnalyze(const int16_t* buf, size_t samples) {
  MicStats s{INT16_MAX, INT16_MIN, 0, 0, 0};
  if (!buf || samples == 0) return s;
  int64_t sumSq = 0;
  uint32_t peak = 0;
  uint32_t zc = 0;
  int16_t prev = 0;
  for (size_t i = 0; i < samples; i++) {
    int16_t v = buf[i];
    if (v < s.minSample) s.minSample = v;
    if (v > s.maxSample) s.maxSample = v;
    uint32_t mag = (uint32_t)(v < 0 ? -v : v);
    if (mag > peak) peak = mag;
    sumSq += (int32_t)v * v;
    if (i > 0 && ((prev >= 0) != (v >= 0))) zc++;
    prev = v;
  }
  s.rms  = (uint32_t)sqrtf((float)sumSq / (float)samples);
  s.peak = peak;
  s.zeroCrossings = zc;
  return s;
}
