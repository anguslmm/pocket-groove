// Band-limited wavetables for the lead voice (mip-mapped per half octave).
//
// Why tables and not PolyBLEP: a linear-interpolated int16 lookup is ~15
// cycles on the ESP32-S3 with no branches, about half a PolyBLEP saw, and the
// tables are strictly band-limited below Nyquist (no aliasing at all, only
// interpolation images, kept below about -54 dB by sizing each table to its
// harmonic count). That halves the oscillator cost, which is what pays for a
// 5-saw "super" stack and two stereo filters per voice.
//
// Storage: constant data in flash (.rodata), generated ahead of time by
// tools/gen/gen_tables.cpp into lead_tables_data.cpp: one table set per
// supported engine rate (44.1 and 32 kHz; the saw/triangle harmonic counts
// depend on Nyquist, the organ and sine tables are shared). No RAM at all.
// Regenerate after changing the formula in lead_tables_ref.cpp:
//   cmake --build build/host --target gen_tables && build/host/gen_tables lib/pg/src
// host/tests/test_lead_tables.cpp fails while the data and formula disagree.
#pragma once
#include <cstdint>

namespace pg {
namespace lead {

constexpr int   kNumBands      = 15;     // band b = floor((pitch - 36) / 6), clamped to 0..14
constexpr float kBandBasePitch = 36.0f;  // band 0 = everything below MIDI 42 (92.5 Hz)
constexpr float kTabAmp        = 24000.0f;  // table value of a unit-amplitude wave
constexpr int   kOrgRegs       = 6;      // organ drawbar registrations, mellow -> full
constexpr int   kOrgLevels     = 3;      // organ mips: partials up to 1', 2', 4'

// index = phase >> shift; frac = (phase & mask) * fscale; d has size+1 entries.
struct Tab {
  const int16_t* d;
  uint32_t shift;
  uint32_t mask;
  float fscale;
};

struct Tables {
  Tab saw[kNumBands];    // rising saw 2x-1, jump at phase 0
  Tab tri[kNumBands];    // triangle, fundamental in phase with the saw's
  Tab organ[kOrgRegs][kOrgLevels];  // one cycle = 16' pitch = TWO cycles of the played note
  Tab sine;              // 512 points
  float organPitch1, organPitch2;   // note pitch above which organ level 1 / 2 must be used
  float sampleRate;
};

// The constant table set for an engine sample rate: the 44.1 kHz set at
// fs >= 44100, otherwise the 32 kHz set (band-limited for 32 kHz, so it never
// aliases at 32..44.1 kHz; below 32 kHz the top mips would). Cheap, no state.
const Tables& tablesFor(float sampleRate);

// Generated data (lead_tables_data.cpp).
extern const Tables kTables44k;
extern const Tables kTables32k;

// ---- generator / test only ----
// The formula: fills `pool` (kRefPoolSize int16) and `out` for sampleRate.
// Not real-time (about 1M multiply-adds); the voice never calls it.
constexpr int kRefPoolSize = 12014;
void buildTablesReference(float sampleRate, int16_t* pool, Tables& out);

inline int bandForPitch(float pitch) {
  int b = int((pitch - kBandBasePitch) * (1.0f / 6.0f));
  if (pitch < kBandBasePitch) b = 0;
  return b < 0 ? 0 : (b > kNumBands - 1 ? kNumBands - 1 : b);
}

}  // namespace lead
}  // namespace pg
