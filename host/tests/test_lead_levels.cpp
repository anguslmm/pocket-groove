// Per-WAVE level balance: at equal settings (default patch, osc1 only, SHAPE
// 0, cutoff open enough not to bias it) every osc1 wave plays a 4-note chord
// within 1.5 dB RMS of SAW, and none peaks above the loudest existing
// setting (PULSE at SHAPE 127, about -9.9 dBFS). See kWaveTrim in lead.cpp.
#include "test_lead_util.h"

using namespace pg::lead;

static void measure(int wave, double& rms, double& pk) {
  double acc = 0;
  long n = 0;
  pk = 0;
  for (int root : {48, 57, 64}) {
    lt::Synth s(44100.0f);
    s.v.setMono(true);
    s.set(kPWave, wave);
    s.set(kPCutoff, 110);
    s.set(kPOscMix, 0);
    s.on(root, 100, pg::kChordMin7);
    s.run(0.6);
    for (float x : s.L) { acc += double(x) * double(x); pk = std::max(pk, double(std::fabs(x))); }
    n += long(s.L.size());
  }
  rms = std::sqrt(acc / double(n));
}

int main() {
  std::printf("lead wave levels (osc1 only, Min7 chords, SHAPE 0)\n");
  const char* names[kNumWaves] = {"SAW", "PULSE", "TRI", "SUPER", "FM1", "FM2", "BELL", "ORGAN"};
  double sawRms = 0, sawPk = 0;
  measure(kWaveSaw, sawRms, sawPk);
  for (int w = 0; w < kNumWaves; ++w) {
    double r, pk;
    measure(w, r, pk);
    const double d = lt::dB(r) - lt::dB(sawRms);
    LT_CHECK(std::fabs(d) <= 1.5, "%s: %+.2f dB RMS re SAW", names[w], d);
    LT_CHECK(lt::dB(pk) <= -9.0, "%s: peak %.2f dBFS above the ceiling", names[w], lt::dB(pk));
    std::printf("  %-6s rms %+5.2f dB re SAW, peak %6.2f dBFS\n", names[w], d, lt::dB(pk));
  }
  return lt::finish("test_lead_levels");
}
