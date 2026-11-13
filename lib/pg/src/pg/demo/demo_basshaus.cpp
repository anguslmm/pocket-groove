// BASSHAUS: bass house, F minor, 126 BPM, 56 % swing.
//
// The star is a two-bar growl riff on ROAD (between its HOLLOW and GROWL
// stations: square and FM pair, a tempo-synced LFO "wow" on the cutoff and
// the timbre from MOVE) that steps off the kick. Almost every riff note
// carries its own DIRT and MOVE, so the timbre changes from note to note
// (the CLASSIC demo's per-note locks of fold, FM, LFO rate and LFO>timbre)
// and glides between them; ROAD itself stays the player's in the jam
// patterns and moves only in the intro, build and outro. Tight punchy kick
// (BOOM), clap (HYPE) layered with a short wood snare (WOOD), shuffled hats,
// heavy ducking, sparse M1-style organ stabs (KEYS).
// Every pattern is four bars (64 steps); the riff is two bars. Instruments
// and macro positions were chosen by measurement against the CLASSIC version
// (host/tools/demo_compare, docs/STATE.md stage G).
//
//   P1 DROP     the full riff, every note its own colour, organ stabs
//   P2 SWITCH   riff B (higher, jumpier), organ answer, hat ratchets
//   P3 DROP 2   riff A/B alternating, extra perc, more organ
//   P4 BREAK    no kick: held organ chords, one long LFO growl per bar, clap build
//   P5 BUILD    + clap/snare, open hats; the riff opens; snare roll in bar 4
//   P6 INTRO    kick, hats, a ticking perc; the riff plain, ROAD opening
//   P7 OUTRO    kick, hats and the riff closing down
//   P8 TOOL     drums only (jam over it)
#include "demo_songs.h"
#include "demo_util.h"

namespace pg::demo {
namespace {

using namespace util;

constexpr int K = kTrackKick, SN = kTrackSnare, CP = kTrackClap, CH = kTrackHatClosed, OH = kTrackHatOpen,
              PC = kTrackPerc, BS = kTrackBass, LD = kTrackLead;
// macros: bass ROAD (ROAD DIRT MOVE)
constexpr int kRoad = 0, kDirt = 1, kMove = 2;
constexpr int kRoadBase = 140, kDirtBase = 40, kMoveBase = 120;   // the instrument page's positions

// A riff note's colour: its own DIRT and MOVE. MOVE past 70 % switches the
// LFO to 1/8T, past 90 % to 1/16 (ROAD's rate steps).
enum Tone { kPlain = 0, kGrit, kFold, kSnap, kFm, kTrip, kSixt, kWow };
struct Colour { uint8_t dirt, move; };
const Colour kColour[] = {
    {kDirtBase, kMoveBase},   // plain
    {130, kMoveBase},         // grit: driven (was a light fold lock)
    {220, kMoveBase},         // fold: driven hard (a heavy fold lock)
    {100, kMoveBase},         // snap: more envelope and drive (an ENV lock)
    {60, 170},                // fm: the LFO deeper into the FM index and fold (FM depth / mix locks)
    {kDirtBase, 191},         // trip: LFO at 1/8T (an LFO rate lock)
    {kDirtBase, 235},         // sixt: LFO at 1/16
    {80, 180},                // wow: the LFO deep into the timbre (an LFO>timbre lock)
};

const uint8_t A = kStepAccent, S = kStepSlide;
// Riff A (two bars). F minor: F1 43.7 Hz .. F2 87.3 Hz, Eb2 77.8 Hz.
// The downbeats are left to the kick; each note's tone gives it its own colour.
const N kRiffA[] = {
    {2, F1, 112, A, 3, kFold},         {3, F1, 90, 0, 1, kSnap},
    {5, F2, 104, S, 2, kFm},           {6, Eb2, 100, 0, 2, kPlain},
    {7, F1, 108, A, 2, kTrip},         {10, F1, 112, 0, 6, kWow},
    {13, Ab1, 104, A | S, 2, kGrit},   {14, C2, 100, 0, 2, kFm},
    {15, F1, 88, 0, 1, kSnap},
    {18, F1, 112, A, 3, kFold},        {19, F1, 90, 0, 1, kSnap},
    {21, F2, 104, S, 2, kFm},          {22, Eb2, 100, 0, 2, kPlain},
    {23, C2, 108, A, 2, kTrip},        {26, Ab1, 112, 0, 4, kSixt},
    {28, Bb1, 100, S, 2, kFold},       {29, C2, 104, 0, 2, kFm},
    {30, Eb2, 108, A, 2, kFold},       {31, F2, 96, 0, 1, kFm},
};
// Riff B: jumpier, more octave work, LFO at 1/16 on the long notes.
const N kRiffB[] = {
    {2, F2, 112, A, 2, kFm},           {3, F1, 96, 0, 2, kFold},
    {6, F1, 104, 0, 2, kTrip},         {7, F2, 104, A | S, 2, kGrit},
    {8, Eb2, 96, 0, 2, kPlain},        {10, C2, 112, A, 4, kSixt},
    {13, F1, 100, S, 2, kFm},          {14, F2, 108, 0, 2, kFm},
    {18, F2, 112, A, 2, kFm},          {19, F1, 96, 0, 2, kFold},
    {22, Ab1, 104, 0, 2, kTrip},       {23, Ab1, 104, A | S, 2, kGrit},
    {24, Bb1, 96, 0, 2, kPlain},       {26, C2, 112, A, 6, kWow},
    {29, Eb2, 104, S, 2, kFold},       {30, F2, 108, A, 2, kFm},
};

// colours: every note takes its tone's DIRT and MOVE. road >= 0: every note's
// own ROAD (the arrangement's opening and closing; the CLASSIC demo's cutoff
// locks, which also replaced the colours there).
template <int K2>
void riff(Pattern& P, const N (&r)[K2], int offset, bool colours, int road = -1) {
  for (const N& n : r) {
    Step& s = noteStep(P, BS, n, offset);
    if (colours) {
      mv(s, kDirt, kColour[n.tone].dirt);
      mv(s, kMove, kColour[n.tone].move);
    }
    if (road >= 0) mv(s, kRoad, road);
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
  riff(P, kRiffA, 0, false, 128);   // a little darker, plain
  riff(P, kRiffA, 32, false, 158);  // opening
  drums(P, OH, "..o. ..o. ..x. ..x.", 48);
}

void build(Pattern& P) {
  drumsFull(P);
  for (int b = 0; b < 4; ++b) drums(P, PC, kShkr, b * 16);
  riff(P, kRiffA, 0, false, 188);           // open and plain ...
  riff(P, kRiffA, 32, true, kRoadBase);     // ... then the colours arrive
  // snare roll, bar 4
  drums(P, SN, "o.o. o.o. xxxx 2233", 48);
  drums(P, CP, "...X", 60);
}

void drop(Pattern& P) {
  drumsFull(P);
  for (int b = 0; b < 4; ++b) drums(P, PC, kPerc, b * 16);
  riff(P, kRiffA, 0, true);
  riff(P, kRiffA, 32, true);
  organ(P, 11, 100);
  organ(P, 43, 100);
  organ(P, 59, 92, true);
}

void sw(Pattern& P) {
  drumsFull(P);
  for (int b = 0; b < 4; ++b) drums(P, PC, kPerc, b * 16);
  riff(P, kRiffB, 0, true);
  riff(P, kRiffB, 32, true);
  organ(P, 4, 96);
  organ(P, 7, 104);
  organ(P, 36, 96, true);
  organ(P, 39, 104, true);
  ratchet(put(P, CH, 31, 92), 2);
  ratchet(put(P, CH, 62, 90), 3);
  ratchet(put(P, CH, 63, 100), 4);
}

void breakdown(Pattern& P) {
  // held organ chords (KEYS' organ holds by itself)
  chord(put(P, LD, 0, 96, F3, 56), kChordMin7);
  chord(put(P, LD, 16, 88, Db4, 56), kChordMaj7);
  chord(put(P, LD, 32, 96, F3, 56), kChordMin7);
  chord(put(P, LD, 48, 92, Eb4, 40), kChordDom7);   // Eb7 pulls back to Fm
  // one long growl per bar, the last at 1/8T (played hard: ROAD lets a long note decay
  // where the CLASSIC growl held, and the harder start keeps the bar as loud)
  for (int b = 0; b < 4; ++b) {
    const int notes[4] = {F1, Db2, F1, Eb2};
    mv(put(P, BS, b * 16 + 2, 114, notes[b], 40), kMove, b < 3 ? kMoveBase : kColour[kTrip].move);
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
  riff(P, kRiffA, 0, true);
  riff(P, kRiffB, 32, true);
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
  riff(P, kRiffA, 0, false, 176);   // closing down
  riff(P, kRiffA, 32, false, 128);
  chord(put(P, LD, 59, 96, F3, 3), kChordMin7);
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

  // ---- instruments (base macro positions, 0..255)
  instrument(p, K, "BOOM", 87, 159, 215, 0);      // small-ish, loose skin, toward metal, near: clicky, driven, dry
  instrument(p, SN, "WOOD", 79, 0, 63);           // wood, piccolo: a short knock under the clap
  instrument(p, CP, "HYPE", 135, 0);              // MORE 53 %
  instrument(p, CH, "TICK", 151, 63, 111);
  instrument(p, OH, "BLAST", 127, 23);
  instrument(p, PC, "TRAIL", 230, 0, 255);        // the zap end, high and dirty: a ticking texture (was a shaker)
  instrument(p, BS, "ROAD", kRoadBase, kDirtBase, kMoveBase);   // ROAD 55 %: past HOLLOW toward GROWL
  instrument(p, LD, "KEYS", 0, 0);                // the organ

  // ---- mix: level, pan, delay, reverb, duck
  // (levels set so each track measures within 1.5 dB of the CLASSIC demo)
  mix(p, K, 119, 64, 0, 0, 0);
  mix(p, SN, 99, 64, 0, 16, 0);
  mix(p, CP, 115, 64, 0, 36, 10);
  mix(p, CH, 96, 58, 0, 6, 0);
  mix(p, OH, 84, 70, 0, 9, 40);
  mix(p, PC, 42, 76, 12, 20, 30);
  mix(p, BS, 104, 64, 0, 0, 120);
  mix(p, LD, 117, 64, 40, 40, 96);

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
  engine.sanitizeProject(p);   // derives the instruments' sound parameters
}

}  // namespace pg::demo
