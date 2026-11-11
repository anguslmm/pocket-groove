// Audition renders for the lead voice -> build/renders/lead/*.wav
// (this is how the project "listens": a human plays these back later, and
//  host/tests/lead_inspect.py turns them into spectrograms and level stats).
#include "test_lead_util.h"

using namespace lt;
using namespace pg::lead;

namespace {

const char* kDir = "build/renders/lead";

// Minimal step sequencer: times in 16th-note steps.
struct Seq {
  Synth s;
  double stepSec;
  struct Ev {
    double t;
    int kind;  // 0 = param, 1 = note off, 2 = note on
    int a, b, c;
    bool accent, slide;
  };
  std::vector<Ev> evs;

  explicit Seq(double bpm, float fs = 44100.0f) : s(fs), stepSec(60.0 / bpm / 4.0) {}
  void set(int idx, int val) { s.set(idx, val); }
  void param(double step, int idx, int val) { evs.push_back({step * stepSec, 0, idx, val, 0, false, false}); }
  // Linear param ramp, one event per quarter step.
  void ramp(double step0, double step1, int idx, int v0, int v1) {
    const int n = std::max(1, int((step1 - step0) * 4));
    for (int i = 0; i <= n; ++i)
      param(step0 + (step1 - step0) * i / n, idx, int(std::lround(v0 + double(v1 - v0) * i / n)));
  }
  void note(double step, double len, int note, int vel = 110, int chord = 0, bool accent = false, bool slide = false) {
    evs.push_back({step * stepSec, 2, note, vel, chord, accent, slide});
    evs.push_back({(step + len) * stepSec, 1, note, 0, 0, false, false});
  }
  void render(double steps, double tailSec, const char* name) {
    std::stable_sort(evs.begin(), evs.end(), [](const Ev& x, const Ev& y) {
      if (x.t != y.t) return x.t < y.t;
      return x.kind < y.kind;
    });
    for (const Ev& e : evs) {
      s.runTo(e.t);
      if (e.kind == 0) s.set(e.a, e.b);
      else if (e.kind == 1) s.off(e.a);
      else s.on(e.a, e.b, e.c, e.accent, e.slide);
    }
    s.runTo(steps * stepSec + tailSec);
    const std::string path = std::string(kDir) + "/" + name + ".wav";
    const bool ok = s.write(path);
    LT_CHECK(ok, "could not write %s", path.c_str());
    LT_CHECK(allFinite(s.L) && allFinite(s.R), "%s not finite", name);
    const double pk = std::max(peak(s.L), peak(s.R));
    LT_CHECK(pk <= 1.0 && pk > 0.05, "%s peak %.3f", name, pk);
    std::printf("  %-28s %6.2f s  peak %6.2f dBFS  rms %6.2f dBFS\n", name, s.now(), dB(pk),
                dB(std::sqrt(0.5 * (rms(s.L) * rms(s.L) + rms(s.R) * rms(s.R)))));
    evs.clear();
  }
};

// 1. Default patch: off-beat min7 / min9 stabs, then a syncopated second half with accents.
void houseStab() {
  Seq q(124);
  const int roots[4] = {57, 57, 53, 55};                       // Am7 Am9 Fmaj7 G(dom7)
  const int chords[4] = {pg::kChordMin7, pg::kChordMin9, pg::kChordMaj7, pg::kChordDom7};
  for (int bar = 0; bar < 4; ++bar)
    for (int beat = 0; beat < 4; ++beat) q.note(bar * 16 + beat * 4 + 2, 1.0, roots[bar], 110, chords[bar]);
  const int pat[] = {0, 3, 6, 10, 12, 14};                     // classic 3-3-4 push
  for (int bar = 4; bar < 8; ++bar)
    for (int i = 0; i < 6; ++i)
      q.note(bar * 16 + pat[i], i == 2 ? 2.5 : 0.75, roots[bar - 4], i % 3 == 0 ? 120 : 96, chords[bar - 4], i == 0);
  q.render(128, 1.0, "01_house_stab_default");
}

// 2. Pluck arpeggio: one saw + sub square, snappy filter envelope, no sustain.
void pluckArp() {
  Seq q(124);
  q.set(kPCutoff, 48); q.set(kPReso, 50); q.set(kPEnvAmt, 84); q.set(kPFltDecay, 34);
  q.set(kPDecay, 52); q.set(kPSustain, 0); q.set(kPRelease, 40);
  q.set(kPOsc2Wave, kOsc2Pulse); q.set(kPInterval, kIntervalCentre - 12); q.set(kPOscMix, 44);
  q.set(kPDetune, 24); q.set(kPWidth, 110);
  const int arp[4][4] = {{57, 60, 64, 67}, {57, 60, 64, 69}, {53, 57, 60, 64}, {55, 59, 62, 65}};
  for (int bar = 0; bar < 8; ++bar) {
    for (int st = 0; st < 16; ++st) {
      const int* a = arp[bar % 4];
      const int n = a[(st * 3) % 4] + ((st % 8) >= 5 ? 12 : 0);
      q.note(bar * 16 + st, 0.5, n, st % 4 == 0 ? 118 : 88, 0, st % 8 == 3);
    }
  }
  q.ramp(64, 128, kPCutoff, 48, 76);   // open the filter over the second half
  q.render(128, 1.2, "02_pluck_arp");
}

// 3. Drawbar organ: the bouncy garage-house organ riff, then chord stabs.
void organHouse() {
  Seq q(124);
  q.set(kPWave, kWaveOrgan); q.set(kPShape, 64); q.set(kPOscMix, 0);
  q.set(kPCutoff, 104); q.set(kPReso, 0); q.set(kPEnvAmt, 24); q.set(kPFltDecay, 30);
  q.set(kPDecay, 60); q.set(kPSustain, 100); q.set(kPRelease, 16); q.set(kPWidth, 90);
  // riff (single notes, the organ register) for 4 bars
  const int riff[] = {57, -1, 57, 69, -1, 57, 67, -1, 57, -1, 64, 69, -1, 67, 64, 60};
  for (int bar = 0; bar < 4; ++bar)
    for (int st = 0; st < 16; ++st)
      if (riff[st] >= 0) q.note(bar * 16 + st, 0.8, riff[st] + (bar == 3 && st > 11 ? 2 : 0), st % 4 == 0 ? 120 : 100);
  // chord stabs for 4 bars
  const int pat[] = {0, 3, 6, 8, 11, 14};
  const int roots[4] = {57, 57, 60, 55};
  const int chords[4] = {pg::kChordMin7, pg::kChordMin7, pg::kChordMaj, pg::kChordMaj};
  for (int bar = 4; bar < 8; ++bar)
    for (int i = 0; i < 6; ++i) q.note(bar * 16 + pat[i], i == 3 ? 2.0 : 0.9, roots[bar - 4], 108, chords[bar - 4], i == 0);
  q.ramp(96, 128, kPShape, 64, 110);   // pull more drawbars out
  q.render(128, 0.8, "03_organ_house");
}

// 4. FM keys: electric-piano-ish house chords (FM1), then FM2 hollow stabs.
void fmKeys() {
  Seq q(124);
  q.set(kPWave, kWaveFm1); q.set(kPShape, 62); q.set(kPOscMix, 0);
  q.set(kPCutoff, 92); q.set(kPReso, 0); q.set(kPEnvAmt, 30); q.set(kPFltDecay, 58);
  q.set(kPDecay, 86); q.set(kPSustain, 30); q.set(kPRelease, 50); q.set(kPWidth, 100);
  const int roots[4] = {50, 55, 48, 53};                        // Dm9 G7 Cmaj7 Fmaj7
  const int chords[4] = {pg::kChordMin9, pg::kChordDom7, pg::kChordMaj7, pg::kChordMaj7};
  const int pat[] = {0, 3, 6, 9, 12};
  for (int bar = 0; bar < 4; ++bar)
    for (int i = 0; i < 5; ++i)
      q.note(bar * 16 + pat[i], i == 4 ? 3.0 : 1.5, roots[bar] + 12, i == 0 ? 122 : 96 + 4 * i, chords[bar], i == 0);
  q.param(63.5, kPWave, kWaveFm2);
  q.param(63.5, kPShape, 70);
  q.param(63.5, kPSustain, 0);
  q.param(63.5, kPDecay, 60);
  for (int bar = 4; bar < 8; ++bar)
    for (int beat = 0; beat < 4; ++beat) {
      q.note(bar * 16 + beat * 4 + 2, 1.0, roots[bar - 4] + 12, 112, chords[bar - 4]);
      if (beat == 3) q.note(bar * 16 + 15, 0.7, roots[bar - 4] + 12, 90, chords[bar - 4]);
    }
  q.render(128, 1.2, "04_fm_keys");
}

// 5. Techno stab: detuned super-saw minor stabs through a resonant low-pass
//    that opens over 5 bars and closes over 3, with some drive.
void technoStab() {
  Seq q(132);
  q.set(kPWave, kWaveSuper); q.set(kPShape, 60); q.set(kPDetune, 70); q.set(kPOscMix, 0);
  q.set(kPCutoff, 24); q.set(kPReso, 96); q.set(kPEnvAmt, 36); q.set(kPFltDecay, 50);
  q.set(kPDecay, 64); q.set(kPSustain, 0); q.set(kPRelease, 36); q.set(kPDrive, 50); q.set(kPWidth, 100);
  const int pat[] = {0, 3, 6, 9, 11, 14};
  for (int bar = 0; bar < 8; ++bar)
    for (int i = 0; i < 6; ++i)
      q.note(bar * 16 + pat[i], 0.7, (bar % 4 == 3 && i > 3) ? 51 : 48, i == 0 ? 124 : 100, pg::kChordMin, i == 0 || i == 3);
  q.ramp(0, 80, kPCutoff, 24, 100);
  q.ramp(80, 128, kPCutoff, 100, 36);
  q.render(128, 1.0, "05_techno_stab_sweep");
}

// 6. Short pad: PWM pulses, slow attack, high-ish cutoff, slow LFO.
void pad() {
  Seq q(124);
  q.set(kPWave, kWavePulse); q.set(kPOsc2Wave, kOsc2Saw); q.set(kPShape, 30); q.set(kPOscMix, 56);
  q.set(kPDetune, 60); q.set(kPCutoff, 62); q.set(kPReso, 18); q.set(kPEnvAmt, 30);
  q.set(kPFltAttack, 96); q.set(kPFltDecay, 100);
  q.set(kPAttack, 84); q.set(kPDecay, 100); q.set(kPSustain, 110); q.set(kPRelease, 88);
  q.set(kPLfoRate, 40); q.set(kPLfoShape, 60); q.set(kPVibrato, 14); q.set(kPWidth, 127);
  q.note(0, 28, 57, 100, pg::kChordMin9);
  q.note(32, 28, 53, 100, pg::kChordMaj7);
  q.note(64, 28, 50, 100, pg::kChordMin7);
  q.note(96, 28, 55, 100, pg::kChordSus4);
  q.render(128, 2.5, "06_pad_pwm");
}

// 7. Mono lead with 303-style slides (overlapping gates + slide flag).
void monoLead() {
  Seq q(124);
  q.set(kPWave, kWaveSaw); q.set(kPOsc2Wave, kOsc2Pulse); q.set(kPInterval, kIntervalCentre - 12);
  q.set(kPOscMix, 50); q.set(kPDetune, 30); q.set(kPShape, 20);
  q.set(kPCutoff, 60); q.set(kPReso, 60); q.set(kPEnvAmt, 50); q.set(kPFltDecay, 60);
  q.set(kPDecay, 80); q.set(kPSustain, 100); q.set(kPRelease, 30); q.set(kPGlide, 70);
  q.set(kPVibrato, 30); q.set(kPLfoRate, 98); q.set(kPWidth, 60);
  struct N { int st; double len; int note; bool slide, accent; };
  const N line[] = {
      {0, 2.2, 69, false, true}, {2, 1.2, 72, true, false}, {3, 1.0, 69, true, false},  {6, 1.0, 76, false, false},
      {8, 2.2, 74, false, true}, {10, 2.2, 72, true, false}, {12, 1.2, 67, true, false}, {13, 2.0, 69, true, false},
      {16, 1.0, 81, false, true}, {18, 2.2, 79, false, false}, {20, 1.2, 76, true, false}, {21, 2.5, 72, true, false},
      {24, 3.2, 69, false, false}, {27, 1.2, 81, true, true}, {28, 3.0, 69, true, false},
  };
  for (int rep = 0; rep < 4; ++rep)
    for (const N& n : line) q.note(rep * 32 + n.st, n.len, n.note - (rep == 2 ? 5 : 0), 108, 0, n.accent, n.slide);
  q.render(128, 1.0, "07_mono_lead_slide");
}

// 8. Bell / FM stab and a wide super-saw chord progression: the remaining flavours.
void bellAndSuper() {
  {
    Seq q(124);
    q.set(kPWave, kWaveBell); q.set(kPShape, 70); q.set(kPOscMix, 0);
    q.set(kPCutoff, 100); q.set(kPReso, 0); q.set(kPEnvAmt, 20); q.set(kPFltDecay, 76);
    q.set(kPDecay, 90); q.set(kPSustain, 0); q.set(kPRelease, 76); q.set(kPWidth, 110);
    const int mel[] = {72, 76, 79, 84, 79, 76, 81, 77};
    for (int i = 0; i < 16; ++i) q.note(i * 4 + (i % 2 ? 2 : 0), 1.5, mel[i % 8], 96 + (i % 4) * 8, i % 4 == 3 ? pg::kChordFifth : 0);
    q.render(64, 2.0, "08_fm_bell");
  }
  {
    Seq q(126);
    q.set(kPWave, kWaveSuper); q.set(kPShape, 90); q.set(kPDetune, 72); q.set(kPOscMix, 30);
    q.set(kPOsc2Wave, kOsc2Saw); q.set(kPInterval, kIntervalCentre + 12);
    q.set(kPCutoff, 80); q.set(kPReso, 10); q.set(kPEnvAmt, 30); q.set(kPFltDecay, 70);
    q.set(kPAttack, 10); q.set(kPDecay, 90); q.set(kPSustain, 100); q.set(kPRelease, 60); q.set(kPWidth, 127);
    const int roots[4] = {53, 55, 57, 52};
    const int chords[4] = {pg::kChordMaj7, pg::kChordMaj, pg::kChordMin7, pg::kChordMin7};
    for (int bar = 0; bar < 4; ++bar) {
      q.note(bar * 16, 5.5, roots[bar], 110, chords[bar]);
      q.note(bar * 16 + 6, 3.5, roots[bar], 100, chords[bar]);
      q.note(bar * 16 + 10, 5.5, roots[bar], 104, chords[bar]);
    }
    q.render(64, 1.5, "09_supersaw_chords");
  }
}

}  // namespace

int main() {
  mkdirs(kDir);
  std::printf("rendering audition files to %s/\n", kDir);
  houseStab();
  pluckArp();
  organHouse();
  fmKeys();
  technoStab();
  pad();
  monoLead();
  bellAndSuper();
  return finish("test_lead_render");
}
