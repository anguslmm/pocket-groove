// Benchmark: ns per sample on the host for typical and worst-case patches.
// (ESP32-S3 is roughly 15-25x slower for this kind of code; the budget of
// 500 cycles/sample at 240 MHz is 2083 ns/sample on the target.)
//
// Op-count estimate for the ESP32-S3 (ARCHITECTURE.md prices: MAC ~5 cycles,
// interpolated table read ~15, divide ~40), per sample, everything enabled:
//   4 sine-table reads (FM mod, FM carrier, folder, sub)            ~60
//   naive osc + FM index + fold scaling + crossfade + ring            ~45
//   BLEP edge events (16 taps + folded pre-value), amortised at C5     ~5
//   ZDF diode ladder (~30 flops incl. input saturator)               ~150
//   post drive with ADAA (antiderivative + 1 divide)                 ~115
//   DC/crossover high-pass, sub mix, VCA ramp                         ~35
//   control rate (glide, envs, LFO, fastTan + 6 divides) / 16         ~75
//   total                                                           ~485 cycles  (budget 500)
// The host-ratio estimate (50 ns x 15..25 at 240 MHz) gives 180..300 cycles,
// so the op count is the pessimistic bound.
#include <chrono>

#include "bass_testutil.h"

using namespace bt;
using namespace pg::bass;

namespace {
double bench(const char* name, Rig& rig, int note, bool retrig) {
  float l[16], r[16];
  const int quanta = int(20.0f * rig.sr / 16.0f);
  double best = 1e30;
  volatile float sink = 0;
  for (int rep = 0; rep < 5; ++rep) {
    rig.on(note, 127, true);
    const auto t0 = std::chrono::steady_clock::now();
    for (int q = 0; q < quanta; ++q) {
      rig.v.setParams(rig.p);
      if (retrig && (q & 511) == 0) rig.on(note + ((q >> 9) % 12), 127, (q >> 9) & 1, (q >> 10) & 1);
      rig.v.render(l, r, 16);
      sink = sink + l[7];
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / (double(quanta) * 16.0);
    best = std::fmin(best, ns);
  }
  std::printf("  %-44s %7.2f ns/sample  (%.2f%% of one host core at %.0f Hz)\n", name, best, best * double(rig.sr) * 1e-7, double(rig.sr));
  return best;
}
}  // namespace

int main() {
  Checker ck;
  double worst = 0;
  {
    Rig rig; rig.set(kAmpDecay, 127);
    worst = std::fmax(worst, bench("default patch, A1", rig, 33, true));
  }
  {
    Rig rig; rig.set(kAmpDecay, 127);
    worst = std::fmax(worst, bench("default patch, C5 (many BLEP events)", rig, 72, true));
  }
  {
    Rig rig; rig.set(kAmpDecay, 127);
    rig.set(kWave, 100); rig.set(kFmMix, 64); rig.set(kFmDepth, 127); rig.set(kFold, 127); rig.set(kSub, 127);
    rig.set(kDrive, 127); rig.set(kReso, 120); rig.set(kLfoDepth, 100); rig.set(kLfoRate, 9); rig.set(kLfoTimbre, 127);
    worst = std::fmax(worst, bench("everything on (pulse+FM+fold+drive+LFO), A1", rig, 33, true));
    worst = std::fmax(worst, bench("everything on, C5 + tune +12", rig, 72, true));
  }
  {
    Rig rig(32000.0f); rig.set(kAmpDecay, 127);
    worst = std::fmax(worst, bench("default patch, A1, 32 kHz", rig, 33, true));
  }
  ck.check(worst < 100.0, "worst case %.2f ns/sample (regression limit 100; target budget ~ 83..139 ns at 15-25x)", worst);
  return ck.result("test_bass_bench");
}
