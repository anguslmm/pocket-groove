// Step-programming helpers shared by the factory demo songs (demo_*.cpp).
// Not real-time; used only while building a Project.
#pragma once
#include <cstdint>
#include <cstring>

#include "../engine.h"
#include "../project.h"

namespace pg::demo::util {

// ---- notes ---------------------------------------------------------------
// MIDI numbers for the registers the demos use (C4 = 60).
enum Note : uint8_t {
  E1 = 28, F1 = 29, G1 = 31, Ab1 = 32, A1 = 33, Bb1 = 34, B1 = 35,
  C2 = 36, Db2 = 37, D2 = 38, Eb2 = 39, E2 = 40, F2 = 41, G2 = 43, Ab2 = 44, A2 = 45, Bb2 = 46, B2 = 47,
  C3 = 48, D3 = 50, Eb3 = 51, E3 = 52, F3 = 53, G3 = 55, A3 = 57, Bb3 = 58, B3 = 59,
  C4 = 60, Db4 = 61, D4 = 62, Eb4 = 63, E4 = 64, F4 = 65, G4 = 67, A4 = 69, B4 = 71, C5 = 72,
};

// ---- steps ---------------------------------------------------------------
inline Step& at(Pattern& P, int track, int i) { return P.tracks[track].steps[i & (kMaxSteps - 1)]; }

// Turn a step on (from kDefaultStep) and return it for chaining with the modifiers below.
inline Step& put(Pattern& P, int track, int i, int vel, int note = 60, int gate = 2) {
  Step& s = at(P, track, i);
  s = kDefaultStep;
  s.flags = kStepOn;
  s.vel = uint8_t(vel < 1 ? 1 : (vel > 127 ? 127 : vel));
  s.note = uint8_t(note);
  s.gate = uint8_t(gate < 1 ? 1 : gate);
  return s;
}
inline Step& acc(Step& s) { s.flags |= kStepAccent; return s; }
inline Step& slide(Step& s) { s.flags |= kStepSlide; return s; }
inline Step& prob(Step& s, int pct) { s.prob = uint8_t(pct); return s; }
inline Step& ratchet(Step& s, int hits) {
  s.flags = uint8_t((s.flags & ~kStepRatchetMask) | (((hits - 1) & 3) << kStepRatchetShift));
  return s;
}
inline Step& chord(Step& s, int c) { s.chord = uint8_t(c); return s; }
// One parameter lock: `param` is the track-param index (0..23 sound, 24..31 mixer).
inline Step& lock(Step& s, int param, int value) {
  s.lockParam = uint8_t(param + 1);
  s.lockValue = uint8_t(value);
  return s;
}

inline void clearTrack(Pattern& P, int track) {
  for (int i = 0; i < kMaxSteps; ++i) P.tracks[track].steps[i] = kDefaultStep;
}

// Master length and every track's length.
inline void length(Pattern& P, int steps) {
  P.length = uint8_t(steps);
  for (int t = 0; t < kNumTracks; ++t) P.tracks[t].length = uint8_t(steps);
}

// Drum strip from a string, one character per 16th starting at `start`;
// ' ' and '|' are ignored (readability). `scale` (percent) scales velocity.
//   X accent 122   x 106   o 86   g ghost 62   - whisper 44
//   ? 74 at 50 %   : 58 at 35 %   2/3/4 ratchet (96)   . rest (clears the step)
// Returns the step index after the last character.
inline int drums(Pattern& P, int track, const char* s, int start = 0, int scale = 100) {
  int i = start;
  for (const char* c = s; *c; ++c) {
    const char ch = *c;
    if (ch == ' ' || ch == '|') continue;
    auto v = [&](int base) { return base * scale / 100; };
    switch (ch) {
      case 'X': acc(put(P, track, i, v(122))); break;
      case 'x': put(P, track, i, v(106)); break;
      case 'o': put(P, track, i, v(86)); break;
      case 'g': put(P, track, i, v(62)); break;
      case '-': put(P, track, i, v(44)); break;
      case '?': prob(put(P, track, i, v(74)), 50); break;
      case ':': prob(put(P, track, i, v(58)), 35); break;
      case '2': case '3': case '4': ratchet(put(P, track, i, v(96)), ch - '0'); break;
      default: at(P, track, i) = kDefaultStep; break;
    }
    ++i;
  }
  return i;
}
// The same strip repeated `times` from step 0 (e.g. a one-bar strip over four bars).
inline void drumsRep(Pattern& P, int track, const char* s, int times, int scale = 100) {
  int i = 0;
  for (int k = 0; k < times; ++k) i = drums(P, track, s, i, scale);
}

// ---- synth lines -------------------------------------------------------------
// A note of a monophonic line or a stab. flags: kStepAccent / kStepSlide.
// lockP < 0 = no lock.
struct N {
  int step, note, vel;
  uint8_t flags;
  int gate;
  int lockP, lockV;
};
inline Step& noteStep(Pattern& P, int track, const N& n, int offset = 0, int transpose = 0) {
  Step& s = put(P, track, n.step + offset, n.vel, n.note + transpose, n.gate > 0 ? n.gate : 2);
  s.flags |= uint8_t(n.flags & (kStepAccent | kStepSlide));
  if (n.lockP >= 0) lock(s, n.lockP, n.lockV);
  return s;
}

// ---- project-level -------------------------------------------------------------
inline void name(Project& p, const char* n) {
  std::memset(p.name, 0, sizeof(p.name));
  std::strncpy(p.name, n, sizeof(p.name) - 1);
}
inline void param(Project& p, int track, int index, int value) { p.tracks[track].params[index] = uint8_t(value); }
struct PV { int index, value; };
template <int K>
inline void params(Project& p, int track, const PV (&v)[K]) {
  for (int i = 0; i < K; ++i) param(p, track, v[i].index, v[i].value);
}
// Mixer row for one track: level, pan, delay send, reverb send, duck amount.
inline void mix(Project& p, int track, int level, int pan, int dly, int rev, int duck) {
  uint8_t* m = p.tracks[track].params;
  m[kMixLevel] = uint8_t(level);
  m[kMixPan] = uint8_t(pan);
  m[kMixDelaySend] = uint8_t(dly);
  m[kMixReverbSend] = uint8_t(rev);
  m[kMixDuck] = uint8_t(duck);
}
struct Slot { int pattern, repeats; };
template <int K>
inline void song(Project& p, const Slot (&s)[K]) {
  static_assert(K < kSongSlots, "song too long");
  for (int i = 0; i < kSongSlots; ++i) p.song[i] = SongSlot{0, 0};
  for (int i = 0; i < K; ++i) p.song[i] = SongSlot{uint8_t(s[i].pattern), uint8_t(s[i].repeats)};
}

}  // namespace pg::demo::util
