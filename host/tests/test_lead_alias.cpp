// Aliasing: with the filter open, everything that is not a harmonic of the
// played note (aliases, interpolation images, quantisation noise) must be far
// below the signal. Measured over the full band 20 Hz .. Nyquist.
#include "test_lead_util.h"

using namespace lt;
using namespace pg::lead;

struct Result {
  double nonHarmDb;    // non-harmonic energy / total energy
  double topHarmHz;    // highest harmonic within 40 dB of the fundamental
  double h1;           // fundamental magnitude
  std::vector<double> harm;
};

static Result measure(float fs, int wave, int shape, int note, double baseDiv = 1.0) {
  Synth s(fs);
  openFilter(s);
  s.set(kPWave, wave);
  s.set(kPShape, shape);
  s.set(kPDetune, 0);
  s.set(kPWidth, 0);
  s.set(kPOscMix, 0);
  s.set(kPFltDecay, 127);   // keep FM index / organ percussion steady during the window
  const size_t n = 65536;
  s.on(note);
  s.run(0.3 + double(n) / double(fs));
  const size_t start = size_t(0.25 * double(fs));
  const std::vector<double> m = spectrum(s.L, start, n);
  const double binHz = double(fs) / double(n);
  // Nominal pitch (test_lead_tuning shows it is exact to 0.05 cent). baseDiv 2: the organ has a 16' sub.
  const double f0 = midiHz(note) / baseDiv;
  Result r;
  double tot = 0, harm = 0;
  const int guard = 10;   // bins either side of a harmonic that count as that harmonic
  std::vector<char> isHarm(m.size(), 0);
  for (int k = 1; double(k) * f0 < double(fs) / 2; ++k) {
    const long c = std::lround(double(k) * f0 / binHz);
    double mag = 0;
    for (long b = c - guard; b <= c + guard; ++b)
      if (b >= 0 && b < long(m.size())) { isHarm[size_t(b)] = 1; mag = std::max(mag, m[size_t(b)]); }
    r.harm.push_back(mag);
  }
  for (size_t b = size_t(20.0 / binHz); b < m.size(); ++b) {
    tot += m[b] * m[b];
    if (isHarm[b]) harm += m[b] * m[b];
  }
  r.nonHarmDb = 10.0 * std::log10(std::max((tot - harm) / tot, 1e-20));
  r.h1 = r.harm.empty() ? 0 : *std::max_element(r.harm.begin(), r.harm.end());
  r.topHarmHz = 0;
  for (size_t k = 0; k < r.harm.size(); ++k)
    if (r.harm[k] > r.h1 * 0.01) r.topHarmHz = double(k + 1) * f0;
  return r;
}

int main() {
  std::printf("aliasing: non-harmonic energy re total, filter open (dB) / highest harmonic above -40 dB\n");
  const char* names[kNumWaves] = {"SAW", "PULSE", "TRI", "SUPER", "FM1", "FM2", "BELL", "ORGAN"};
  for (float fs : {44100.0f, 32000.0f}) {
    for (int w : {int(kWaveSaw), int(kWavePulse), int(kWaveTri), int(kWaveSuper), int(kWaveFm1), int(kWaveFm2), int(kWaveOrgan)}) {
      std::printf("  %-6s fs %5.0f:", names[w], double(fs));
      for (int note : {36, 48, 60, 72, 84, 91, 96, 103, 108}) {
        const int shape = (w == kWaveFm1 || w == kWaveFm2) ? 127 : (w == kWavePulse ? 40 : (w == kWaveOrgan ? 127 : 0));
        const Result r = measure(fs, w, shape, note, w == kWaveOrgan ? 2.0 : 1.0);
        std::printf(" %d:%.0f/%.1fk", note, r.nonHarmDb, r.topHarmHz / 1000.0);
        const double limit = -50.0;   // wavetables are strictly band-limited; FM by an index cap
        LT_CHECK(r.nonHarmDb < limit, "%s fs %.0f note %d: non-harmonic energy %.1f dB", names[w], double(fs), note, r.nonHarmDb);
      }
      std::printf("\n");
    }
  }
  // The saw must really be a saw (harmonic k at 1/k) right up to its band limit,
  // not a dulled approximation: check a mid and a high note against the ideal.
  for (int note : {60, 96}) {
    const Result r = measure(44100.0f, kWaveSaw, 0, note);
    double worst = 0;
    int checked = 0;
    for (size_t k = 1; k < r.harm.size() && double(k + 1) * midiHz(note) < 13000.0; ++k) {
      worst = std::max(worst, std::fabs(dB(r.harm[k] / r.harm[0]) - dB(1.0 / double(k + 1))));
      ++checked;
    }
    std::printf("  saw note %d: %d harmonics below 13 kHz within %.2f dB of 1/k\n", note, checked, worst);
    LT_CHECK(checked >= 3 && worst < 1.5, "saw harmonics off by %.2f dB", worst);
  }
  // Square: even harmonics absent, odd ones at 1/k.
  {
    const Result r = measure(44100.0f, kWavePulse, 0, 60);
    LT_CHECK(dB(r.harm[1] / r.harm[0]) < -50.0, "square has 2nd harmonic at %.1f dB", dB(r.harm[1] / r.harm[0]));
    LT_CHECK(std::fabs(dB(r.harm[2] / r.harm[0]) - dB(1.0 / 3.0)) < 0.5, "square 3rd harmonic");
    std::printf("  square note 60: h2 %.1f dB, h3 %.2f dB (ideal -9.54)\n", dB(r.harm[1] / r.harm[0]), dB(r.harm[2] / r.harm[0]));
  }
  // Triangle: odd harmonics at 1/k^2.
  {
    const Result r = measure(44100.0f, kWaveTri, 0, 60);
    LT_CHECK(std::fabs(dB(r.harm[2] / r.harm[0]) - dB(1.0 / 9.0)) < 0.5, "triangle 3rd harmonic");
    std::printf("  triangle note 60: h3 %.2f dB (ideal -19.08)\n", dB(r.harm[2] / r.harm[0]));
  }
  return finish("test_lead_alias");
}
