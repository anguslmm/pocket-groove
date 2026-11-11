// The CUT knob sweep, the bass's main live gesture: opening the filter must
// never make the bass darker or make it disappear.
//
// Bass voice alone, a sustained note (MIDI 33 and 45, both sample rates), CUT
// swept 0 -> 127 in steps of 8 for RES {0, 40, 78, 112, 127} x ENV {0, 40, 70,
// 100} x four patches (default, the ACIDJAM demo bass, driven + sub, dry: DRV 0
// SUB 0). Measured over the note's first 0.37 s (envelope included):
//   1. energy above 1 kHz never falls more than 1.5 dB below its running
//      maximum as CUT rises (a resonant peak passing a harmonic may dip a
//      little): for the plain filter (RES 0) all the way, with resonance up to
//      the resonant maximum. A resonant hump over a 1/n saw spectrum carries
//      more energy above 1 kHz than the open filter (+2 dB at RES 40, +5 at 78,
//      +12 at 112, +20 at 127), so once it climbs out of the harmonics the
//      energy settles back towards the open filter; it must never go below
//      the RES 0 sweep at the same CUT (resonance never darkens the sound).
//      Judged where the high band is audible (within 30 dB of the open filter);
//      near self-oscillation (RES >= 112, CUT <= 64) a peak passing a low
//      harmonic may dip up to 4 dB, as it always did;
//   2. at RES 78 the RMS stays within +-4 dB of its CUT 40 value up to 127;
//   3. at CUT 127 with RES <= 40 (dry patch) the spectrum from 300 Hz up to the
//      filter's cap matches a raw saw's within 3 dB per octave band (shape and
//      level relative to the RES 0 render: the filter is effectively open);
//   4. no fixed-frequency whistle at the cap: at CUT 127 the spectrum has no
//      narrow peak near fs/5 standing more than 6 dB above its neighbourhood.
#include <complex>

#include "bass_testutil.h"

using namespace bt;
using pg::bass::BassVoice;
namespace bp = pg::bass;

namespace {

int gFail = 0;
#define SW_CHECK(cond, ...)                         \
  do {                                              \
    if (!(cond)) {                                  \
      if (++gFail <= (std::getenv("SW_ALL") ? 100000 : 25)) {                          \
        std::printf("  FAIL %s:%d: ", __FILE__, __LINE__); \
        std::printf(__VA_ARGS__);                   \
        std::printf("\n");                          \
      }                                             \
    }                                               \
  } while (0)

constexpr int kN = 16384;

void fft(std::vector<std::complex<double>>& a) {
  const size_t n = a.size();
  for (size_t i = 1, j = 0; i < n; ++i) {
    size_t bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) std::swap(a[i], a[j]);
  }
  for (size_t len = 2; len <= n; len <<= 1) {
    const double ang = -2.0 * kPiD / double(len);
    const std::complex<double> wl(std::cos(ang), std::sin(ang));
    for (size_t i = 0; i < n; i += len) {
      std::complex<double> w(1.0, 0.0);
      for (size_t j = 0; j < len / 2; ++j) {
        const std::complex<double> u = a[i + j], v = a[i + j + len / 2] * w;
        a[i + j] = u + v;
        a[i + j + len / 2] = u - v;
        w *= wl;
      }
    }
  }
}

struct Patch {
  const char* name;
  std::vector<std::pair<int, int>> p;
};

struct Spec {
  std::vector<double> pow;   // power per bin, kN/2 + 1
  double rms = 0;
  float sr = 44100.0f;
  double band(double lo, double hi) const {
    double e = 0;
    for (size_t k = 0; k < pow.size(); ++k) {
      const double f = double(k) * double(sr) / kN;
      if (f >= lo && f < hi) e += pow[k];
    }
    return e;
  }
};

Spec render(float sr, const Patch& patch, int res, int env, int cut, int note) {
  Rig r(sr);
  for (const auto& e : patch.p) r.set(e.first, e.second);
  r.set(bp::kReso, res);
  r.set(bp::kEnvMod, env);
  r.set(bp::kCutoff, cut);
  r.set(bp::kAmpDecay, 127);   // sustained
  r.on(note, 100);
  r.run(r.quantaFor(float(kN) / sr) + 1);
  Spec s;
  s.sr = sr;
  std::vector<std::complex<double>> a(kN);
  double e = 0;
  for (int i = 0; i < kN; ++i) {
    const double x = r.out[size_t(i)];
    e += x * x;
    a[size_t(i)] = x * (0.5 - 0.5 * std::cos(2.0 * kPiD * double(i) / double(kN)));
  }
  s.rms = std::sqrt(e / kN);
  fft(a);
  s.pow.resize(kN / 2 + 1);
  for (int k = 0; k <= kN / 2; ++k) s.pow[size_t(k)] = std::norm(a[size_t(k)]);
  return s;
}

double dB10(double e) { return 10.0 * std::log10(e + 1e-30); }

}  // namespace

int main() {
  std::printf("bass CUT sweep: brightness monotonic, level kept, open at the top, no whistle\n");
  // ACIDJAM's bass patch (lib/pg/src/pg/demo/demo_acidjam.cpp).
  const Patch patches[] = {
      {"default", {}},
      {"acidjam", {{bp::kCutoff, 52}, {bp::kReso, 112}, {bp::kEnvMod, 66}, {bp::kDecay, 50}, {bp::kAccent, 104},
                   {bp::kDrive, 52}, {bp::kSub, 34}, {bp::kSlide, 58}, {bp::kAmpDecay, 96}, {bp::kKeyTrack, 50}}},
      {"drive+sub", {{bp::kDrive, 100}, {bp::kSub, 90}}},
      {"dry", {{bp::kDrive, 0}, {bp::kSub, 0}}},
  };
  double worstDip = 0, worstUnder = 0, worstRms = 0, worstOpen = 0, worstWhistle = -99;
  for (float sr : {44100.0f, 32000.0f}) {
    for (int note : {33, 45}) {
      for (const Patch& patch : patches) {
        for (int res : {0, 40, 78, 112, 127}) {
          for (int env : {0, 40, 70, 100}) {
            static double refByEnv[128];
            static double hi0[128][17];   // RES 0 curve of this patch/note/rate, per ENV
            double hiv[17];
            int cutv[17];
            double rms40 = 0;
            int ci = 0;
            for (int cut = 0; cut <= 128; cut += 8, ++ci) {
              const int c = cut > 127 ? 127 : cut;
              const Spec s = render(sr, patch, res, env, c, note);
              const double hi = dB10(s.band(1000.0, 0.5 * double(sr)));
              hiv[ci] = hi;
              cutv[ci] = c;
              if (res == 0) hi0[env][ci] = hi;
              const double r = 20.0 * std::log10(s.rms + 1e-30);
              if (c == 40) rms40 = r;
              if (res == 78 && c > 40) {
                const double d = std::fabs(r - rms40);
                if (d > worstRms) worstRms = d;
                SW_CHECK(d <= 4.0, "fs %.0f note %d %s RES 78 ENV %d: RMS at CUT %d is %+.1f dB re CUT 40", double(sr), note,
                         patch.name, env, c, r - rms40);
              }
              if (c == 127) {
                // 4. whistle: a narrow peak near the cap (fs/5)?
                const double cap = 0.2 * double(sr);
                const int k0 = int(cap * 0.9 * kN / double(sr)), k1 = int(cap * 1.1 * kN / double(sr));
                double pk = 0, nb = 0;
                for (int k = k0; k <= k1; ++k) pk = std::max(pk, s.pow[size_t(k)]);
                for (int k = int(cap * 0.6 * kN / double(sr)); k < int(cap * 0.8 * kN / double(sr)); ++k) nb += s.pow[size_t(k)];
                nb /= double(int(cap * 0.8 * kN / double(sr)) - int(cap * 0.6 * kN / double(sr)));
                const double prom = dB10(pk) - dB10(nb);
                // Harmonic lines stand ~10-15 dB over the inter-harmonic floor of a
                // plain saw anyway: compare with the same measure on RES 0.
                if (res == 0) refByEnv[env] = prom;
                const double w = prom - refByEnv[env];
                if (w > worstWhistle) worstWhistle = w;
                SW_CHECK(res == 0 || w <= 6.0, "fs %.0f note %d %s RES %d ENV %d: peak near fs/5 stands %.1f dB above RES 0's",
                         double(sr), note, patch.name, res, env, w);
              }
            }
            // 1. Brightness. The plain filter (RES 0): never more than 1.5 dB
            //    below the best so far, all the way. With resonance: the same
            //    up to the sweep's resonant maximum; beyond it the hump leaves
            //    the band and the energy settles back, but never below the
            //    plain filter at the same CUT.
            {
              int iMax = 0;
              for (int i = 1; i < ci; ++i)
                if (hiv[i] > hiv[iMax]) iMax = i;
              // Only where the high band is audible at all: within 30 dB of the
              // open filter (RES 0, CUT 127). Further down it is the residue of
              // a 24 dB/oct slope, where a resonant peak landing on or between
              // harmonics moves -70 dB levels by several dB.
              const double floor = hi0[env][ci - 1] - 30.0;
              double runMax = -1e9;
              for (int i = 0; i < ci; ++i) {
                if (hiv[i] < floor && (res == 0 ? hi0[env][i] : hiv[i]) < floor) { runMax = std::max(runMax, hiv[i]); continue; }
                if (res == 0 || i <= iMax) {
                  const double dip = runMax - hiv[i];
                  // Near self-oscillation (RES >= 112) the peak is narrow and
                  // strong: passing a low harmonic in the lower half of the
                  // knob dips up to ~4 dB (unchanged, original behaviour).
                  const double tol = (res >= 112 && cutv[i] <= 64) ? 4.0 : 1.5;
                  if (dip > worstDip && tol == 1.5) worstDip = dip;
                  SW_CHECK(dip <= tol, "fs %.0f note %d %s RES %d ENV %d: >1 kHz energy at CUT %d is %.1f dB below the best below it",
                           double(sr), note, patch.name, res, env, cutv[i], dip);
                }
                if (res > 0) {
                  const double under = hi0[env][i] - hiv[i];
                  if (under > worstUnder) worstUnder = under;
                  SW_CHECK(under <= 1.5, "fs %.0f note %d %s RES %d ENV %d: >1 kHz energy at CUT %d is %.1f dB below RES 0's",
                           double(sr), note, patch.name, res, env, cutv[i], under);
                }
                runMax = std::max(runMax, hiv[i]);
              }
            }
          }
        }
        // 3. open at the top: dry patch, CUT 127, RES 0 and 40 vs a raw saw's spectrum.
        if (std::string(patch.name) == "dry") {
          const double f0 = 440.0 * std::pow(2.0, (note - 69) / 12.0);
          const double cap = 0.2 * double(sr);
          double open0 = 0;
          for (int res : {0, 40}) {
            const Spec s = render(sr, patch, res, 0, 127, note);
            // level above 300 Hz: RES 40 as open as RES 0
            const double above = dB10(s.band(300.0, cap));
            if (res == 0) open0 = above;
            SW_CHECK(std::fabs(above - open0) <= 3.0, "fs %.0f note %d RES %d CUT 127: %+.1f dB above 300 Hz re RES 0",
                     double(sr), note, res, above - open0);
            worstOpen = std::max(worstOpen, std::fabs(above - open0));
            // raw saw: harmonic n has power ~ 1/n^2. Compare octave-band shapes
            // from 300 Hz to the cap, both normalised to the 300-600 Hz band.
            double refBase = 0, sBase = s.band(300.0, 600.0);
            for (int n = 1; n * f0 < 600.0; ++n)
              if (n * f0 >= 300.0) refBase += 1.0 / double(n * n);
            for (double lo = 600.0; lo * 1.5 < cap; lo *= 2.0) {
              const double hi = std::min(2.0 * lo, cap);
              double ref = 0;
              for (int n = 1; n * f0 < hi; ++n)
                if (n * f0 >= lo) ref += 1.0 / double(n * n);
              const double d = (dB10(s.band(lo, hi)) - dB10(sBase)) - (dB10(ref) - dB10(refBase));
              worstOpen = std::max(worstOpen, std::fabs(d));
              SW_CHECK(std::fabs(d) <= 3.0, "fs %.0f note %d RES %d CUT 127: band %.0f-%.0f Hz is %+.1f dB re a raw saw",
                       double(sr), note, res, lo, hi, d);
            }
          }
        }
      }
    }
  }
  std::printf("  worst brightness dip %.2f dB, worst shortfall under RES 0 %.2f dB (limits 1.5), worst RES-78 level change %.2f dB (limit 4), "
              "open-filter shape error %.2f dB (limit 3), worst cap peak over RES 0 %.1f dB (limit 6)\n",
              worstDip, worstUnder, worstRms, worstOpen, worstWhistle);
  std::printf("test_bass_sweep: %s (%d failure%s)\n", gFail ? "FAILED" : "passed", gFail, gFail == 1 ? "" : "s");
  return gFail ? 1 : 0;
}
