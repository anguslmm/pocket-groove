// Benchmark (ns per stereo sample) and level report for the lead voice.
#include <chrono>

#include "test_lead_util.h"

using namespace lt;
using namespace pg::lead;

static double bench(int wave, int osc2Mix, int shape, int nNotes, int flt = kFilterLP, bool mono = false) {
  Synth s;
  s.v.setMono(mono);
  s.set(kPWave, wave);
  s.set(kPOscMix, osc2Mix);
  s.set(kPShape, shape);
  s.set(kPSustain, 127);
  s.set(kPFltType, flt);
  s.set(kPOsc2Wave, kOsc2Pulse);
  const int notes[4] = {48, 55, 60, 64};
  for (int i = 0; i < nNotes; ++i) s.on(notes[i]);
  float l[pg::kQuantum], r[pg::kQuantum];
  for (int q = 0; q < 2000; ++q) { s.v.setParams(s.p); s.v.render(l, r, pg::kQuantum); }
  double best = 1e9;
  volatile float sink = 0;
  for (int rep = 0; rep < 7; ++rep) {
    const int nq = 20000;
    const auto t0 = std::chrono::steady_clock::now();
    for (int q = 0; q < nq; ++q) {
      s.v.setParams(s.p);
      s.v.render(l, r, pg::kQuantum);
      sink = sink + l[0];
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / double(nq * pg::kQuantum);
    best = std::min(best, ns);
  }
  if (s.v.soundingVoices() != nNotes) { ++gFailures; std::printf("  FAIL: expected %d voices sounding\n", nNotes); }
  return best;
}

int main() {
  std::printf("lead benchmark, ns per stereo sample (best of 7)\n");
  std::printf("  %-34s %8s %8s\n", "patch", "1 voice", "4 voices");
  struct Case { const char* name; int wave, mix, shape; };
  const Case cases[] = {
      {"SAW + SAW (default stab)", kWaveSaw, 64, 0},
      {"SAW only", kWaveSaw, 0, 0},
      {"PULSE + PULSE", kWavePulse, 64, 40},
      {"TRI morph + PULSE", kWaveTri, 64, 64},
      {"SUPER (5 saws)", kWaveSuper, 0, 64},
      {"SUPER + PULSE (worst case)", kWaveSuper, 64, 64},
      {"FM1", kWaveFm1, 0, 64},
      {"BELL + PULSE", kWaveBell, 64, 64},
      {"ORGAN", kWaveOrgan, 0, 50},
  };
  double worst4 = 0, def4 = 0;
  for (const Case& c : cases) {
    const double a = bench(c.wave, c.mix, c.shape, 1), b = bench(c.wave, c.mix, c.shape, 4);
    std::printf("  %-34s %8.1f %8.1f\n", c.name, a, b);
    worst4 = std::max(worst4, b);
    if (&c == &cases[0]) def4 = b;
  }
  std::printf("  HP filter, SUPER + PULSE, 4 voices: %.1f\n", bench(kWaveSuper, 64, 64, 4, kFilterHP));
  // The device codec is mono: setMono(true) runs one filter per voice.
  const double monoDef = bench(kWaveSaw, 64, 0, 4, kFilterLP, true), monoWorst = bench(kWaveSuper, 64, 64, 4, kFilterLP, true);
  std::printf("  setMono(true), 4 voices: default stab %.1f, SUPER + PULSE %.1f\n", monoDef, monoWorst);
  (void)monoWorst;
  // x86 is 15-25x faster than the ESP32-S3 for this code. 1100 cycles/sample at
  // 240 MHz = 4583 ns; even at 25x that allows 183 ns here. Fail far above that
  // so a loaded CI box doesn't flake but a real regression (2x+) is caught.
  LT_CHECK(worst4 < 400.0, "worst case 4-voice cost %.1f ns/sample", worst4);
  LT_CHECK(def4 < 250.0, "default 4-voice cost %.1f ns/sample", def4);

  // Idle cost must be nil: active() false => engine does not call render.
  {
    Synth s;
    s.run(0.05);
    LT_CHECK(s.renderedQuanta == 0, "idle synth rendered");
  }

  // ---- level report: full-velocity 4-note chords at defaults
  std::printf("levels at defaults (vel 127, held 0.6 s):\n");
  double worstPk = 0, sumPk = 0;
  int n = 0;
  const int chords[] = {pg::kChordMin7, pg::kChordMin9, pg::kChordMaj7, pg::kChordMaj, pg::kChordDom7};
  const char* names[] = {"min7", "min9", "maj7", "maj", "dom7"};
  for (int ci = 0; ci < 5; ++ci) {
    for (int root : {48, 53, 57, 60, 65}) {
      Synth s;
      s.on(root, 127, chords[ci]);
      s.run(0.6);
      const double pl = peak(s.L), pr = peak(s.R);
      const double pk = std::max(pl, pr);
      if (root == 57) std::printf("  %-5s root %d: peak L %6.2f dBFS  R %6.2f dBFS   rms L %6.2f  R %6.2f\n", names[ci], root,
                  dB(pl), dB(pr), dB(rms(s.L)), dB(rms(s.R)));
      worstPk = std::max(worstPk, pk);
      sumPk += dB(pk);
      ++n;
    }
  }
  std::printf("  25 chords: mean peak %.2f dBFS, max peak %.2f dBFS\n", sumPk / n, dB(worstPk));
  LT_CHECK(std::fabs(sumPk / n + 9.0) < 1.5, "mean default chord peak %.2f dBFS, want about -9", sumPk / n);
  LT_CHECK(dB(worstPk) < -6.0, "max default chord peak %.2f dBFS", dB(worstPk));
  {
    Synth s;
    s.on(60, 127, pg::kChordNone);
    s.run(0.6);
    std::printf("  single note 60: peak %.2f dBFS\n", dB(std::max(peak(s.L), peak(s.R))));
    Synth a;
    a.on(57, 127, pg::kChordMin7, true);
    a.run(0.6);
    std::printf("  min7 root 57 with accent: peak %.2f dBFS\n", dB(std::max(peak(a.L), peak(a.R))));
    Synth q;
    q.on(57, 64, pg::kChordMin7);
    q.run(0.6);
    std::printf("  min7 root 57 at velocity 64: peak %.2f dBFS\n", dB(std::max(peak(q.L), peak(q.R))));
  }
  return finish("test_lead_bench");
}
