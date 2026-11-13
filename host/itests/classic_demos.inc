// FROZEN COPY of the factory demos as they were before stage G (commit 25eaa9f:
// lib/pg/src/pg/demo/demo_util.h, demo_acidjam.cpp, demo_basshaus.cpp,
// demo_warehous.cpp), when every demo track was CLASSIC: bare sound parameters
// and one parameter lock per step. The firmware's demos use instruments since
// stage G (docs/STATE.md); this copy lives only in the tests, so that the
// CLASSIC sound path stays pinned by host/itests/itest_golden.cpp with the
// hashes taken before instruments existed. Do not edit: only the namespaces and
// the includes differ from the originals.
#pragma once
#include <cstdint>
#include <cstring>

#include "pg/engine.h"
#include "pg/project.h"

// Step-programming helpers shared by the factory demo songs (demo_*.cpp).
// Not real-time; used only while building a Project.


namespace pg::classic_demo::util {

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
  s.setFlags(kStepOn);
  s.vel = uint8_t(vel < 1 ? 1 : (vel > 127 ? 127 : vel));
  s.note = uint8_t(note);
  s.gate = uint8_t(gate < 1 ? 1 : gate);
  return s;
}
inline Step& acc(Step& s) { s.addFlags(kStepAccent); return s; }
inline Step& slide(Step& s) { s.addFlags(kStepSlide); return s; }
inline Step& prob(Step& s, int pct) { s.setProb(uint8_t(pct)); return s; }
inline Step& ratchet(Step& s, int hits) {
  s.setFlags(uint8_t((s.flags() & ~kStepRatchetMask) | (((hits - 1) & 3) << kStepRatchetShift)));
  return s;
}
inline Step& chord(Step& s, int c) { s.setChord(uint8_t(c)); return s; }
// One parameter lock: `param` is the track-param index (0..23 sound, 24..31 mixer).
inline Step& lock(Step& s, int param, int value) {
  s.setLock(uint8_t(param + 1), uint8_t(value));
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
  s.addFlags(uint8_t(n.flags & (kStepAccent | kStepSlide)));
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

}  // namespace pg::classic_demo::util

// ACIDJAM: acid house, A minor, 124 BPM, 54 % swing.
//
// 909-style kit, off-beat open hat, a squelchy 303 line (A minor pentatonic,
// octave jumps, slides, accents) whose cutoff is opened bar by bar with
// p-locks, Am9 / Dm9 stabs through a dotted-eighth delay, kick ducker on bass
// and stabs. Every pattern is four bars (64 steps).
//
//   P1 GROOVE  the full groove, Am9 / Dm9 stabs
//   P2 JACK    second acid line (more jumps), push stabs, snare fill in bar 4
//   P3 PEAK    acid wide open, accents, ratchets, every element in
//   P4 BREAK   no kick: held Am9 / Dm9 chords, filtered acid, snare-roll build
//   P5 BUILD   + clap, open hat, ghost snare; acid brighter; stabs creep in
//   P6 INTRO   kick, hats, rim; acid filtered, opening bar by bar
//   P7 OUTRO   kick, hats, rim and a closing acid line
//   P8 TOOL    kick + acid only (DJ-tool loop for jamming)

namespace pg::classic_demo::acid {
namespace {

using namespace util;

constexpr int K = kTrackKick, SN = kTrackSnare, CP = kTrackClap, CH = kTrackHatClosed, OH = kTrackHatOpen,
              PC = kTrackPerc, BS = kTrackBass, LD = kTrackLead;
constexpr int kCut = 0;      // bass + lead sound param 0 = cutoff
constexpr int kEnv = 2;      // bass env mod
constexpr int kLeadSus = 10; // lead sustain

// Two 303 lines, one bar each, A minor pentatonic (A C D E G) so they sit
// under both Am9 and Dm9. Fundamentals G1 (49 Hz) .. C3 (131 Hz).
// {step, note, vel, flags, gate, -, -}: locks are added per bar below.
const uint8_t A = kStepAccent, S = kStepSlide;
const N kLineA[] = {
    {0, A1, 100, 0, 2, -1, 0},  {1, A1, 78, A, 2, -1, 0},  {2, A2, 96, S, 2, -1, 0},   {3, G2, 90, 0, 2, -1, 0},
    {4, A1, 100, 0, 2, -1, 0},  {6, C2, 96, A | S, 2, -1, 0}, {7, A1, 88, 0, 2, -1, 0},
    {8, G1, 96, 0, 2, -1, 0},   {9, A1, 92, A, 2, -1, 0},  {10, A2, 96, S, 2, -1, 0},  {11, E2, 90, 0, 2, -1, 0},
    {12, A1, 100, 0, 2, -1, 0}, {13, A1, 80, 0, 2, -1, 0}, {14, C3, 96, A | S, 2, -1, 0}, {15, D2, 90, 0, 2, -1, 0},
};
const N kLineB[] = {
    {0, A1, 104, A, 2, -1, 0},  {2, A1, 86, 0, 2, -1, 0},  {3, A2, 96, S, 2, -1, 0},   {4, G2, 92, 0, 2, -1, 0},
    {5, A1, 84, 0, 2, -1, 0},   {7, E2, 100, A, 2, -1, 0}, {8, A1, 90, 0, 2, -1, 0},   {9, C3, 96, S, 2, -1, 0},
    {10, A2, 92, 0, 2, -1, 0},  {11, A1, 84, 0, 2, -1, 0}, {12, D2, 100, A | S, 2, -1, 0}, {13, E2, 90, 0, 2, -1, 0},
    {14, G2, 100, A, 2, -1, 0}, {15, A1, 86, S, 2, -1, 0},
};
// Sparse breakdown line: long slid notes.
const N kLineBreak[] = {
    {0, A1, 96, S, 2, -1, 0}, {3, A2, 90, 0, 6, -1, 0}, {6, G2, 90, S, 2, -1, 0}, {7, E2, 84, 0, 4, -1, 0},
    {10, A1, 96, A | S, 2, -1, 0}, {11, C2, 90, 0, 6, -1, 0}, {14, D2, 88, S, 2, -1, 0}, {15, E2, 84, 0, 2, -1, 0},
};

// Write one bar of a line at `bar`, `param` locked from a (first step) to b
// (last). Arrangement patterns lock the cutoff (filtered-then-opening); the
// jam patterns (P1-P3, P8) lock ENV instead so the bass CUT knob and the
// SHIFT+[ ] sweep stay live. param < 0: no locks.
template <int K2>
void acidBar(Pattern& P, int bar, const N (&line)[K2], int a, int b, int param = kCut) {
  for (const N& n : line) {
    Step& s = noteStep(P, BS, n, bar * 16);
    if (param >= 0) lock(s, param, a + (b - a) * n.step / 15);
  }
}

// Am9 (A3 C4 G4 B4) for two bars, then Dm9 (D4 F4 C5 E5): above the bass,
// below the hats. Stab rhythm per bar, gate 2 = an eighth-of-a-beat stab.
void stabs(Pattern& P, int bar, const int* steps, int n, int vel, int cutLock = -1) {
  const bool dm = (bar & 1) == 1;
  const int root = dm ? D4 : A3;
  for (int i = 0; i < n; ++i) {
    Step& s = chord(put(P, LD, bar * 16 + steps[i], i == 0 ? vel : vel - 10, root, 2), kChordMin9);
    if (cutLock >= 0) lock(s, kCut, cutLock);
  }
}
const int kStabMain[] = {3, 6, 10, 13};
const int kStabPush[] = {0, 3, 6, 11, 14};
const int kStabSparse[] = {6, 14};

// ---- drum layers (one-bar strips) ----------------------------------------
const char* kKick     = "x... x... x... x...";
const char* kKickFill = "x... x... x... x.go";
const char* kClap     = ".... x... .... x...";
const char* kClapFill = ".... x... .... x.g.";
const char* kHatC     = "ox.g ox.g ox.g ox.?";
const char* kHatCLow  = "o-.- o-.- o-.- o-.:";
const char* kHatO     = "..x. ..x. ..x. ..x.";
const char* kGhostSn  = ".... ...: .... .:.g";
const char* kRim      = "...g ..o. .g.. ?.o.";
const char* kRimBusy  = "..gg ..o. .g.o ?.o.";

void drumsBasic(Pattern& P, bool clap, bool oh, int hatScale = 100) {
  for (int b = 0; b < 4; ++b) {
    drums(P, K, b == 3 ? kKickFill : kKick, b * 16);
    drums(P, CH, kHatC, b * 16, hatScale);
    drums(P, PC, kRim, b * 16);
    if (clap) drums(P, CP, b == 3 ? kClapFill : kClap, b * 16);
    if (oh) drums(P, OH, kHatO, b * 16);
  }
  at(P, K, 0).addFlags(kStepAccent);
}

void intro(Pattern& P) {
  for (int b = 0; b < 4; ++b) {
    drums(P, K, kKick, b * 16);
    drums(P, CH, b < 2 ? kHatCLow : kHatC, b * 16);
    drums(P, PC, kRim, b * 16, 80);
    acidBar(P, b, kLineA, 16 + 6 * b, 22 + 6 * b);
  }
  drums(P, OH, "..o. ..o. ..o. ..x.", 48);   // open hats arrive in bar 4
  drums(P, SN, "2222 3333", 56, 50);         // soft roll into the next pattern
  ratchet(at(P, SN, 60), 2);
}

void build(Pattern& P) {
  drumsBasic(P, true, true);
  for (int b = 0; b < 4; ++b) {
    drums(P, SN, kGhostSn, b * 16);
    acidBar(P, b, kLineA, 36 + 4 * b, 42 + 4 * b);
  }
  // stabs creep in, filtered, in bars 3-4
  stabs(P, 2, kStabSparse, 2, 92, 40);
  stabs(P, 3, kStabMain, 4, 96, 52);
}

void groove(Pattern& P) {
  drumsBasic(P, true, true);
  for (int b = 0; b < 4; ++b) {
    drums(P, SN, kGhostSn, b * 16);
    acidBar(P, b, kLineA, 56 + 2 * b, 66 + 2 * b, kEnv);
    stabs(P, b, kStabMain, 4, 104);
  }
}

void jack(Pattern& P) {
  drumsBasic(P, true, true);
  for (int b = 0; b < 4; ++b) {
    drums(P, PC, kRimBusy, b * 16);
    acidBar(P, b, kLineB, 62 + 2 * b, 72 + 2 * b, kEnv);
    stabs(P, b, kStabPush, 5, 104);
  }
  drums(P, SN, "g... ..g. g.g. xxXX", 48);   // fill: 909 snare build
  ratchet(at(P, SN, 62), 2);
  ratchet(at(P, SN, 63), 3);
  // delay throw on the last stab
  lock(at(P, LD, 62), kMixDelaySend, 110);
}

void breakdown(Pattern& P) {
  // no kick, no clap: chords held (sustain locked up), reverb, filtered acid
  for (int b = 0; b < 4; ++b) {
    drums(P, CH, kHatCLow, b * 16, 80);
    acidBar(P, b, kLineBreak, 22 + 6 * b, 28 + 6 * b);
  }
  drums(P, OH, "..g. ..g. ..g. ..g.", 32);
  // Am9 2 bars, Dm9 2 bars, long gates, sustain locked high
  lock(chord(put(P, LD, 0, 96, A3, 60), kChordMin9), kLeadSus, 96);
  lock(chord(put(P, LD, 16, 84, A3, 28), kChordMin9), kLeadSus, 96);
  lock(chord(put(P, LD, 24, 80, A3, 28), kChordMin9), kLeadSus, 96);
  lock(chord(put(P, LD, 32, 96, D4, 60), kChordMin9), kLeadSus, 96);
  lock(chord(put(P, LD, 48, 88, D4, 20), kChordMin9), kLeadSus, 96);
  // snare roll build, bar 4
  drums(P, SN, "g.g. g.g. o.o. o.o.", 48, 80);
  drums(P, SN, "xxxx", 56, 90);
  for (int i = 56; i < 60; ++i) ratchet(at(P, SN, i), 2);
  drums(P, SN, "4444", 60, 100);
  drums(P, CP, "...X", 60);   // clap pickup into the drop
}

void peak(Pattern& P) {
  drumsBasic(P, true, true);
  for (int b = 0; b < 4; ++b) {
    drums(P, SN, kGhostSn, b * 16);
    drums(P, PC, kRimBusy, b * 16);
    if (b & 1) acidBar(P, b, kLineB, 76, 86, kEnv);
    else acidBar(P, b, kLineA, 70, 80, kEnv);
    stabs(P, b, kStabMain, 4, 108);
  }
  // hat ratchets at the end of bars 2 and 4
  ratchet(put(P, CH, 31, 90), 2);
  ratchet(put(P, CH, 63, 96), 3);
  lock(at(P, LD, 61), kMixDelaySend, 110);
}

void outro(Pattern& P) {
  for (int b = 0; b < 4; ++b) {
    drums(P, K, kKick, b * 16);
    drums(P, CH, kHatC, b * 16);
    drums(P, OH, kHatO, b * 16, b < 2 ? 90 : 60);
    drums(P, PC, kRim, b * 16);
    acidBar(P, b, kLineA, 50 - 7 * b, 44 - 7 * b);
  }
  lock(chord(put(P, LD, 3, 96, A3, 2), kChordMin9), kMixDelaySend, 120);   // one last delay throw
}

void tool(Pattern& P) {
  for (int b = 0; b < 4; ++b) {
    drums(P, K, kKick, b * 16);
    drums(P, CH, kHatCLow, b * 16);
    if (b & 1) acidBar(P, b, kLineB, 0, 0, -1);
    else acidBar(P, b, kLineA, 0, 0, -1);
  }
}

}  // namespace

void buildAcidJam(const Engine& engine, Project& p) {
  engine.initProject(p);
  name(p, "ACIDJAM");

  // ---- globals
  p.globals[kGTempo] = 124;
  p.globals[kGSwing] = 8;           // 54 %
  p.globals[kGDuckSource] = 1;      // kick
  p.globals[kGDuckDepth] = 100;
  p.globals[kGDuckHold] = 30;
  p.globals[kGDuckRelease] = 84;
  p.globals[kGDuckCurve] = 72;
  p.globals[kGDuckFx] = 70;
  p.globals[kGDelayTime] = 7;       // 1/8D
  p.globals[kGDelayFeedback] = 56;
  p.globals[kGDelayTone] = 54;
  p.globals[kGReverbSize] = 64;
  p.globals[kGReverbDamp] = 56;
  p.globals[kGMasterDrive] = 16;
  p.globals[kGScaleRoot] = 9;       // A
  p.globals[kGScaleType] = 2;       // minor

  // ---- sounds
  // 909-style kick tuned near A1 (55 Hz), a touch more punch and drive.
  params(p, K, {{0, 66}, {1, 50}, {2, 82}, {3, 56}, {4, 76}, {5, 38}, {6, 100}, {7, 32}, {8, 40}, {9, 56}});
  params(p, SN, {{0, 62}, {1, 50}, {2, 86}, {3, 74}, {4, 46}});
  params(p, CP, {{0, 58}, {1, 60}, {3, 78}});
  params(p, CH, {{1, 44}, {2, 84}});
  params(p, OH, {{1, 54}, {2, 74}});
  // TB-303-ish: saw, high resonance, big envelope, strong accent.
  params(p, BS, {{0, 52}, {1, 112}, {2, 66}, {3, 50}, {4, 104}, {5, 52}, {9, 34}, {16, 58}, {17, 96}, {20, 50}});
  // Saw stab: short, no sustain, a little drive.
  params(p, LD, {{0, 66}, {1, 28}, {2, 56}, {3, 46}, {6, 50}, {7, 54}, {10, 0}, {11, 40}, {15, 18}});

  // ---- mix: level, pan, delay, reverb, duck
  mix(p, K, 106, 64, 0, 0, 0);
  mix(p, SN, 88, 64, 0, 30, 0);
  mix(p, CP, 116, 64, 10, 46, 16);
  mix(p, CH, 94, 58, 0, 8, 0);
  mix(p, OH, 96, 70, 6, 20, 30);
  mix(p, PC, 92, 76, 26, 30, 30);
  mix(p, BS, 110, 64, 0, 0, 100);
  mix(p, LD, 106, 64, 70, 44, 84);

  // ---- patterns (P1 = the main groove: what plays when a new user presses play)
  Pattern* P = p.patterns;
  for (int i = 0; i < 8; ++i) length(P[i], 64);
  groove(P[0]);
  jack(P[1]);
  peak(P[2]);
  breakdown(P[3]);
  build(P[4]);
  intro(P[5]);
  outro(P[6]);
  tool(P[7]);

  // ---- song: 72 bars, about 2:19 at 124 BPM
  song(p, {{5, 2}, {4, 2}, {0, 2}, {1, 2}, {3, 2}, {2, 4}, {1, 2}, {6, 2}});
  p.songMode = 0;   // boots in loop mode on P1 (the main groove); SONG view plays the chain
}

}  // namespace pg::classic_demo::acid

// BASSHAUS: bass house, F minor, 126 BPM, 56 % swing.
//
// The star is a two-bar growl riff (saw + FM pair through the wavefolder and
// the diode ladder, drive, a note-synced sine LFO "wow" on the cutoff and the
// timbre) that steps off the kick. Almost every riff note carries a p-lock
// that changes the timbre for that note only: fold, FM depth, FM mix, LFO
// rate, LFO>timbre, cutoff. Tight punchy kick, clap layered with a short rim
// snare, shuffled hats, heavy ducking, sparse M1-style organ stabs.
// Every pattern is four bars (64 steps); the riff is two bars.
//
//   P1 DROP     the full riff with timbre locks, organ stabs
//   P2 SWITCH   riff B (higher, jumpier), organ answer, hat ratchets
//   P3 DROP 2   riff A/B alternating, extra perc, more organ
//   P4 BREAK    no kick: held organ chords, one long LFO growl per bar, clap build
//   P5 BUILD    + clap/snare, open hats; the riff opens; snare roll in bar 4
//   P6 INTRO    kick, hats, shaker; the riff low-passed (cutoff locks)
//   P7 OUTRO    kick, hats and the riff closing down
//   P8 TOOL     drums only (jam over it)

namespace pg::classic_demo::haus {
namespace {

using namespace util;

constexpr int K = kTrackKick, SN = kTrackSnare, CP = kTrackClap, CH = kTrackHatClosed, OH = kTrackHatOpen,
              PC = kTrackPerc, BS = kTrackBass, LD = kTrackLead;
// bass sound params
constexpr int bCut = 0, bEnv = 2, bLfoRate = 7, bFmMix = 10, bFm = 11, bFold = 13, bLfoTimbre = 21;
constexpr int lSus = 10;

const uint8_t A = kStepAccent, S = kStepSlide;
// Riff A (two bars). F minor: F1 43.7 Hz .. F2 87.3 Hz, Eb2 77.8 Hz.
// The downbeats are left to the kick; each note's lock gives it its own colour.
const N kRiffA[] = {
    {2, F1, 112, A, 3, bFold, 96},         {3, F1, 90, 0, 1, bEnv, 60},
    {5, F2, 104, S, 2, bFm, 110},          {6, Eb2, 100, 0, 2, -1, 0},
    {7, F1, 108, A, 2, bLfoRate, 9},       {10, F1, 112, 0, 6, bLfoTimbre, 120},
    {13, Ab1, 104, A | S, 2, bFold, 60},   {14, C2, 100, 0, 2, bFmMix, 127},
    {15, F1, 88, 0, 1, bEnv, 52},
    {18, F1, 112, A, 3, bFold, 96},        {19, F1, 90, 0, 1, bEnv, 60},
    {21, F2, 104, S, 2, bFm, 110},         {22, Eb2, 100, 0, 2, -1, 0},
    {23, C2, 108, A, 2, bLfoRate, 9},      {26, Ab1, 112, 0, 4, bLfoRate, 10},
    {28, Bb1, 100, S, 2, bFold, 120},      {29, C2, 104, 0, 2, bFm, 127},
    {30, Eb2, 108, A, 2, bFold, 127},      {31, F2, 96, 0, 1, bFmMix, 127},
};
// Riff B: jumpier, more octave work, LFO at 1/16 on the long notes.
const N kRiffB[] = {
    {2, F2, 112, A, 2, bFm, 120},          {3, F1, 96, 0, 2, bFold, 110},
    {6, F1, 104, 0, 2, bLfoRate, 9},       {7, F2, 104, A | S, 2, bFold, 80},
    {8, Eb2, 96, 0, 2, -1, 0},             {10, C2, 112, A, 4, bLfoRate, 10},
    {13, F1, 100, S, 2, bFmMix, 127},      {14, F2, 108, 0, 2, bFm, 127},
    {18, F2, 112, A, 2, bFm, 120},         {19, F1, 96, 0, 2, bFold, 110},
    {22, Ab1, 104, 0, 2, bLfoRate, 9},     {23, Ab1, 104, A | S, 2, bFold, 80},
    {24, Bb1, 96, 0, 2, -1, 0},            {26, C2, 112, A, 6, bLfoTimbre, 127},
    {29, Eb2, 104, S, 2, bFold, 127},      {30, F2, 108, A, 2, bFm, 127},
};

template <int K2>
void riff(Pattern& P, const N (&r)[K2], int offset, int cutLock = -1) {
  for (const N& n : r) {
    Step& s = noteStep(P, BS, n, offset);
    if (cutLock >= 0) lock(s, bCut, cutLock);
  }
}

// Organ stab: Fm7 (F3 Ab3 C4 Eb4) or Dbmaj7 (Db4 F4 Ab4 C5).
void organ(Pattern& P, int step, int vel, bool db = false, int gate = 2) {
  chord(put(P, LD, step, vel, db ? Db4 : F3, gate), db ? kChordMaj7 : kChordMin7);
}

// ---- drums
const char* kKick  = "x... x... x... x...";
const char* kKickE = "x... x... x... x..g";
const char* kClap  = ".... x... .... x...";
const char* kSnr   = ".... x... .... x...";
const char* kHatC  = "x-o- x-og x-o- x-o?";
const char* kHatO  = "..x. ..x. ..x. ..x.";
const char* kShkr  = "-g-g -g-g -g-g -gog";
const char* kPerc  = "...o .... ..o. .?..";

void drumsFull(Pattern& P, bool clap = true, bool oh = true) {
  for (int b = 0; b < 4; ++b) {
    drums(P, K, b == 3 ? kKickE : kKick, b * 16);
    drums(P, CH, kHatC, b * 16);
    if (clap) {
      drums(P, CP, kClap, b * 16);
      drums(P, SN, kSnr, b * 16, 80);
    }
    if (oh) drums(P, OH, kHatO, b * 16);
  }
  at(P, K, 0).addFlags(kStepAccent);
}

void intro(Pattern& P) {
  for (int b = 0; b < 4; ++b) {
    drums(P, K, kKick, b * 16);
    drums(P, CH, kHatC, b * 16, 85);
    drums(P, PC, kShkr, b * 16);
  }
  riff(P, kRiffA, 0, 30);
  riff(P, kRiffA, 32, 40);
  drums(P, OH, "..o. ..o. ..x. ..x.", 48);
}

void build(Pattern& P) {
  drumsFull(P);
  for (int b = 0; b < 4; ++b) drums(P, PC, kShkr, b * 16);
  riff(P, kRiffA, 0, 50);
  riff(P, kRiffA, 32);
  // snare roll, bar 4
  drums(P, SN, "o.o. o.o. xxxx 2233", 48);
  drums(P, CP, "...X", 60);
}

void drop(Pattern& P) {
  drumsFull(P);
  for (int b = 0; b < 4; ++b) drums(P, PC, kPerc, b * 16);
  riff(P, kRiffA, 0);
  riff(P, kRiffA, 32);
  organ(P, 11, 100);
  organ(P, 43, 100);
  organ(P, 59, 92, true);
}

void sw(Pattern& P) {
  drumsFull(P);
  for (int b = 0; b < 4; ++b) drums(P, PC, kPerc, b * 16);
  riff(P, kRiffB, 0);
  riff(P, kRiffB, 32);
  organ(P, 4, 96);
  organ(P, 7, 104);
  organ(P, 36, 96, true);
  organ(P, 39, 104, true);
  ratchet(put(P, CH, 31, 92), 2);
  ratchet(put(P, CH, 62, 90), 3);
  ratchet(put(P, CH, 63, 100), 4);
}

void breakdown(Pattern& P) {
  // held organ chords, sustain locked high
  lock(chord(put(P, LD, 0, 96, F3, 56), kChordMin7), lSus, 110);
  lock(chord(put(P, LD, 16, 88, Db4, 56), kChordMaj7), lSus, 110);
  lock(chord(put(P, LD, 32, 96, F3, 56), kChordMin7), lSus, 110);
  lock(chord(put(P, LD, 48, 92, Eb4, 40), kChordDom7), lSus, 110);   // Eb7 pulls back to Fm
  // one long growl per bar
  for (int b = 0; b < 4; ++b) {
    const int notes[4] = {F1, Db2, F1, Eb2};
    lock(put(P, BS, b * 16 + 2, 100, notes[b], 40), bLfoRate, b < 3 ? 7 : 9);
  }
  for (int b = 0; b < 4; ++b) drums(P, CH, "..-. ..-. ..-. ..-:", b * 16);
  drums(P, CP, "x... x... x... x...", 32, 70);
  drums(P, CP, "x.x. x.x. xxxx 2234", 48, 85);
}

void drop2(Pattern& P) {
  drumsFull(P);
  for (int b = 0; b < 4; ++b) {
    drums(P, PC, kPerc, b * 16);
  }
  riff(P, kRiffA, 0);
  riff(P, kRiffB, 32);
  organ(P, 11, 100);
  organ(P, 27, 96, true);
  organ(P, 43, 100);
  organ(P, 58, 96, true);
  organ(P, 61, 104, true);
  ratchet(put(P, CH, 63, 100), 3);
}

void outro(Pattern& P) {
  for (int b = 0; b < 4; ++b) {
    drums(P, K, kKick, b * 16);
    drums(P, CH, kHatC, b * 16, 90);
    drums(P, PC, kShkr, b * 16);
  }
  riff(P, kRiffA, 0, 46);
  riff(P, kRiffA, 32, 30);
  lock(chord(put(P, LD, 59, 96, F3, 3), kChordMin7), kMixDelaySend, 120);
}

void tool(Pattern& P) {
  drumsFull(P);
  for (int b = 0; b < 4; ++b) drums(P, PC, kShkr, b * 16);
}

}  // namespace

void buildBassHaus(const Engine& engine, Project& p) {
  engine.initProject(p);
  name(p, "BASSHAUS");

  // ---- globals
  p.globals[kGTempo] = 126;
  p.globals[kGSwing] = 12;          // 56 %
  p.globals[kGDuckSource] = 1;      // kick
  p.globals[kGDuckDepth] = 114;
  p.globals[kGDuckHold] = 40;
  p.globals[kGDuckRelease] = 80;
  p.globals[kGDuckCurve] = 70;
  p.globals[kGDuckFx] = 80;
  p.globals[kGDelayTime] = 5;       // 1/8
  p.globals[kGDelayFeedback] = 44;
  p.globals[kGDelayTone] = 60;
  p.globals[kGReverbSize] = 48;
  p.globals[kGReverbDamp] = 60;
  p.globals[kGMasterDrive] = 20;
  p.globals[kGScaleRoot] = 5;       // F
  p.globals[kGScaleType] = 2;       // minor

  // ---- sounds
  // Tight, clicky, driven kick (~50 Hz).
  params(p, K, {{0, 62}, {1, 40}, {2, 86}, {3, 48}, {4, 88}, {5, 56}, {6, 108}, {7, 26}, {8, 44}, {9, 60}});
  // Short rimmy snare: layered under the clap for snap.
  params(p, SN, {{0, 96}, {1, 22}, {2, 52}, {3, 96}, {4, 28}, {5, 30}, {6, 100}, {7, 76}, {8, 70}});
  params(p, CP, {{0, 72}, {1, 34}, {2, 44}, {3, 60}, {4, 3}, {6, 60}, {7, 64}});
  params(p, CH, {{1, 36}, {2, 88}, {5, 56}});
  params(p, OH, {{1, 40}, {2, 80}});
  params(p, PC, {{0, 4}});          // shaker
  // Growl: saw/pulse + FM pair (ratio 1) through the folder and ladder.
  // Drive 60 / sub 70 (not 85 / 90) so the CUT knob audibly sweeps it;
  // heavy drive, note-synced sine LFO on cutoff and timbre, big clean sub.
  params(p, BS, {{bCut, 34}, {1, 70}, {2, 30}, {3, 70}, {4, 60}, {5, 60}, {6, 62}, {bLfoRate, 7},
                 {8, 20}, {9, 70}, {bFmMix, 70}, {bFm, 70}, {12, 1}, {bFold, 60}, {14, 20}, {15, 1},
                 {17, 127}, {18, 40}, {20, 70}, {bLfoTimbre, 70}});
  // M1-style organ.
  params(p, LD, {{0, 104}, {1, 0}, {2, 24}, {3, 30}, {4, 7}, {5, 64}, {7, 60}, {lSus, 100}, {11, 16}, {18, 0},
                 {9, 90}});

  // ---- mix: level, pan, delay, reverb, duck
  mix(p, K, 108, 64, 0, 0, 0);
  mix(p, SN, 76, 64, 0, 16, 0);
  mix(p, CP, 112, 64, 0, 36, 10);
  mix(p, CH, 100, 58, 0, 6, 0);
  mix(p, OH, 92, 70, 0, 16, 40);
  mix(p, PC, 90, 76, 12, 20, 30);
  mix(p, BS, 102, 64, 0, 0, 120);
  mix(p, LD, 122, 64, 40, 40, 96);

  // ---- patterns (P1 = the main groove: what plays when a new user presses play)
  Pattern* P = p.patterns;
  for (int i = 0; i < 8; ++i) length(P[i], 64);
  drop(P[0]);
  sw(P[1]);
  drop2(P[2]);
  breakdown(P[3]);
  build(P[4]);
  intro(P[5]);
  outro(P[6]);
  tool(P[7]);

  // ---- song: 72 bars, about 2:17 at 126 BPM
  song(p, {{5, 2}, {4, 2}, {0, 4}, {1, 2}, {3, 2}, {2, 4}, {6, 2}});
  p.songMode = 0;   // boots in loop mode on P1 (the main groove); SONG view plays the chain
}

}  // namespace pg::classic_demo::haus

// WAREHOUS: driving techno, E minor, 133 BPM, straight.
//
// Hard driven kick tuned to E1, its reverb send (dark, long, ducked) as the
// rumble, a rolling bass (E2, 82 Hz) on the three 16ths after each kick whose cutoff
// creeps with p-locks, polymetric percussion (clave on 7 steps, rim-snare on
// 12, against 16-step hats), and an Em9 dub stab on a 12-step loop (3 against
// 4) through a long dotted-eighth delay, its cutoff opened stab by stab.
// Probability keeps ghost hits and hat ticks moving. Every pattern is four
// bars (64 steps); the polymetric tracks re-align at each pattern start.
//
//   P1 MAIN    + clap, dub stab (12), bass opens a little
//   P2 SHIFT   clave on 5, rim on 10, 16th hats with ratchets, bass to G2/D2
//   P3 PEAK    stab open, ghost kicks, everything, ratchet fills
//   P4 BREAK   no kick: stab sweep in the delay, bass rolling on (closed), hats
//   P5 BUILD   + rolling bass (closed), rim (12), open hats
//   P6 INTRO   kick + rumble, clave (7), hat ticks
//   P7 OUTRO   kick, rumble, clave, hats
//   P8 TOOL    kick, rumble and the rolling bass only

namespace pg::classic_demo::ware {
namespace {

using namespace util;

constexpr int K = kTrackKick, SN = kTrackSnare, CP = kTrackClap, CH = kTrackHatClosed, OH = kTrackHatOpen,
              PC = kTrackPerc, BS = kTrackBass, LD = kTrackLead;
constexpr int bCut = 0, bEnv = 2;
constexpr int lCut = 0, lEnv = 2;

// ---- drums
const char* kKick  = "x... x... x... x...";
const char* kKickG = "x... x... x... x.g.";
const char* kClap  = ".... x... .... x...";
const char* kHatT  = "..o. ..g: ..o. .:o.";   // off-beat ticks
const char* kHat16 = "-g-o -g-o -g-o -gxo";
const char* kHatO  = "..o. ..o. ..o. ..x.";

// Rolling bass: the three 16ths after every kick, `param` locked per beat from
// a to b. Arrangement patterns lock the cutoff; the jam patterns (P1-P3, P8)
// lock ENV so the bass CUT knob and the SHIFT+[ ] sweep stay live.
void roll(Pattern& P, int bar, int note, int cutA, int cutB, bool ghosts = true, int param = bCut) {
  for (int beat = 0; beat < 4; ++beat) {
    const int c = cutA + (cutB - cutA) * beat / 3;
    for (int k = 1; k < 4; ++k) {
      const int i = bar * 16 + beat * 4 + k;
      Step& s = put(P, BS, i, k == 1 ? 108 : (k == 2 ? 86 : 96), note, 2);
      lock(s, param, c + (k == 3 ? 6 : 0));
      if (k == 3 && beat == 3) s.addFlags(kStepAccent);
      if (ghosts && k == 2 && (beat & 1)) prob(s, 70);
    }
  }
}

// Em9 dub stab (E3 G3 D4 F#4) on a 12-step loop: steps 0 and 7, so the
// pair phases 3-against-4 over the bar. Cutoff locks per stab.
// Locks the lead's ENV (not CUT: the lead CUT knob stays live in jam patterns).
void stabLoop(Pattern& P, int envA, int envB) {
  P.tracks[LD].length = 12;
  lock(chord(put(P, LD, 0, 108, E3, 2), kChordMin9), lEnv, envA);
  lock(chord(put(P, LD, 7, 92, E3, 2), kChordMin9), lEnv, envB);
}

void percPoly(Pattern& P, int pcLen, const char* pc, int snLen, const char* sn) {
  P.tracks[PC].length = uint8_t(pcLen);
  drums(P, PC, pc, 0);
  if (snLen) {
    P.tracks[SN].length = uint8_t(snLen);
    drums(P, SN, sn, 0);
  }
}

void intro(Pattern& P) {
  for (int b = 0; b < 4; ++b) {
    drums(P, K, kKick, b * 16);
    drums(P, CH, kHatT, b * 16, b < 2 ? 70 : 100);
  }
  at(P, K, 0).addFlags(kStepAccent);
  percPoly(P, 7, "o..g.?.", 0, "");
}

void build(Pattern& P) {
  for (int b = 0; b < 4; ++b) {
    drums(P, K, b == 3 ? kKickG : kKick, b * 16);
    drums(P, CH, kHatT, b * 16);
    drums(P, OH, kHatO, b * 16, 70 + 10 * b);
    roll(P, b, E2, 14 + 3 * b, 18 + 3 * b);
  }
  percPoly(P, 7, "o..g.?.", 12, "..g. .:o. ..?.");
}

void mainP(Pattern& P) {
  for (int b = 0; b < 4; ++b) {
    drums(P, K, b == 3 ? kKickG : kKick, b * 16);
    drums(P, CH, kHat16, b * 16, 85);
    drums(P, OH, kHatO, b * 16);
    drums(P, CP, kClap, b * 16);
    roll(P, b, E2, 40 + 2 * b, 46 + 2 * b, true, bEnv);
  }
  percPoly(P, 7, "o..g.?.", 12, "..g. .:o. ..?.");
  stabLoop(P, 40, 50);
}

void shift(Pattern& P) {
  for (int b = 0; b < 4; ++b) {
    drums(P, K, kKick, b * 16);
    drums(P, CH, kHat16, b * 16);
    drums(P, OH, kHatO, b * 16);
    drums(P, CP, kClap, b * 16);
    roll(P, b, b < 2 ? E2 : (b == 2 ? G2 : D2), 48, 56, true, bEnv);
  }
  ratchet(at(P, CH, 31), 2);
  ratchet(at(P, CH, 63), 3);
  percPoly(P, 5, "o.g.?", 10, "..o. :.g. .?");
  stabLoop(P, 56, 66);
}

void breakdown(Pattern& P) {
  for (int b = 0; b < 4; ++b) drums(P, CH, b < 2 ? kHatT : kHat16, b * 16);
  drums(P, OH, "..g. ..g. ..o. ..o.", 32);
  drums(P, OH, "..o. ..o. ..x. ..x.", 48);
  percPoly(P, 7, "o..g.?.", 12, "..g. .:o. ..?.");
  // the rolling bass carries on, filter closed, opening in bar 4
  for (int b = 0; b < 4; ++b) roll(P, b, E2, b < 3 ? 12 : 16, b < 3 ? 16 : 30);
  // stab sweep: a 16-step track here, one stab per beat-and-a-half, cutoff rising
  P.tracks[LD].length = 64;
  const int st[] = {0, 7, 12, 19, 24, 31, 36, 43, 48, 55, 60};
  for (int i = 0; i < 11; ++i) lock(chord(put(P, LD, st[i], 104 + 2 * i, E3, 2), kChordMin9), lCut, 30 + 4 * i);
  // snare build into the drop
  drums(P, CP, "x... x... x.x. xxxx", 48, 80);
  ratchet(at(P, CP, 63), 3);
}

void peak(Pattern& P) {
  for (int b = 0; b < 4; ++b) {
    drums(P, K, kKickG, b * 16);
    drums(P, CH, kHat16, b * 16);
    drums(P, OH, kHatO, b * 16);
    drums(P, CP, kClap, b * 16);
    roll(P, b, E2, 56 + 3 * b, 62 + 3 * b, false, bEnv);
  }
  at(P, K, 0).addFlags(kStepAccent);
  percPoly(P, 7, "o..o.g?", 12, "..g. .oo. ..?g");
  stabLoop(P, 74, 86);
  ratchet(at(P, CH, 47), 2);
  ratchet(put(P, CP, 62, 90), 2);
  ratchet(put(P, CP, 63, 100), 4);
}

void outro(Pattern& P) {
  for (int b = 0; b < 4; ++b) {
    drums(P, K, kKick, b * 16);
    drums(P, CH, kHatT, b * 16, 100 - 10 * b);
  }
  percPoly(P, 7, "o..g.?.", 0, "");
  lock(chord(put(P, LD, 2, 100, E3, 2), kChordMin9), kMixDelaySend, 127);
}

void tool(Pattern& P) {
  for (int b = 0; b < 4; ++b) {
    drums(P, K, kKick, b * 16);
    roll(P, b, E2, 40, 46, true, bEnv);
  }
}

}  // namespace

void buildWarehous(const Engine& engine, Project& p) {
  engine.initProject(p);
  name(p, "WAREHOUS");

  // ---- globals
  p.globals[kGTempo] = 133;
  p.globals[kGSwing] = 0;
  p.globals[kGDuckSource] = 1;      // kick
  p.globals[kGDuckDepth] = 96;
  p.globals[kGDuckHold] = 24;
  p.globals[kGDuckRelease] = 72;
  p.globals[kGDuckCurve] = 64;
  p.globals[kGDuckFx] = 110;        // the rumble (kick reverb) pumps
  p.globals[kGDelayTime] = 7;       // 1/8D
  p.globals[kGDelayFeedback] = 84;
  p.globals[kGDelayTone] = 36;      // dark echoes
  p.globals[kGDelayWidth] = 127;
  p.globals[kGReverbSize] = 96;
  p.globals[kGReverbDamp] = 100;    // dark
  p.globals[kGMasterDrive] = 28;
  p.globals[kGScaleRoot] = 4;       // E
  p.globals[kGScaleType] = 2;       // minor

  // ---- sounds
  // Hard techno kick (~48 Hz, G1: the minor third), held and heavily driven.
  params(p, K, {{0, 54}, {1, 46}, {2, 94}, {3, 66}, {4, 84}, {5, 84}, {6, 84}, {7, 40}, {8, 60}, {9, 80}});
  // Short rim-snare for the 12-step layer.
  params(p, SN, {{0, 100}, {1, 14}, {2, 40}, {3, 100}, {4, 18}, {5, 20}, {6, 110}, {7, 80}, {8, 60}});
  params(p, CP, {{0, 64}, {1, 50}, {2, 70}, {3, 76}, {5, 60}, {6, 50}, {7, 50}});
  params(p, CH, {{0, 74}, {1, 30}, {2, 92}, {3, 100}, {5, 60}});
  params(p, OH, {{0, 70}, {1, 46}, {2, 84}});
  // Low woodblock clave for the 7-step loop.
  params(p, PC, {{0, 3}, {1, 22}, {2, 50}, {3, 96}, {5, 40}});
  // Rolling sub bass: saw + strong sub, short amp decay, low cutoff.
  params(p, BS, {{bCut, 32}, {1, 44}, {2, 40}, {3, 30}, {4, 70}, {5, 64}, {7, 2}, {9, 84},
                 {15, 1}, {17, 30}, {18, 20}, {20, 30}});
  // Dub stab: two detuned saws, resonant low-pass, plucky.
  params(p, LD, {{0, 40}, {1, 64}, {2, 44}, {3, 38}, {6, 44}, {7, 50}, {10, 0}, {11, 50}, {15, 30}});

  // ---- mix: level, pan, delay, reverb, duck
  mix(p, K, 106, 64, 0, 30, 0);
  mix(p, SN, 84, 50, 10, 30, 0);
  mix(p, CP, 104, 64, 16, 56, 20);
  mix(p, CH, 112, 60, 0, 10, 0);
  mix(p, OH, 94, 70, 0, 24, 30);
  mix(p, PC, 96, 80, 30, 30, 20);
  mix(p, BS, 100, 64, 0, 0, 110);
  mix(p, LD, 110, 64, 96, 50, 70);

  // ---- patterns (P1 = the main groove: what plays when a new user presses play)
  Pattern* P = p.patterns;
  for (int i = 0; i < 8; ++i) length(P[i], 64);
  mainP(P[0]);
  shift(P[1]);
  peak(P[2]);
  breakdown(P[3]);
  build(P[4]);
  intro(P[5]);
  outro(P[6]);
  tool(P[7]);

  // ---- song: 72 bars, about 2:10 at 133 BPM
  song(p, {{5, 2}, {4, 2}, {0, 3}, {1, 3}, {3, 2}, {2, 4}, {6, 2}});
  p.songMode = 0;   // boots in loop mode on P1 (the main groove); SONG view plays the chain
}

}  // namespace pg::classic_demo::ware
