// Pocket Groove hardware spike -- microphone quality measurement of the
// current setup (SAMPLER page, key n; serial N). docs/hw-spike.md 4.
//
// Hands off for about 12 s, no stimulus timing:
//   1. for each gain preset: 0.3 s settle, 2 s of room silence -> noise floor
//      (RMS with DC removed, dBFS), peak, DC, and the averaged spectrum's
//      worst spur (2048-point Hann FFTs, spectrum.h);
//   2. at the current preset: the 1 kHz test tone on the speaker (-12 dBFS
//      digital, DAC -8 dB, as the mic matrix), 0.5 s settle, 1 s capture ->
//      level, harmonics 2-5, THD, noise without the harmonics, SINAD (tone.h
//      HarmonicDetector).
// The result goes into the SAMPLER block (quality / noise_gainN / tone /
// tone_noise lines).
//
// Threads: start() / tick() on the UI thread; the run in a job task on core 1
// (priority 5, I2C for the gain and volume, the FFTs); process() in the audio
// task. Buffers (~29 KB) are allocated for one run only.
#pragma once
#include <cstdint>

#include "report.h"

namespace hws {
namespace quality {

struct GainSet {
  uint8_t pga;      // reg 0x14 low nibble
  int     reg16;    // reg 0x16 (< 0: power-on value)
  int     pgaDb;
};

// gains[0..n-1] are measured in order (n <= kNumGainRows); userGain is
// restored afterwards and used for the tone; slot = I2S slot (0 L, 1 R).
bool start(const GainSet* gains, int n, int userGain, int slot);
bool running();
const char* statusLine();

// Audio task, while running(): owns the output (silence, then the tone).
void process(const int16_t* in, int16_t* out, int frames);

// UI, every loop: true once when a run has finished (result() is then new).
bool tick();
const MicQuality& result();

}  // namespace quality
}  // namespace hws
