// Drum voices: CPU benchmark (ns per sample on the host) and a denormal /
// tail-cost check. The ESP32-S3 estimate printed next to each voice comes
// from counting operations in the inner loops (see the table below); the
// host number is there to catch regressions.
//
// Operation counts per sample, worst case (attack phase), and the cycle
// estimate used: float add/sub/mul ~2.5 cycles each (ARCHITECTURE.md: a
// multiply-add is 4-6), integer op ~1, int->float ~2, float select ~3, plus
// ~6 cycles per sample for loop/load/store overhead and ~4 for the
// per-quantum work (virtual call, param compare, L->R copy) spread over 16.
//
//   voice        float ops  int ops  selects   est. cycles/sample
//   kick             31        3        2          ~100   (after the click: 24 flops, ~80)
//   snare            43        3        2          ~130
//   clap             24        3        2          ~ 80
//   closed hat       21       22        2          ~ 95
//   open hat         21       22        2          ~ 95
//   perc (tonal)     34        3        2          ~105   (cowbell ~80, shaker ~90)
//   ---------------------------------------------------------------
//   all six                                        ~605 worst case, all in attack
#include <chrono>
#include "tests/drums_testutil.h"

using namespace pg::drums;
using dt::Params;

namespace {
dt::Checker T;

// Keep the voice sounding the whole time (retrigger every `periodQ` quanta)
// and time render() + setParams() only.
double bench(pg::Voice& v, const Params& p, float fs, int periodQ, float* checksum) {
  v.init(fs);
  float l[pg::kQuantum], r[pg::kQuantum];
  const int quanta = 200000;  // 3.2 M samples
  float acc = 0.0f;
  long rendered = 0;
  const auto t0 = std::chrono::steady_clock::now();
  for (int q = 0; q < quanta; ++q) {
    v.setParams(p.v);
    if (q % periodQ == 0) v.noteOn(pg::NoteEvent{60, 110, (q / periodQ) % 4 == 0, false, 0});
    if (!v.active()) continue;
    v.render(l, r, pg::kQuantum);
    acc += l[3] + r[11];
    ++rendered;
  }
  const auto t1 = std::chrono::steady_clock::now();
  *checksum += acc;
  const double ns = double(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
  return rendered ? ns / (double(rendered) * pg::kQuantum) : 0.0;
}
}  // namespace

int main() {
  const float fs = 44100.0f;
  KickVoice kick; SnareVoice snare; ClapVoice clap; HatVoice ch(false), oh(true); PercVoice perc;
  struct Case { const char* name; pg::Voice* v; Params p; int periodQ; int estCycles; bool inKit; };
  auto pm = [&](int m) { Params p = dt::defaults(perc); p[PercVoice::kModel] = uint8_t(m); return p; };
  // Retrigger periods chosen so each voice is still sounding when the next hit lands.
  std::vector<Case> cases = {
      {"kick", &kick, dt::defaults(kick), 600, 100, true},
      {"snare", &snare, dt::defaults(snare), 600, 130, true},
      {"clap", &clap, dt::defaults(clap), 600, 80, true},
      {"closed hat", &ch, dt::defaults(ch), 300, 95, true},
      {"open hat", &oh, dt::defaults(oh), 1200, 95, true},
      {"perc rim (tonal)", &perc, pm(PercVoice::kRim), 200, 105, true},
      {"perc tom (tonal)", &perc, pm(PercVoice::kTom), 600, 105, false},
      {"perc cowbell", &perc, pm(PercVoice::kCowbell), 600, 80, false},
      {"perc shaker", &perc, pm(PercVoice::kShaker), 300, 90, false},
  };
  float checksum = 0.0f;
  double total = 0.0;
  int totalEst = 0;
  std::printf("== benchmark: ns per sample while sounding (host), ESP32-S3 estimate from op counts ==\n");
  for (Case& c : cases) {
    double best = 1e9;
    for (int rep = 0; rep < 3; ++rep) best = std::fmin(best, bench(*c.v, c.p, fs, c.periodQ, &checksum));
    std::printf("  %-18s %6.2f ns/sample   est. %3d cycles/sample on ESP32-S3\n", c.name, best, c.estCycles);
    if (c.inKit) { total += best; totalEst += c.estCycles; }
    T.check(best < 60.0, "voice costs less than 60 ns/sample on the host");
  }
  std::printf("  %-18s %6.2f ns/sample   est. %3d cycles/sample (budget ~600 of 5400)\n", "six drums together", total, totalEst);
  T.check(total < 150.0, "six drums cost less than 150 ns/sample on the host");

  // Denormal check: a long, quiet tail must not get slower than the loud part.
  std::printf("== tail cost (denormal check): ns per sample over the last second before the voice goes quiet ==\n");
  {
    struct Tail { const char* name; pg::Voice* v; Params p; };
    Params kp = dt::defaults(kick);
    kp[KickVoice::kDecay] = 127; kp[KickVoice::kSweep] = 0; kp[KickVoice::kClickTone] = 0;
    Params sp = dt::defaults(snare);
    sp[SnareVoice::kDecay] = 127; sp[SnareVoice::kBody] = 0;
    Params op = dt::defaults(oh);
    op[HatVoice::kDecay] = 127;
    Params cp = dt::defaults(clap);
    cp[ClapVoice::kDecay] = 127;
    Params tp = pm(PercVoice::kTom);
    tp[PercVoice::kDecay] = 127;
    std::vector<Tail> tails = {{"kick", &kick, kp}, {"snare", &snare, sp}, {"clap", &clap, cp}, {"open hat", &oh, op}, {"perc tom", &perc, tp}};
    for (Tail& t : tails) {
      t.v->init(fs);
      t.v->setParams(t.p.v);
      t.v->noteOn(pg::NoteEvent{60, 127, false, false, 0});
      float l[pg::kQuantum], r[pg::kQuantum];
      std::vector<double> perQ;
      while (t.v->active() && perQ.size() < 2000000) {
        const auto a = std::chrono::steady_clock::now();
        for (int k = 0; k < 8 && t.v->active(); ++k) { t.v->render(l, r, pg::kQuantum); checksum += l[0]; }
        const auto b = std::chrono::steady_clock::now();
        perQ.push_back(double(std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count()) / (8.0 * pg::kQuantum));
      }
      const size_t n = perQ.size(), win = size_t(fs / (8 * pg::kQuantum));
      auto avg = [&](size_t from, size_t to) { double s = 0; for (size_t i = from; i < to; ++i) s += perQ[i]; return s / double(to - from); };
      if (n < 3 * win) { T.check(false, "tail long enough to measure"); continue; }
      const double head = avg(win / 4, win), tail = avg(n - win, n);
      std::printf("  %-10s sounded %5.2f s: early %6.2f ns/sample, last second %6.2f ns/sample\n", t.name,
                  double(n) * 8.0 * pg::kQuantum / double(fs), head, tail);
      T.check(tail < 2.5 * head + 5.0, "no denormal slowdown in the tail");
    }
  }
  std::printf("(checksum %g)\n", double(checksum));
  return T.done("test_drums_bench");
}
