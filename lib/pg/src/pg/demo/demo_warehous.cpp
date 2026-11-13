// WAREHOUS: driving techno, E minor, 133 BPM, straight.
//
// Hard driven kick (PUSH), its reverb send (dark, long, ducked) as the
// rumble, a rolling bass (POLES, E2, 82 Hz) on the three 16ths after each kick
// that moves by itself (TONE creeps open in the build and the break, TOUCH
// hardens beat by beat in the jam patterns), polymetric percussion (a TRAIL
// clave on 7 steps, a WOOD rim-snare on 12, against 16-step hats), and an Em9
// dub stab (HANDS) on a 12-step loop (3 against 4) through a long
// dotted-eighth delay, its PUSH moving stab by stab. Probability keeps ghost
// hits and hat ticks moving. Every pattern is four bars (64 steps); the
// polymetric tracks re-align at each pattern start. Instruments and macro
// positions were chosen by measurement against the CLASSIC version
// (host/tools/demo_compare, docs/STATE.md stage G).
//
//   P1 MAIN    + clap, dub stab (12), bass opens a little
//   P2 SHIFT   clave on 5, rim on 10, 16th hats with ratchets, bass to G2/D2
//   P3 PEAK    stab open, ghost kicks, everything, ratchet fills
//   P4 BREAK   no kick: stab sweep in the delay, bass rolling on (closed), hats
//   P5 BUILD   + rolling bass (closed), rim (12), open hats
//   P6 INTRO   kick + rumble, clave (7), hat ticks
//   P7 OUTRO   kick, rumble, clave, hats
//   P8 TOOL    kick, rumble and the rolling bass only
#include "demo_songs.h"
#include "demo_util.h"

namespace pg::demo {
namespace {

using namespace util;

constexpr int K = kTrackKick, SN = kTrackSnare, CP = kTrackClap, CH = kTrackHatClosed, OH = kTrackHatOpen,
              PC = kTrackPerc, BS = kTrackBass, LD = kTrackLead;
// macros: bass POLES (TONE TOUCH LENGTH DIRT), lead HANDS (OPEN PUSH SHAKE HOLD)
constexpr int kTone = 0, kTouch = 1;
constexpr int kOpen = 0, kPush = 1;

// ---- drums
const char* kKick  = "x... x... x... x...";
const char* kKickG = "x... x... x... x.g.";
const char* kClap  = ".... x... .... x...";
const char* kHatT  = "..o. ..g: ..o. .:o.";   // off-beat ticks
const char* kHat16 = "-g-o -g-o -g-o -gxo";
const char* kHatO  = "..o. ..o. ..o. ..x.";

// Rolling bass: the three 16ths after every kick, the notes' own value of
// `macro` going from a to b beat by beat (the third note of a beat a little
// further). Arrangement patterns move TONE (the CLASSIC demo's cutoff locks);
// the jam patterns (P1-P3, P8) move TOUCH (its ENV locks), so TONE and the
// SHIFT+[ ] sweep stay the player's.
void roll(Pattern& P, int bar, int note, int a, int b, bool ghosts = true, int macro = kTone) {
  const int lift = macro == kTone ? 18 : 16;
  for (int beat = 0; beat < 4; ++beat) {
    const int c = ramp(a, b, beat, 3);
    for (int k = 1; k < 4; ++k) {
      const int i = bar * 16 + beat * 4 + k;
      Step& s = put(P, BS, i, k == 1 ? 108 : (k == 2 ? 86 : 96), note, 2);
      mv(s, macro, c + (k == 3 ? lift : 0));
      if (k == 3 && beat == 3) s.addFlags(kStepAccent);
      if (ghosts && k == 2 && (beat & 1)) prob(s, 70);
    }
  }
}

// Em9 dub stab (E3 G3 D4 F#4) on a 12-step loop: steps 0 and 7, so the
// pair phases 3-against-4 over the bar. Each stab has its own PUSH (the CLASSIC
// demo's ENV locks; not OPEN: it stays the player's in the jam patterns).
void stabLoop(Pattern& P, int pushA, int pushB) {
  P.tracks[LD].length = 12;
  mv(chord(put(P, LD, 0, 108, E3, 2), kChordMin9), kPush, pushA);
  mv(chord(put(P, LD, 7, 92, E3, 2), kChordMin9), kPush, pushB);
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
    roll(P, b, E2, 9 + 9 * b, 21 + 9 * b);   // TONE creeps open
  }
  percPoly(P, 7, "o..g.?.", 12, "..g. .:o. ..?.");
}

void mainP(Pattern& P) {
  for (int b = 0; b < 4; ++b) {
    drums(P, K, b == 3 ? kKickG : kKick, b * 16);
    drums(P, CH, kHat16, b * 16, 85);
    drums(P, OH, kHatO, b * 16);
    drums(P, CP, kClap, b * 16);
    roll(P, b, E2, 151 + 6 * b, 167 + 6 * b, true, kTouch);   // TOUCH hardens beat by beat
  }
  percPoly(P, 7, "o..g.?.", 12, "..g. .:o. ..?.");
  stabLoop(P, 207, 217);
}

void shift(Pattern& P) {
  for (int b = 0; b < 4; ++b) {
    drums(P, K, kKick, b * 16);
    drums(P, CH, kHat16, b * 16);
    drums(P, OH, kHatO, b * 16);
    drums(P, CP, kClap, b * 16);
    roll(P, b, b < 2 ? E2 : (b == 2 ? G2 : D2), 163, 187, true, kTouch);
  }
  ratchet(at(P, CH, 31), 2);
  ratchet(at(P, CH, 63), 3);
  percPoly(P, 5, "o.g.?", 10, "..o. :.g. .?");
  stabLoop(P, 224, 234);
}

void breakdown(Pattern& P) {
  for (int b = 0; b < 4; ++b) drums(P, CH, b < 2 ? kHatT : kHat16, b * 16);
  drums(P, OH, "..g. ..g. ..o. ..o.", 32);
  drums(P, OH, "..o. ..o. ..x. ..x.", 48);
  percPoly(P, 7, "o..g.?.", 12, "..g. .:o. ..?.");
  // the rolling bass carries on, filter closed, opening in bar 4
  for (int b = 0; b < 4; ++b) roll(P, b, E2, b < 3 ? 3 : 15, b < 3 ? 15 : 57);
  // stab sweep: a 64-step track here, one stab per beat-and-a-half, OPEN rising
  P.tracks[LD].length = 64;
  const int st[] = {0, 7, 12, 19, 24, 31, 36, 43, 48, 55, 60};
  for (int i = 0; i < 11; ++i) mv(chord(put(P, LD, st[i], 104 + 2 * i, E3, 2), kChordMin9), kOpen, 1 + 12 * i);
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
    roll(P, b, E2, 167 + 8 * b, 191 + 8 * b, false, kTouch);
  }
  at(P, K, 0).addFlags(kStepAccent);
  percPoly(P, 7, "o..o.g?", 12, "..g. .oo. ..?g");
  stabLoop(P, 242, 255);
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
  chord(put(P, LD, 2, 100, E3, 2), kChordMin9);   // one last stab
}

void tool(Pattern& P) {
  for (int b = 0; b < 4; ++b) {
    drums(P, K, kKick, b * 16);
    roll(P, b, E2, 151, 167, true, kTouch);
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

  // ---- instruments (base macro positions, 0..255)
  instrument(p, K, "PUSH", 159, 119, 7);          // PUSH 62 %, HOLD 47 %: hard and held
  instrument(p, SN, "WOOD", 7, 0, 120);           // wood, piccolo, loose: the 12-step rim knock
  instrument(p, CP, "HYPE", 95, 0);
  instrument(p, CH, "TICK", 159, 47, 135);
  instrument(p, OH, "BLAST", 143, 47);
  instrument(p, PC, "TRAIL", 191, 7, 39);         // TRAIL 75 %: the low clave
  instrument(p, BS, "POLES", 63, 159, 63, 207);   // dark, hard, short, dirty: the rolling sub
  instrument(p, LD, "HANDS", 31, 231, 0, 71);     // nearly closed, pushed short, a little held: the dub stab

  // ---- mix: level, pan, delay, reverb, duck
  // (levels set so each track measures within 1.5 dB of the CLASSIC demo)
  mix(p, K, 122, 64, 0, 30, 0);
  mix(p, SN, 102, 50, 10, 30, 0);
  mix(p, CP, 118, 64, 16, 56, 20);
  mix(p, CH, 107, 60, 0, 10, 0);
  mix(p, OH, 87, 70, 0, 9, 30);
  mix(p, PC, 74, 80, 30, 30, 20);
  mix(p, BS, 103, 64, 0, 0, 110);
  mix(p, LD, 89, 64, 96, 50, 70);

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
  engine.sanitizeProject(p);   // derives the instruments' sound parameters
}

}  // namespace pg::demo
