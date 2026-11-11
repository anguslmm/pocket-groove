// Drum voices: robustness. Every parameter to min and max, all-min, all-max
// and random combinations, hits at several notes / velocities, at 44.1 kHz
// and 32 kHz: output must stay finite and bounded (|x| < 2.0), must not be
// silent at any single-knob extreme, and the voice must always go inactive.
// Also changes parameters at random while sounding, and feeds out-of-range
// bytes.
#include <cstdlib>
#include "tests/drums_testutil.h"

using namespace pg::drums;
using dt::Params;

namespace {
dt::Checker T;
uint32_t rngState = 0xC0FFEE11u;
uint32_t rnd() { rngState = rngState * 1664525u + 1013904223u; return rngState >> 8; }

struct Result { float peak; bool finite; bool endsInactive; float jumpAfterOff; };

// A busy little phrase: different notes, velocities, accents, a retrigger and
// a choke, then a long tail.
Result exercise(pg::Voice& v, const Params& p, float fs, float tailSeconds, bool simple) {
  v.init(fs);
  std::vector<dt::Event> ev;
  if (simple) {
    ev = {dt::hit(0.0f, fs, 127, 60)};
  } else {
    ev = {dt::hit(0.00f, fs, 127, 60), dt::hit(0.05f, fs, 127, 60, true), dt::hit(0.30f, fs, 1, 60),
          dt::hit(0.40f, fs, 64, 36),  dt::hit(0.55f, fs, 127, 84, true), dt::hit(0.70f, fs, 100, 0),
          dt::hit(0.85f, fs, 127, 127, true), dt::chokeAt(0.9f, fs), dt::hit(1.0f, fs, 127, 48),
          dt::hit(1.004f, fs, 127, 72), dt::hit(1.2f, fs, 127, 60)};
  }
  std::vector<char> act;
  std::vector<float> x = dt::run(v, p, ev, (simple ? 0.0f : 1.2f) + tailSeconds, fs, &act);
  Result r;
  r.finite = dt::allFinite(x);
  r.peak = dt::peak(x);
  r.endsInactive = !act.back();
  r.jumpAfterOff = 0.0f;
  return r;
}

void sweepVoice(const char* name, pg::Voice& v, const Params& base, float fs, int firstParam = 0) {
  const int n = dt::numParams(v);
  int combos = 0;
  float worstPeak = 0.0f, quietest = 10.0f;
  const char* quietName = "";
  auto runOne = [&](const Params& p, const char* what, bool checkAudible) {
    ++combos;
    // Longest tails in the box: kick decay max with full drive (~15 s).
    const Result r = exercise(v, p, fs, 22.0f, false);
    if (!r.finite) { T.check(false, what); std::printf("    (%s: non-finite)\n", name); }
    if (!(r.peak < 2.0f)) { T.check(false, what); std::printf("    (%s: peak %.3f)\n", name, double(r.peak)); }
    if (!r.endsInactive) { T.check(false, what); std::printf("    (%s: still active after 22 s)\n", name); }
    worstPeak = std::fmax(worstPeak, r.peak);
    if (checkAudible) {
      const Result s = exercise(v, p, fs, 1.0f, true);
      if (s.peak < quietest) { quietest = s.peak; quietName = what; }
    }
  };
  static char label[64][48];
  int li = 0;
  for (int i = firstParam; i < n; ++i) {
    const pg::ParamInfo* pi = v.paramInfo(i);
    for (int k = 0; k < 2; ++k) {
      Params p = base;
      p[i] = k ? pi->max : pi->min;
      std::snprintf(label[li], sizeof label[li], "%s=%s", pi->name, k ? "max" : "min");
      runOne(p, label[li], true);
      li = (li + 1) % 64;
    }
  }
  Params lo = base, hi = base;
  for (int i = firstParam; i < n; ++i) { lo[i] = v.paramInfo(i)->min; hi[i] = v.paramInfo(i)->max; }
  runOne(lo, "all min", false);
  runOne(hi, "all max", false);
  for (int c = 0; c < 24; ++c) {
    Params p = base;
    for (int i = firstParam; i < n; ++i) {
      const pg::ParamInfo* pi = v.paramInfo(i);
      const uint32_t r = rnd() % 4;
      p[i] = r == 0 ? pi->min : (r == 1 ? pi->max : uint8_t(pi->min + rnd() % uint32_t(pi->max - pi->min + 1)));
    }
    runOne(p, "random combination", false);
  }
  std::printf("  %-14s %5.0f Hz: %3d settings, worst peak %.3f, quietest single-knob extreme %.3f (%s)\n", name,
              double(fs), combos, double(worstPeak), double(quietest), quietName);
  T.check(worstPeak < 1.0f, "stays below full scale at every setting");
  T.check(quietest > 0.02f, "no single knob extreme is (near) silent at full velocity");
}

// Parameters wiggling while the voice sounds, plus garbage bytes in every slot.
void automate(const char* name, pg::Voice& v, float fs) {
  v.init(fs);
  const int n = dt::numParams(v);
  Params p = dt::defaults(v);
  float l[pg::kQuantum], r[pg::kQuantum];
  float peak = 0.0f;
  bool finite = true;
  const int quanta = int(6.0f * fs / pg::kQuantum);
  for (int q = 0; q < quanta; ++q) {
    if (q % 3 == 0) p[int(rnd() % uint32_t(n))] = uint8_t(rnd() & 0xFF);   // any byte, in range or not
    if (q % 97 == 0) for (int i = 0; i < pg::kSoundParams; ++i) p[i] = uint8_t(rnd() & 0xFF);
    v.setParams(p.v);
    if (q % 41 == 0) v.noteOn(pg::NoteEvent{uint8_t(rnd() & 0x7F), uint8_t(1 + rnd() % 127), (rnd() & 1) != 0, false, 0});
    if (q % 173 == 5) v.choke();
    if (q % 59 == 7) v.noteOff(60);
    if (!v.active()) continue;
    v.render(l, r, pg::kQuantum);
    for (int i = 0; i < pg::kQuantum; ++i) {
      if (!std::isfinite(l[i]) || !std::isfinite(r[i])) finite = false;
      peak = std::fmax(peak, std::fabs(l[i]));
    }
  }
  std::printf("  %-14s %5.0f Hz: automation peak %.3f\n", name, double(fs), double(peak));
  T.check(finite, "finite under parameter automation");
  T.check(peak < 2.0f, "bounded under parameter automation");
}
}  // namespace

int main() {
  for (float fs : {44100.0f, 32000.0f}) {
    std::printf("== parameter sweep at %.0f Hz ==\n", double(fs));
    KickVoice kick; SnareVoice snare; ClapVoice clap; HatVoice ch(false), oh(true); PercVoice perc;
    sweepVoice("kick", kick, dt::defaults(kick), fs);
    sweepVoice("snare", snare, dt::defaults(snare), fs);
    sweepVoice("clap", clap, dt::defaults(clap), fs);
    sweepVoice("closed hat", ch, dt::defaults(ch), fs);
    sweepVoice("open hat", oh, dt::defaults(oh), fs);
    const char* models[PercVoice::kNumModels] = {"perc tom", "perc rim", "perc cowbell", "perc clave", "perc shaker", "perc zap"};
    for (int m = 0; m < PercVoice::kNumModels; ++m) {
      Params p = dt::defaults(perc);
      p[PercVoice::kModel] = uint8_t(m);
      sweepVoice(models[m], perc, p, fs, 1);  // sweep everything but the model itself
    }
    automate("kick", kick, fs);
    automate("snare", snare, fs);
    automate("clap", clap, fs);
    automate("closed hat", ch, fs);
    automate("open hat", oh, fs);
    automate("perc", perc, fs);
  }
  // Odd sample rates must not break init.
  for (float fs : {8000.0f, 22050.0f, 48000.0f, 96000.0f}) {
    KickVoice kick; SnareVoice snare; ClapVoice clap; HatVoice ch(false), oh(true); PercVoice perc;
    pg::Voice* vs[6] = {&kick, &snare, &clap, &ch, &oh, &perc};
    for (pg::Voice* v : vs) {
      const Result r = exercise(*v, dt::defaults(*v), fs, 8.0f, false);
      T.check(r.finite && r.peak < 2.0f && r.peak > 0.01f && r.endsInactive, "works at other sample rates");
    }
  }
  T.check(dt::monoMismatches() == 0, "every voice writes identical L and R");
  return T.done("test_drums_sweep");
}
