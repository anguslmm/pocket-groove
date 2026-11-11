// Envelope times against the published mappings (amp attack / decay / release,
// filter-envelope decay and attack) and clicks at note on, note off, steal,
// choke and legato retarget.
#include "test_lead_util.h"

using namespace lt;
using namespace pg::lead;

namespace {

size_t at(const Synth& s, double t) { return size_t(t * double(s.fs)); }

// Pure sine voice from osc2 (osc2 starts at a non-zero phase, so the onset is a
// real test of the attack ramp, unlike a sine that starts at phase 0).
void sineOsc2(Synth& s) {
  openFilter(s);
  s.set(kPWave, kWaveSaw);
  s.set(kPOscMix, 127);
  s.set(kPOsc2Wave, kOsc2Sine);
  s.set(kPInterval, kIntervalCentre);
  s.set(kPDetune, 0);
  s.set(kPWidth, 0);
  s.set(kPVibrato, 0);
}

// First index >= from where env drops below `level` (or rises above, if up).
size_t crossing(const std::vector<double>& e, size_t from, double level, bool up) {
  for (size_t i = from; i < e.size(); ++i)
    if (up ? e[i] >= level : e[i] <= level) return i;
  return e.size();
}

// T60 from the time the envelope takes to fall from -10 to -40 dB re `ref`.
double t60From(const std::vector<double>& e, size_t from, double ref, double fs) {
  const size_t a = crossing(e, from, ref * std::pow(10.0, -10.0 / 20.0), false);
  const size_t b = crossing(e, a, ref * std::pow(10.0, -40.0 / 20.0), false);
  return 2.0 * double(b - a) / fs;
}

// 4th-order Butterworth high-pass at 5 kHz (two RBJ biquads): what is left of a
// note below ~600 Hz after this is click energy.
std::vector<double> highpass5k(const std::vector<float>& x, double fs) {
  std::vector<double> y(x.begin(), x.end());
  for (double q : {0.5411961, 1.3065630}) {
    const double w = 2.0 * M_PI * 5000.0 / fs, c = std::cos(w), al = std::sin(w) / (2.0 * q);
    const double b0 = (1 + c) / 2, b1 = -(1 + c), b2 = (1 + c) / 2, a0 = 1 + al, a1 = -2 * c, a2 = 1 - al;
    double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    for (double& v : y) {
      const double o = (b0 * v + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2) / a0;
      x2 = x1; x1 = v; y2 = y1; y1 = o; v = o;
    }
  }
  return y;
}
double absPeak(const std::vector<double>& x, size_t a, size_t b) {
  double m = 0;
  for (size_t i = a; i < std::min(b, x.size()); ++i) m = std::max(m, std::fabs(x[i]));
  return m;
}

// HF peak of a hard-gated sine of amplitude `amp` switched on at its crest: the
// worst-case click this test calibrates against.
double clickReference(double fs, double hz, double amp) {
  std::vector<float> x(size_t(fs * 0.05), 0.0f);
  for (size_t i = x.size() / 2; i < x.size(); ++i)
    x[i] = float(amp * std::cos(2.0 * M_PI * hz * double(i - x.size() / 2) / fs));
  const std::vector<double> h = highpass5k(x, fs);
  return absPeak(h, 0, h.size());
}

}  // namespace

int main() {
  const double fs = 44100.0;

  // ---- amp attack: linear ramp, 90% point at 0.9 x mapping (+- one quantum)
  std::printf("amp attack (time to 90%%):\n");
  for (int v : {0, 30, 64, 96, 127}) {
    Synth s;
    sineOsc2(s);
    s.set(kPAttack, v);
    const double T = double(LeadVoice::attackSeconds(uint8_t(v)));
    s.on(69);
    s.run(T + 0.15);
    const std::vector<double> e = envelope(s.L);
    double mx = 0;
    for (size_t i = at(s, T + 0.02); i < at(s, T + 0.12); ++i) mx = std::max(mx, e[i]);
    const double t90 = double(crossing(e, 0, 0.9 * mx, true)) / fs;
    const double tol = 0.12 * T + 2.0 * pg::kQuantum / fs;
    std::printf("  ATK %3d: mapping %8.4f s, measured t90 %8.4f s (ideal %8.4f)\n", v, T, t90, 0.9 * T);
    LT_CHECK(std::fabs(t90 - 0.9 * T) < tol, "attack %d: t90 %.4f vs %.4f", v, t90, 0.9 * T);
  }

  // ---- amp decay to sustain 0 and release: T60
  std::printf("amp decay / release (T60):\n");
  for (int v : {0, 30, 64, 96, 127}) {
    {
      Synth s;
      sineOsc2(s);
      s.set(kPSustain, 0);
      s.set(kPDecay, v);
      const double T = double(LeadVoice::decaySeconds(uint8_t(v)));
      s.on(69);
      s.run(T * 1.2 + 0.05);
      const std::vector<double> e = envelope(s.L);
      double mx = 0;
      for (double x : e) mx = std::max(mx, x);
      size_t pk = 0;
      for (size_t i = 0; i < e.size(); ++i)
        if (e[i] > e[pk]) pk = i;
      const double m = t60From(e, pk, mx, fs);
      std::printf("  DCY %3d: mapping %8.4f s, measured %8.4f s\n", v, T, m);
      LT_CHECK(std::fabs(m / T - 1.0) < 0.1, "decay %d: T60 %.4f vs %.4f", v, m, T);
    }
    {
      Synth s;
      sineOsc2(s);
      s.set(kPRelease, v);
      const double T = double(LeadVoice::releaseSeconds(uint8_t(v)));
      s.on(69);
      s.run(0.1);
      const double ref = rms(s.L, at(s, 0.05), at(s, 0.1)) * std::sqrt(2.0);
      s.off(69);
      s.run(T * 1.4 + 0.05);   // the voice is freed at -80 dB = 1.33 x T60
      const std::vector<double> e = envelope(s.L);
      const double m = t60From(e, at(s, 0.1), ref, fs);
      std::printf("  REL %3d: mapping %8.4f s, measured %8.4f s\n", v, T, m);
      LT_CHECK(std::fabs(m / T - 1.0) < 0.1, "release %d: T60 %.4f vs %.4f", v, m, T);
      LT_CHECK(!s.v.active(), "release %d: still active after 1.4 x T60", v);
    }
  }

  // ---- filter envelope decay. A sine far above a 2-pole low-pass cutoff loses
  // 12 dB per octave of cutoff, i.e. 1 dB per semitone: its level in dB tracks
  // the envelope (72 semitones at ENV max, velocity 127) linearly.
  // Fit dB(t) = c + 72 exp(-t / tau) where the cutoff is >= 2 octaves below.
  std::printf("filter envelope decay (T60, fitted from a sine above the cutoff):\n");
  for (int v : {0, 40, 80, 127}) {
    Synth s;
    sineOsc2(s);
    s.set(kPCutoff, 0); s.set(kPEnvAmt, 127); s.set(kPKeyTrack, 0); s.set(kPFltDecay, v);
    const double T = double(LeadVoice::fltDecaySeconds(uint8_t(v)));
    s.on(96);   // 2093 Hz; cutoff at env 0 is ~46 Hz
    s.run(T * 0.9 + 0.05);
    const std::vector<double> e = envelope(s.L);
    // Samples where the cutoff (30 + 72 fenv semis, fenv from the model) is 24+ semis below 96:
    // fenv < 0.58. Use the stretch fenv 0.5 .. 0.02 (t = 0.1 T .. 0.57 T) skipping the attack.
    std::vector<double> ts, ys;
    for (double f = 0.10; f <= 0.57; f += 0.01) {
      const size_t i = at(s, f * T);
      ts.push_back(f * T);
      ys.push_back(dB(e[i]));
    }
    double best = 1e30, bestTau = 0;
    for (double tau = T / 6.9078 * 0.5; tau < T / 6.9078 * 2.0; tau *= 1.002) {
      double c = 0;
      for (size_t i = 0; i < ts.size(); ++i) c += ys[i] - 72.0 * std::exp(-ts[i] / tau);
      c /= double(ts.size());
      double err = 0;
      for (size_t i = 0; i < ts.size(); ++i) {
        const double d = ys[i] - c - 72.0 * std::exp(-ts[i] / tau);
        err += d * d;
      }
      if (err < best) { best = err; bestTau = tau; }
    }
    const double m = bestTau * 6.9078;
    std::printf("  FDCY %3d: mapping %8.4f s, measured %8.4f s (fit rms %.2f dB)\n", v, T, m, std::sqrt(best / double(ts.size())));
    LT_CHECK(std::fabs(m / T - 1.0) < 0.2, "filter decay %d: T60 %.4f vs %.4f", v, m, T);
  }
  // Filter envelope attack: the brightest point comes at the attack time.
  std::printf("filter envelope attack (time of brightest point):\n");
  for (int v : {40, 80, 110}) {
    Synth s;
    sineOsc2(s);
    s.set(kPCutoff, 0); s.set(kPEnvAmt, 127); s.set(kPKeyTrack, 0); s.set(kPFltDecay, 127); s.set(kPFltAttack, v);
    const double T = double(LeadVoice::fltAttackSeconds(uint8_t(v)));
    s.on(96);
    s.run(T * 1.5 + 0.05);
    const std::vector<double> e = envelope(s.L);
    size_t k = 0;
    for (size_t i = 0; i < e.size() - 64; ++i)
      if (e[i] > e[k]) k = i;
    const double m = double(k) / fs;
    std::printf("  FATK %3d: mapping %8.4f s, brightest at %8.4f s\n", v, T, m);
    LT_CHECK(std::fabs(m / T - 1.0) < 0.15, "filter attack %d: %.4f vs %.4f", v, m, T);
  }

  // ---- clicks: high-passed (5 kHz) output of a sine voice must stay >= 20 dB
  // below a hard-gated sine of the same amplitude, at note on (fastest attack),
  // note off (fastest release), steal, choke and legato retarget.
  std::printf("clicks (HF peak re a hard-gated sine, need < -20 dB):\n");
  for (float sr : {44100.0f, 32000.0f}) {
    for (int note : {45, 57, 69}) {
      Synth s(sr);
      sineOsc2(s);
      s.set(kPAttack, 0);
      s.set(kPRelease, 0);
      s.set(kPGlide, 0);
      const double hz = midiHz(note);
      s.on(note);
      s.run(0.2);
      const double amp = rms(s.L, at(s, 0.1), at(s, 0.2)) * std::sqrt(2.0);
      const double ref = clickReference(sr, hz, amp);
      s.off(note);
      s.run(0.15);                                   // 0.2: off
      for (int i = 0; i < 4; ++i) { s.on(note + 3 * i); s.run(0.01); }   // 0.35: 4 notes
      s.run(0.15);
      s.on(note + 14);                               // 0.54: steal
      s.run(0.15);
      s.on(note + 12, 127, 0, false, true);          // 0.69: legato retarget
      s.run(0.15);
      s.v.choke();                                   // 0.84: choke
      s.run(0.05);
      const std::vector<double> h = highpass5k(s.L, sr);
      struct W { const char* what; double t; };
      const W ws[] = {{"note on", 0.0}, {"note off", 0.2}, {"poly on", 0.35}, {"steal", 0.54}, {"legato", 0.69}, {"choke", 0.84}};
      std::printf("  fs %5.0f note %d:", double(sr), note);
      for (const W& w : ws) {
        const size_t a = at(s, w.t) > 32 ? at(s, w.t) - 32 : 0;
        const double r = dB(absPeak(h, a, at(s, w.t + 0.04)) / ref);
        std::printf("  %s %.0f", w.what, r);
        LT_CHECK(r < -20.0, "fs %.0f note %d: click at %s (%.1f dB re hard gate)", double(sr), note, w.what, r);
      }
      std::printf("\n");
    }
  }
  return finish("test_lead_env");
}
