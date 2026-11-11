// Scale table for kGScaleType / kGScaleRoot. Header-only (constexpr data), so
// the UI can include it without linking the engine library.
// `mask` bit n set = semitone n above the root belongs to the scale.
#pragma once
#include <cstdint>

namespace pg {
namespace engine_detail {

struct ScaleDef {
  const char* name;      // <= 4 chars, matches the kGScaleType enum names
  const char* longName;
  uint16_t mask;
  uint8_t count;         // notes per octave
};

constexpr int kNumScales = 10;
constexpr ScaleDef kScales[kNumScales] = {
    {"CHRO", "Chromatic", 0x0FFF, 12},
    {"MAJ", "Major", 0x0AB5, 7},        // 0 2 4 5 7 9 11
    {"MIN", "Minor", 0x05AD, 7},        // 0 2 3 5 7 8 10
    {"DOR", "Dorian", 0x06AD, 7},       // 0 2 3 5 7 9 10
    {"PHRY", "Phrygian", 0x05AB, 7},    // 0 1 3 5 7 8 10
    {"MIXO", "Mixolydian", 0x06B5, 7},  // 0 2 4 5 7 9 10
    {"MPEN", "Min Penta", 0x04A9, 5},   // 0 3 5 7 10
    {"JPEN", "Maj Penta", 0x0295, 5},   // 0 2 4 7 9
    {"BLUE", "Blues", 0x04E9, 6},       // 0 3 5 6 7 10
    {"HMIN", "Harm Minor", 0x09AD, 7},  // 0 2 3 5 7 8 11
};

// Semitone offset (may exceed 11) of scale degree `degree` (0-based, may exceed count).
constexpr int scaleDegreeToSemitone(int scale, int degree) {
  const ScaleDef& s = kScales[scale < 0 || scale >= kNumScales ? 0 : scale];
  int oct = degree / s.count, d = degree % s.count;
  if (d < 0) { d += s.count; --oct; }
  int semi = 0;
  for (int n = 0, seen = 0; n < 12; ++n) {
    if (s.mask & (1 << n)) {
      if (seen == d) { semi = n; break; }
      ++seen;
    }
  }
  return oct * 12 + semi;
}

}  // namespace engine_detail
}  // namespace pg
