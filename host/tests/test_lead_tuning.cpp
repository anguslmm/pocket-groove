// Tuning across the keyboard for every oscillator flavour, osc2 interval and
// detune, and the chord interval table (spectral peaks).
#include "test_lead_util.h"

using namespace lt;
using namespace pg::lead;

static double worstCents(float fs, int wave, bool osc2, int interval, const char* label) {
  double worst = 0;
  const size_t n = 65536;
  for (int note = 36; note <= 96; ++note) {
    Synth s(fs);
    openFilter(s);
    s.set(kPWave, wave);
    s.set(kPShape, wave == kWavePulse ? 30 : 0);
    s.set(kPDetune, 0);
    s.set(kPWidth, 0);
    s.set(kPOscMix, osc2 ? 127 : 0);
    s.set(kPOsc2Wave, kOsc2Saw);
    s.set(kPInterval, kIntervalCentre + interval);
    s.on(note);
    s.run(0.1 + double(n) / double(fs));
    const double want = midiHz(note + (osc2 ? interval : 0));
    const double got = measureHz(s.L, fs, size_t(0.08 * double(fs)), n, want);
    const double c = cents(got, want);
    worst = std::max(worst, std::fabs(c));
    LT_CHECK(std::fabs(c) < 1.5, "%s fs %.0f note %d: %.3f Hz, want %.3f (%.2f cents)", label, double(fs), note, got, want, c);
  }
  std::printf("  %-22s fs %5.0f: worst error %.3f cents over MIDI 36..96\n", label, double(fs), worst);
  return worst;
}

int main() {
  std::printf("tuning\n");
  const char* names[kNumWaves] = {"SAW", "PULSE", "TRI", "SUPER", "FM1", "FM2", "BELL", "ORGAN"};
  for (float fs : {44100.0f, 32000.0f})
    for (int w = 0; w < kNumWaves; ++w) worstCents(fs, w, false, 0, names[w]);
  worstCents(44100.0f, kWaveSaw, true, 0, "osc2 saw");
  worstCents(44100.0f, kWaveSaw, true, 7, "osc2 saw +7");
  worstCents(44100.0f, kWaveSaw, true, -12, "osc2 saw -12");

  // Detune: osc1 and osc2 sit symmetrically around the note.
  {
    Synth s;
    sinePatch(s);
    s.set(kPWave, kWaveTri);
    s.set(kPOsc2Wave, kOsc2Tri);
    s.set(kPOscMix, 64);
    s.set(kPDetune, 127);
    s.on(81);
    s.run(1.7);
    const std::vector<double> m = spectrum(s.L, 4410, 65536);
    const std::vector<Peak> pk = findPeaks(m, 44100.0 / 65536.0, 0.01);
    LT_CHECK(pk.size() >= 2, "detune: expected two peaks");
    if (pk.size() >= 2) {
      const double lo = std::min(pk[0].hz, pk[1].hz), hi = std::max(pk[0].hz, pk[1].hz);
      const double d = double(LeadVoice::detuneCents(127));
      std::printf("  detune 127: peaks at %+.2f and %+.2f cents (mapped distance %.1f)\n", cents(lo, midiHz(81)), cents(hi, midiHz(81)), d);
      LT_CHECK(std::fabs(cents(lo, midiHz(81)) + d / 2) < 1.5 && std::fabs(cents(hi, midiHz(81)) - d / 2) < 1.5, "detune not symmetric");
    }
  }

  // Chords: every ChordType must produce exactly its interval set.
  std::printf("chords (root 60, sine patch):\n");
  const char* cn[pg::kChordNum] = {"None", "Oct", "Fifth", "Maj", "Min", "Maj7", "Min7", "Dom7", "Sus4", "Min9"};
  for (int c = 0; c < pg::kChordNum; ++c) {
    int8_t iv[kMaxChordNotes];
    const int n = LeadVoice::chordIntervals(uint8_t(c), iv);
    LT_CHECK(n >= 1 && n <= kMaxChordNotes, "chord %d size %d", c, n);
    LT_CHECK((c == pg::kChordNone) == (n == 1), "only kChordNone is a single note");
    Synth s;
    sinePatch(s);
    s.on(60, 127, c);
    s.run(1.7);
    const std::vector<double> m = spectrum(s.L, 4410, 65536);
    double mx = 0;
    for (double v : m) mx = std::max(mx, v);
    std::vector<Peak> pk = findPeaks(m, 44100.0 / 65536.0, mx * 0.03);
    std::sort(pk.begin(), pk.end(), [](const Peak& a, const Peak& b) { return a.hz < b.hz; });
    std::string got;
    bool ok = int(pk.size()) == n;
    for (size_t i = 0; i < pk.size(); ++i) {
      const double semis = cents(pk[i].hz, midiHz(60)) / 100.0;
      char buf[32];
      std::snprintf(buf, sizeof buf, "%s%.2f", i ? " " : "", semis);
      got += buf;
      if (i < size_t(n) && std::fabs(semis - iv[i]) > 0.03) ok = false;
    }
    std::string want;
    for (int i = 0; i < n; ++i) want += (i ? " " : "") + std::to_string(int(iv[i]));
    std::printf("  %-6s want {%s}  measured {%s}\n", cn[c], want.c_str(), got.c_str());
    LT_CHECK(ok, "chord %s intervals wrong", cn[c]);
    LT_CHECK(s.v.soundingVoices() == n, "chord %s uses %d voices", cn[c], s.v.soundingVoices());
  }
  // Out-of-range chord type behaves as a single note; notes above 127 clamp.
  {
    Synth s;
    s.on(60, 127, 200);
    s.run(0.01);
    LT_CHECK(s.v.soundingVoices() == 1, "bad chord type should be a single note");
    Synth t;
    t.on(127, 127, pg::kChordMin9);
    t.run(0.05);
    LT_CHECK(allFinite(t.L), "top-of-range chord not finite");
  }
  return finish("test_lead_tuning");
}
