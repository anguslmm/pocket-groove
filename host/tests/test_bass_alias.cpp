// Aliasing check: play high notes with the filter open and compare the energy
// that is NOT on a harmonic of the note against the fundamental.
#include "bass_testutil.h"

using namespace bt;
using namespace pg::bass;

namespace {
struct Result { double fundDb, worstDb, worstHz, totalDb; };

// Steady-state spectrum split into "on a harmonic" / "everything else".
Result measure(Rig& rig, double f0) {
  const size_t n = 65536;
  rig.out.clear();
  rig.runSeconds(0.3f + float(n) / rig.sr);
  const size_t start = rig.out.size() - n;
  std::vector<double> m = spectrum(rig.out, start, n);
  const double binHz = double(rig.sr) / double(n);
  // Refine f0 on the real signal so the harmonic mask sits exactly right.
  // Refine on the strongest harmonic (the fundamental may be absent).
  {
    size_t best = size_t(0.5 * f0 / binHz);
    for (size_t b = best; b < m.size(); ++b) if (m[b] > m[best]) best = b;
    const double h = std::fmax(1.0, std::round(double(best) * binHz / f0));
    f0 = findPeakFreq(rig.out, start, n, h * f0, rig.sr, 0.3 / h) / h;
  }
  const int guard = 7;   // BH4 main lobe is +-4 bins
  std::vector<char> harm(m.size(), 0);
  for (int k = 1; double(k) * f0 < double(rig.sr) * 0.5 + 10.0 * binHz; ++k) {
    const long c = lround(double(k) * f0 / binHz);
    for (long b = c - guard; b <= c + guard; ++b)
      if (b >= 0 && b < long(m.size())) harm[size_t(b)] = 1;
  }
  // Reference = strongest harmonic. For the clean oscillators that is the
  // fundamental; a folded saw can move almost all energy to a higher harmonic
  // (at exactly 1.5 fold cycles it is a pure 3rd-harmonic sine).
  double fund = 0;
  for (size_t b = 0; b < m.size(); ++b)
    if (harm[b]) fund = std::fmax(fund, m[b]);
  double worst = 0, worstHz = 0, total = 0;
  for (size_t b = size_t(25.0 / binHz); b < m.size(); ++b) {
    if (harm[b]) continue;
    total += m[b] * m[b];
    if (m[b] > worst) { worst = m[b]; worstHz = double(b) * binHz; }
  }
  // Each sinusoid spreads over the window: sum of squares over the main lobe
  // is ~2.0x the peak squared for BH4 (ENBW 2.0 bins). Normalise.
  total = std::sqrt(total / 2.0);
  return {dB(fund), dB(worst / fund), worstHz, dB(total / fund)};
}
}  // namespace

int main() {
  Checker ck;
  struct Case { const char* name; float sr; int note; int tune; int wave; int drive; int fmMix; int fold; double limWorst, limTotal; int reso = 0; };
  const Case cases[] = {
      // clean oscillator, filter wide open: the >= 50 dB requirement
      {"saw    C5", 44100, 72, 24, 0, 0, 0, 0, -55, -50},
      {"square C5", 44100, 72, 24, 64, 0, 0, 0, -55, -50},
      {"pulse  C5", 44100, 72, 24, 127, 0, 0, 0, -55, -50},
      {"saw    A4", 44100, 69, 24, 0, 0, 0, 0, -55, -50},
      {"saw    C4", 44100, 60, 24, 0, 0, 0, 0, -55, -50},
      {"saw    C6 (tune +12)", 44100, 72, 36, 0, 0, 0, 0, -50, -45},
      {"saw    C5 @32k", 32000, 72, 24, 0, 0, 0, 0, -55, -50},
      {"square C5 @32k", 32000, 72, 24, 64, 0, 0, 0, -55, -50},
      {"FM     C5", 44100, 72, 24, 0, 0, 127, 0, -50, -48},
      // RES 50 flattens the ladder's passband right up to the cutoff (8.6 kHz): the most open setting
      {"saw    C5 res 50", 44100, 72, 24, 0, 0, 0, 0, -55, -50, 50},
      {"square C5 res 50", 44100, 72, 24, 64, 0, 0, 0, -55, -50, 50},
      {"saw    C5 res 50 @32k", 32000, 72, 24, 0, 0, 0, 0, -55, -50, 50},
      // informational: nonlinear stages on a high note (limits are loose)
      {"saw    C5 drive 64", 44100, 72, 24, 0, 64, 0, 0, -30, -25},
      {"saw    C5 drive 127", 44100, 72, 24, 0, 127, 0, 0, -25, -20},
      {"saw    C3 drive 127", 44100, 48, 24, 0, 127, 0, 0, -35, -30},
      // wavefolder: folded in the naive domain, edges band-limited with folded-jump BLEPs
      {"FM     C3 fold 127", 44100, 48, 24, 0, 0, 127, 127, -60, -55},
      {"saw    C3 fold 127", 44100, 48, 24, 0, 0, 0, 127, -60, -55},
      {"saw    C5 fold 127", 44100, 72, 24, 0, 0, 0, 127, -60, -55},
      {"pulse  C4 fold 127", 44100, 60, 24, 100, 0, 0, 127, -60, -55},
      {"saw    C4 fold 127 @32k", 32000, 60, 24, 0, 0, 0, 127, -55, -50},
      {"FM+saw C2 fold 127", 44100, 36, 24, 0, 0, 64, 127, -55, -45},
  };
  for (const Case& c : cases) {
    Rig rig(c.sr);
    rig.set(kCutoff, 127);
    rig.set(kReso, c.reso);
    rig.set(kEnvMod, 0);
    rig.set(kKeyTrack, 0);
    rig.set(kSub, 0);
    rig.set(kAmpDecay, 127);
    rig.set(kTimbreEnv, 0);
    rig.set(kDrive, c.drive);
    rig.set(kWave, c.wave);
    rig.set(kTune, c.tune);
    rig.set(kFmMix, c.fmMix);
    rig.set(kFmDepth, 127);
    rig.set(kFold, c.fold);
    rig.on(c.note, 127);
    Result r = measure(rig, midiHz(c.note + c.tune - 24));
    ck.check(r.worstDb < c.limWorst && r.totalDb < c.limTotal,
             "%-22s ref harmonic %6.1f dBFS | worst inharmonic %6.1f dBc @ %7.0f Hz (limit %g) | total inharmonic %6.1f dBc (limit %g)",
             c.name, r.fundDb, r.worstDb, r.worstHz, c.limWorst, r.totalDb, c.limTotal);
  }
  return ck.result("test_bass_alias");
}
