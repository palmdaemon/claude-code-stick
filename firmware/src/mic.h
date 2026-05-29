#pragma once
#include <stdint.h>
#include <stddef.h>

// Microphone wrapper around M5.Mic. On StickS3 the ES8311 codec serves both
// playback and capture through one I2S bus, so the speaker MUST be stopped
// while recording (and vice versa). micBegin / micEnd manage that switch.

bool micBegin(uint32_t sample_rate = 16000);  // also calls Speaker.end()
void micEnd();                                // and restarts Speaker
bool micIsActive();

// ES8311 analog PGA gain (0..7). 0=0dB (default M5Unified), 7≈+30dB.
// Higher = louder + more noise. Re-applied on every micBegin().
void micSetPgaGain(uint8_t gain);
uint8_t micGetPgaGain();

// Blocking record of `samples` int16 frames at the configured sample rate.
// Returns true on success.
bool micRecord(int16_t* buf, size_t samples);

// Helpers for diagnostics.
struct MicStats {
  int16_t  minSample;
  int16_t  maxSample;
  uint32_t rms;          // sqrt(mean(x^2))
  uint32_t peak;         // max(|x|)
  uint32_t zeroCrossings;
};
MicStats micAnalyze(const int16_t* buf, size_t samples);
