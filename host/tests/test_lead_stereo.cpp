// Stereo vs the MONO device. The Cardputer's codec is mono and the firmware
// plays (L+R)/2, so: WIDTH 0 must give L == R exactly, and at default / max
// width the mono sum must be as full as WIDTH 0 (level within 1 dB, no comb
// notches from spreading detuned oscillators), for every oscillator flavour.
// setMono(true) must render that same mono sum.
#include "test_lead_util.h"

using namespace lt;
using namespace pg::lead;

namespace {

std::vector<float> monoSum(const Synth& s) {
  std::vector<float> m(s.L.size());
  for (size_t i = 0; i < m.size(); ++i) m[i] = 0.5f * (s.L[i] + s.R[i]);
  return m;
}

// A short chord sequence that exercises chords, singles and overlaps.
void play(Synth& s) {
  s.on(57, 127, pg::kChordMin7);
  s.run(0.35);
  s.off(57);
  s.on(53, 110, pg::kChordMin9, true);
  s.run(0.35);
  s.off(53);
  s.on(45, 100);
  s.run(0.1);
  s.on(64, 90);
  s.run(0.3);
  s.off(45);
  s.off(64);
  s.run(0.3);
}

// Average spectrum of x in 1/3-octave bands from 100 Hz to 8 kHz (dB).
std::vector<double> thirdOctaves(const std::vector<float>& x, double fs) {
  const size_t n = 4096;
  std::vector<double> acc(n / 2, 0.0);
  int frames = 0;
  for (size_t st = 0; st + n <= x.size(); st += n / 2, ++frames) {
    const std::vector<double> m = spectrum(x, st, n);
    for (size_t i = 0; i < m.size(); ++i) acc[i] += m[i] * m[i];
  }
  std::vector<double> bands;
  const double binHz = fs / double(n);
  for (double f = 100.0; f < 8000.0; f *= std::pow(2.0, 1.0 / 3.0)) {
    double e = 0;
    for (size_t b = size_t(f / binHz); b < size_t(f * std::pow(2.0, 1.0 / 3.0) / binHz); ++b) e += acc[b];
    bands.push_back(10.0 * std::log10(std::max(e / std::max(frames, 1), 1e-30)));
  }
  return bands;
}

}  // namespace

int main() {
  const char* names[kNumWaves] = {"SAW", "PULSE", "TRI", "SUPER", "FM1", "FM2", "BELL", "ORGAN"};
  std::printf("mono sum vs WIDTH 0, defaults + WAVE (and SHAPE 64 / max DETUNE variants)\n");
  std::printf("  %-14s %10s %10s %10s %12s %12s\n", "patch", "side/mid", "lvl dflt", "lvl max", "band dflt", "band max");
  for (int w = 0; w < kNumWaves; ++w) {
    for (int variant = 0; variant < 2; ++variant) {
      auto patch = [&](Synth& s, int width) {
        s.set(kPWave, w);
        if (variant == 1) { s.set(kPShape, 64); s.set(kPDetune, 127); }
        s.set(kPWidth, width);
      };
      Synth z;
      patch(z, 0);
      play(z);
      const int dflt = z.v.paramInfo(kPWidth)->def;
      Synth d, m;
      patch(d, dflt);
      patch(m, 127);
      play(d);
      play(m);
      // WIDTH 0: L == R exactly.
      bool same = true;
      for (size_t i = 0; i < z.L.size(); ++i)
        if (z.L[i] != z.R[i]) { same = false; break; }
      LT_CHECK(same, "%s v%d: WIDTH 0 but L != R", names[w], variant);

      const std::vector<float> md = monoSum(d), mm = monoSum(m);
      const double r0 = rms(z.L), rd = rms(md), rm = rms(mm);
      std::vector<float> side(d.L.size());
      for (size_t i = 0; i < side.size(); ++i) side[i] = 0.5f * (d.L[i] - d.R[i]);
      const double sm = dB(rms(side) / rd);
      const std::vector<double> b0 = thirdOctaves(z.L, z.fs), bd = thirdOctaves(md, d.fs), bm = thirdOctaves(mm, m.fs);
      double devD = 0, devM = 0;
      for (size_t i = 0; i < b0.size(); ++i) {
        devD = std::max(devD, std::fabs(bd[i] - b0[i]));
        devM = std::max(devM, std::fabs(bm[i] - b0[i]));
      }
      char label[32];
      std::snprintf(label, sizeof label, "%s%s", names[w], variant ? " shp/det" : "");
      std::printf("  %-14s %8.1f dB %+8.2f dB %+8.2f dB %10.2f dB %10.2f dB\n", label, sm, dB(rd / r0), dB(rm / r0), devD, devM);
      // The default patch must actually be stereo, or this test proves nothing.
      if (w == kWaveSaw && variant == 0) LT_CHECK(sm > -20.0, "default patch is not stereo (S/M %.1f dB)", sm);
      LT_CHECK(std::fabs(dB(rd / r0)) < 1.0, "%s: mono sum at default width %.2f dB re WIDTH 0", label, dB(rd / r0));
      LT_CHECK(std::fabs(dB(rm / r0)) < 1.0, "%s: mono sum at max width %.2f dB re WIDTH 0", label, dB(rm / r0));
      // No comb filtering: every 1/3-octave band of the mono sum within 1 dB of WIDTH 0.
      LT_CHECK(devD < 1.0 && devM < 1.0, "%s: mono-sum spectrum differs from WIDTH 0 by %.2f / %.2f dB", label, devD, devM);
    }
  }

  // setMono(true): the device path. Same as (L+R)/2 of the stereo render.
  {
    Synth st, mo;
    mo.v.setMono(true);
    play(st);
    play(mo);
    const std::vector<float> m = monoSum(st);
    double err = 0;
    bool lr = true;
    for (size_t i = 0; i < m.size(); ++i) {
      err = std::max(err, double(std::fabs(m[i] - mo.L[i])));
      if (mo.L[i] != mo.R[i]) lr = false;
    }
    std::printf("setMono(true) vs (L+R)/2 of the default stereo render: max diff %.2e (peak %.3f)\n", err, peak(m));
    LT_CHECK(lr, "setMono: L != R");
    LT_CHECK(err < 1e-4, "setMono output differs from the stereo mono sum by %.2e", err);
  }

  // WIDTH moved while notes sound (p-lock / knob): still bounded, no jump in the mono sum.
  {
    Synth s;
    s.on(57, 127, pg::kChordMin7);
    s.run(0.2);
    const double steady = maxStep(monoSum(s), size_t(0.1 * double(s.fs)), size_t(0.2 * double(s.fs)));
    for (int i = 0; i < 20; ++i) { s.set(kPWidth, i % 2 ? 127 : 0); s.run(0.02); }
    const std::vector<float> m = monoSum(s);
    const double st = maxStep(m, size_t(0.2 * double(s.fs)) - 4);
    std::printf("WIDTH flipped 0 <-> 127 every 20 ms under a chord: mono max step %.4f (steady %.4f)\n", st, steady);
    LT_CHECK(st < steady * 1.5, "WIDTH change disturbs the mono sum");
  }
  return finish("test_lead_stereo");
}
