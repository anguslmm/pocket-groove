// ACIDJAM: acid house, A minor, 124 BPM, 54 % swing.
//
// 909-style kit (TRIO at 909), off-beat open hat, a squelchy 303 line on ACID
// (A minor pentatonic, octave jumps, slides, accents) that moves by itself:
// in the arrangement patterns its notes open OPEN bar by bar, in the jam
// patterns (P1-P3) they swell BITE through each bar and leave OPEN to the
// player (and SHIFT+[ ]). Am9 / Dm9 stabs on HANDS through a dotted-eighth
// delay, kick ducker on bass and stabs. Every pattern is four bars (64 steps).
// Instruments and macro positions were chosen by measurement against the
// CLASSIC version (host/tools/demo_compare, docs/STATE.md stage G).
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
// macros: bass ACID (OPEN BITE WOBL SLIDE), lead HANDS (OPEN PUSH SHAKE HOLD)
constexpr int kOpen = 0, kBite = 1, kHold = 3;

// Two 303 lines, one bar each, A minor pentatonic (A C D E G) so they sit
// under both Am9 and Dm9. Fundamentals G1 (49 Hz) .. C3 (131 Hz).
// {step, note, vel, flags, gate, -}: the notes' own macro values are added per bar below.
const uint8_t A = kStepAccent, S = kStepSlide;
const N kLineA[] = {
    {0, A1, 100, 0, 2, 0},  {1, A1, 78, A, 2, 0},  {2, A2, 96, S, 2, 0},   {3, G2, 90, 0, 2, 0},
    {4, A1, 100, 0, 2, 0},  {6, C2, 96, A | S, 2, 0}, {7, A1, 88, 0, 2, 0},
    {8, G1, 96, 0, 2, 0},   {9, A1, 92, A, 2, 0},  {10, A2, 96, S, 2, 0},  {11, E2, 90, 0, 2, 0},
    {12, A1, 100, 0, 2, 0}, {13, A1, 80, 0, 2, 0}, {14, C3, 96, A | S, 2, 0}, {15, D2, 90, 0, 2, 0},
};
const N kLineB[] = {
    {0, A1, 104, A, 2, 0},  {2, A1, 86, 0, 2, 0},  {3, A2, 96, S, 2, 0},   {4, G2, 92, 0, 2, 0},
    {5, A1, 84, 0, 2, 0},   {7, E2, 100, A, 2, 0}, {8, A1, 90, 0, 2, 0},   {9, C3, 96, S, 2, 0},
    {10, A2, 92, 0, 2, 0},  {11, A1, 84, 0, 2, 0}, {12, D2, 100, A | S, 2, 0}, {13, E2, 90, 0, 2, 0},
    {14, G2, 100, A, 2, 0}, {15, A1, 86, S, 2, 0},
};
// Sparse breakdown line: long slid notes.
const N kLineBreak[] = {
    {0, A1, 96, S, 2, 0}, {3, A2, 90, 0, 6, 0}, {6, G2, 90, S, 2, 0}, {7, E2, 84, 0, 4, 0},
    {10, A1, 96, A | S, 2, 0}, {11, C2, 90, 0, 6, 0}, {14, D2, 88, S, 2, 0}, {15, E2, 84, 0, 2, 0},
};

// Write one bar of a line at `bar`, its notes' own value of `macro` going from
// a (first step) to b (last): the motion glides through the bar. Arrangement
// patterns move OPEN (filtered, then opening: the CLASSIC demo's cutoff locks);
// the jam patterns (P1-P3) move BITE instead (its ENV locks), so OPEN and the
// SHIFT+[ ] sweep stay the player's. macro < 0: no values (P8).
template <int K2>
void acidBar(Pattern& P, int bar, const N (&line)[K2], int a, int b, int macro = kOpen) {
  for (const N& n : line) {
    Step& s = noteStep(P, BS, n, bar * 16);
    if (macro >= 0) mv(s, macro, ramp(a, b, n.step, 15));
  }
}

// Am9 (A3 C4 G4 B4) for two bars, then Dm9 (D4 F4 C5 E5): above the bass,
// below the hats. Stab rhythm per bar, gate 2 = an eighth-of-a-beat stab.
// open >= 0: the stabs' own OPEN (filtered stabs creeping in).
void stabs(Pattern& P, int bar, const int* steps, int n, int vel, int open = -1) {
  const bool dm = (bar & 1) == 1;
  const int root = dm ? D4 : A3;
  for (int i = 0; i < n; ++i) {
    Step& s = chord(put(P, LD, bar * 16 + steps[i], i == 0 ? vel : vel - 10, root, 2), kChordMin9);
    if (open >= 0) mv(s, kOpen, open);
  }
}
const int kStabMain[] = {3, 6, 10, 13};
const int kStabPush[] = {0, 3, 6, 11, 14, 0};
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
    acidBar(P, b, kLineA, 47 + 13 * b, 60 + 13 * b);   // OPEN 18 % .. 52 %
  }
  drums(P, OH, "..o. ..o. ..o. ..x.", 48);   // open hats arrive in bar 4
  drums(P, SN, "2222 3333", 56, 50);         // soft roll into the next pattern
  ratchet(at(P, SN, 60), 2);
}

void build(Pattern& P) {
  drumsBasic(P, true, true);
  for (int b = 0; b < 4; ++b) {
    drums(P, SN, kGhostSn, b * 16);
    acidBar(P, b, kLineA, 91 + 9 * b, 104 + 9 * b);
  }
  // stabs creep in, filtered, in bars 3-4
  stabs(P, 2, kStabSparse, 2, 92, 25);
  stabs(P, 3, kStabMain, 4, 96, 62);
}

void groove(Pattern& P) {
  drumsBasic(P, true, true);
  for (int b = 0; b < 4; ++b) {
    drums(P, SN, kGhostSn, b * 16);
    acidBar(P, b, kLineA, 19 + 12 * b, 79 + 12 * b, kBite);   // BITE swells through each bar
    stabs(P, b, kStabMain, 4, 104);
  }
}

void jack(Pattern& P) {
  drumsBasic(P, true, true);
  for (int b = 0; b < 4; ++b) {
    drums(P, PC, kRimBusy, b * 16);
    acidBar(P, b, kLineB, 55 + 12 * b, 115 + 12 * b, kBite);
    stabs(P, b, kStabPush, 5, 104);
  }
  drums(P, SN, "g... ..g. g.g. xxXX", 48);   // fill: 909 snare build
  ratchet(at(P, SN, 62), 2);
  ratchet(at(P, SN, 63), 3);
}

void breakdown(Pattern& P) {
  // no kick, no clap: chords held (HOLD up), reverb, filtered acid
  for (int b = 0; b < 4; ++b) {
    drums(P, CH, kHatCLow, b * 16, 80);
    acidBar(P, b, kLineBreak, 60 + 13 * b, 74 + 13 * b);
  }
  drums(P, OH, "..g. ..g. ..g. ..g.", 32);
  // Am9 2 bars, Dm9 2 bars, long gates, held
  mv(chord(put(P, LD, 0, 96, A3, 60), kChordMin9), kHold, 215);
  mv(chord(put(P, LD, 16, 84, A3, 28), kChordMin9), kHold, 215);
  mv(chord(put(P, LD, 24, 80, A3, 28), kChordMin9), kHold, 215);
  mv(chord(put(P, LD, 32, 96, D4, 60), kChordMin9), kHold, 215);
  mv(chord(put(P, LD, 48, 88, D4, 20), kChordMin9), kHold, 215);
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
    if (b & 1) acidBar(P, b, kLineB, 139, 199, kBite);
    else acidBar(P, b, kLineA, 103, 163, kBite);
    stabs(P, b, kStabMain, 4, 108);
  }
  // hat ratchets at the end of bars 2 and 4
  ratchet(put(P, CH, 31, 90), 2);
  ratchet(put(P, CH, 63, 96), 3);
}

void outro(Pattern& P) {
  for (int b = 0; b < 4; ++b) {
    drums(P, K, kKick, b * 16);
    drums(P, CH, kHatC, b * 16);
    drums(P, OH, kHatO, b * 16, b < 2 ? 90 : 60);
    drums(P, PC, kRim, b * 16);
    acidBar(P, b, kLineA, 122 - 15 * b, 109 - 15 * b);   // OPEN closes bar by bar
  }
  chord(put(P, LD, 3, 96, A3, 2), kChordMin9);   // one last stab
}

void tool(Pattern& P) {
  for (int b = 0; b < 4; ++b) {
    drums(P, K, kKick, b * 16);
    drums(P, CH, kHatCLow, b * 16);
    if (b & 1) acidBar(P, b, kLineB, 0, 0, -1);   // no motion: the player's loop
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

  // ---- instruments (base macro positions, 0..255)
  instrument(p, K, "TRIO", 255, 63);              // 909 100 %, GABBA 25 %: a 909 with weight
  instrument(p, SN, "BITE", 127, 63, 16);         // PITCH 50 %, BITE 25 %, ROOM 6 %
  instrument(p, CP, "HYPE", 111, 31);             // MORE 44 %, CALM 12 %
  instrument(p, CH, "TICK", 143, 95, 127);        // a little bright, short, middle
  instrument(p, OH, "SWISH", 95, 0, 71);          // OPEN 37 %, no echo, SHIMR 28 %
  instrument(p, PC, "TRAIL", 95, 95, 47);         // TRAIL 37 %: the rim, just before the cowbell
  instrument(p, BS, "ACID", 127, 79, 0, 63);      // OPEN 50 %, BITE 31 %, SLIDE 25 %
  instrument(p, LD, "HANDS", 95, 207, 0, 0);      // OPEN 37 %, PUSH 81 %: a short saw stab

  // ---- mix: level, pan, delay, reverb, duck
  // (levels set so each track measures within 1.5 dB of the CLASSIC demo)
  mix(p, K, 117, 64, 0, 0, 0);
  mix(p, SN, 82, 64, 0, 52, 0);
  mix(p, CP, 109, 64, 10, 39, 16);
  mix(p, CH, 90, 58, 0, 8, 0);
  mix(p, OH, 97, 70, 6, 0, 30);
  mix(p, PC, 70, 76, 26, 30, 30);
  mix(p, BS, 119, 64, 0, 0, 100);
  mix(p, LD, 99, 64, 70, 44, 84);

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
  engine.sanitizeProject(p);   // derives the instruments' sound parameters
}

}  // namespace pg::demo
