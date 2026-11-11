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
#include "demo_songs.h"
#include "demo_util.h"

namespace pg::demo {
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
  at(P, K, 0).flags |= kStepAccent;
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

}  // namespace pg::demo
