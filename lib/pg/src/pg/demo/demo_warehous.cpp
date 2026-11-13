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
#include "demo_songs.h"
#include "demo_util.h"

namespace pg::demo {
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

}  // namespace pg::demo
