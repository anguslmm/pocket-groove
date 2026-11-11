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
//   P1 INTRO    kick, hats, shaker; the riff low-passed (cutoff locks)
//   P2 BUILD    + clap/snare, open hats; the riff opens; snare roll in bar 4
//   P3 DROP     the full riff with timbre locks, organ stabs
//   P4 SWITCH   riff B (higher, jumpier), organ answer, hat ratchets
//   P5 BREAK    no kick: held organ chords, one long LFO growl per bar, clap build
//   P6 DROP 2   riff A/B alternating, extra perc, more organ
//   P7 OUTRO    kick, hats and the riff closing down
//   P8 TOOL     drums only (jam over it)
#include "demo_songs.h"
#include "demo_util.h"

namespace pg::demo {
namespace {

using namespace util;

constexpr int K = kTrackKick, SN = kTrackSnare, CP = kTrackClap, CH = kTrackHatClosed, OH = kTrackHatOpen,
              PC = kTrackPerc, BS = kTrackBass, LD = kTrackLead;
// bass sound params
constexpr int bCut = 0, bLfoRate = 7, bFmMix = 10, bFm = 11, bFold = 13, bLfoTimbre = 21;
constexpr int lSus = 10;

const uint8_t A = kStepAccent, S = kStepSlide;
// Riff A (two bars). F minor: F1 43.7 Hz .. F2 87.3 Hz, Eb2 77.8 Hz.
// The downbeats are left to the kick; each note's lock gives it its own colour.
const N kRiffA[] = {
    {2, F1, 112, A, 3, bFold, 96},         {3, F1, 90, 0, 1, bCut, 50},
    {5, F2, 104, S, 2, bFm, 110},          {6, Eb2, 100, 0, 2, -1, 0},
    {7, F1, 108, A, 2, bLfoRate, 9},       {10, F1, 112, 0, 6, bLfoTimbre, 120},
    {13, Ab1, 104, A | S, 2, bFold, 60},   {14, C2, 100, 0, 2, bFmMix, 127},
    {15, F1, 88, 0, 1, bCut, 44},
    {18, F1, 112, A, 3, bFold, 96},        {19, F1, 90, 0, 1, bCut, 50},
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
  at(P, K, 0).flags |= kStepAccent;
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
  // Growl: saw/pulse + FM pair (ratio 1) through the folder and ladder,
  // heavy drive, note-synced sine LFO on cutoff and timbre, big clean sub.
  params(p, BS, {{bCut, 34}, {1, 70}, {2, 30}, {3, 70}, {4, 60}, {5, 85}, {6, 62}, {bLfoRate, 7},
                 {8, 20}, {9, 90}, {bFmMix, 70}, {bFm, 70}, {12, 1}, {bFold, 60}, {14, 20}, {15, 1},
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

  // ---- patterns
  Pattern* P = p.patterns;
  for (int i = 0; i < 8; ++i) length(P[i], 64);
  intro(P[0]);
  build(P[1]);
  drop(P[2]);
  sw(P[3]);
  breakdown(P[4]);
  drop2(P[5]);
  outro(P[6]);
  tool(P[7]);

  // ---- song: 72 bars, about 2:17 at 126 BPM
  song(p, {{0, 2}, {1, 2}, {2, 4}, {3, 2}, {4, 2}, {5, 4}, {6, 2}});
  p.songMode = 1;
}

}  // namespace pg::demo
