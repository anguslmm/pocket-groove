// zipper_probe: does a slow macro sweep of a filter cutoff step audibly at the
// parameters' 7-bit resolution? (docs/MACROLAB.md, docs/STATE.md; a fact-finding
// tool for the macro redesign, not a test.)
//
//   zipper_probe [OUTDIR]        (default build/zipper)
//
// Bass: a held saw note (A2, RES 110, no filter envelope, no accent, no key
// track, sustain) whose CUT climbs 40 -> 64 one value at a time, like a macro
// moved slowly across a narrow range (24 steps in 4 s, and in 1 s). The
// reference is the same climb done smoothly by the bass's own LFO (shape UP,
// one cycle = the sweep, depth 31 = exactly the 24 CUT steps' 1.465 octaves),
// which moves the cutoff every 16-sample quantum. Lead: a held saw chord note
// (RES 100, no filter envelope, sustain) with CUT 40 -> 64 the same way; it has
// no smooth reference, so it is measured against itself between the steps.
//
// Printed per case: the cutoff jump per CUT step, and the step transient: the
// energy of the first difference of the output (a click detector: it weights
// high frequencies) in the 3 ms after each step, against the same 3 ms of the
// smooth reference (bass) and against 3 ms halfway between steps (both), in dB:
// median and worst over the steps. Also the RMS of stepped - smooth (bass).
// WAVs of every render go to OUTDIR, to listen to.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "common/wav.h"
#include "pg/bass/bass.h"
#include "pg/lead/lead.h"

namespace {

constexpr float kSr = 44100.0f;
constexpr int kC0 = 40, kSteps = 24;

std::vector<float> renderBass(float seconds, bool smooth, std::vector<int>* stepAt, std::vector<float>* cutHz) {
  using namespace pg::bass;
  BassVoice v;
  v.init(kSr);
  // one LFO cycle (2 bars = 8 beats, or 1 bar = 4 beats) = the sweep
  const bool oneBar = seconds < 2.0f;
  v.setTempo((oneBar ? 4.0f : 8.0f) * 60.0f / seconds);
  uint8_t p[pg::kSoundParams] = {};
  for (int i = 0; i < kNumParams; ++i) p[i] = v.paramInfo(i)->def;
  p[kReso] = 110; p[kEnvMod] = 0; p[kAccent] = 0; p[kKeyTrack] = 0; p[kAmpDecay] = 127; p[kSub] = 0;
  p[kWave] = 0; p[kDrive] = 30; p[kCutoff] = kC0;
  p[kLfoShape] = 3;                  // UP: a ramp 0 -> 1 over one cycle
  p[kLfoRate] = oneBar ? 1 : 0;      // 1BAR / 2BAR
  p[kLfoDepth] = smooth ? 31 : 0;    // 31 x 6/127 oct = 24 x 7.75/127 oct
  v.setParams(p);
  v.noteOn(pg::NoteEvent{45, 110, false, false, 0});
  const int quanta = int(seconds * kSr / float(pg::kQuantum));
  std::vector<float> out;
  float l[pg::kQuantum], r[pg::kQuantum];
  int last = kC0;
  for (int q = 0; q < quanta; ++q) {
    if (!smooth) {
      const int c = kC0 + int(float(kSteps) * float(q) / float(quanta) + 0.5f);
      if (c != last && stepAt) stepAt->push_back(q * pg::kQuantum);
      last = c;
      p[kCutoff] = uint8_t(c);
    }
    v.setParams(p);
    v.render(l, r, pg::kQuantum);
    if (cutHz) cutHz->push_back(v.debugCutoffHz());
    out.insert(out.end(), l, l + pg::kQuantum);
  }
  return out;
}

std::vector<float> renderLead(float seconds, std::vector<int>* stepAt) {
  using namespace pg::lead;
  LeadVoice v;
  v.init(kSr);
  v.setMono(true);
  uint8_t p[pg::kSoundParams] = {};
  for (int i = 0; i < kNumParams; ++i) p[i] = v.paramInfo(i)->def;
  p[kPReso] = 100; p[kPEnvAmt] = 0; p[kPSustain] = 127; p[kPDecay] = 127; p[kPVibrato] = 0; p[kPLfoCutoff] = 0;
  p[kPCutoff] = kC0;
  v.setParams(p);
  v.noteOn(pg::NoteEvent{57, 110, false, false, 0});
  const int quanta = int(seconds * kSr / float(pg::kQuantum));
  std::vector<float> out;
  float l[pg::kQuantum], r[pg::kQuantum];
  int last = kC0;
  for (int q = 0; q < quanta; ++q) {
    const int c = kC0 + int(float(kSteps) * float(q) / float(quanta) + 0.5f);
    if (c != last) stepAt->push_back(q * pg::kQuantum);
    last = c;
    p[kPCutoff] = uint8_t(c);
    v.setParams(p);
    v.render(l, r, pg::kQuantum);
    out.insert(out.end(), l, l + pg::kQuantum);
  }
  return out;
}

// energy of the first difference over [a, a + n)
double diffEnergy(const std::vector<float>& x, int a, int n) {
  double e = 0.0;
  for (int i = std::max(a, 1); i < a + n && i < int(x.size()); ++i) {
    const double d = double(x[size_t(i)]) - double(x[size_t(i) - 1]);
    e += d * d;
  }
  return e;
}

// Level (dB) of harmonic h of 110 Hz in a Hann window of n samples at a (Goertzel).
double harmonicDb(const std::vector<float>& x, int a, int n, int h) {
  const double w = 2.0 * 3.14159265358979 * 110.0 * h / double(kSr);
  const double c = 2.0 * std::cos(w);
  double s1 = 0.0, s2 = 0.0;
  for (int i = 0; i < n; ++i) {
    const double hann = 0.5 - 0.5 * std::cos(2.0 * 3.14159265358979 * i / (n - 1));
    const double s0 = double(x[size_t(a + i)]) * hann + c * s1 - s2;
    s2 = s1;
    s1 = s0;
  }
  const double p = s1 * s1 + s2 * s2 - c * s1 * s2;
  return 10.0 * std::log10(p + 1e-20);
}

// The timbre jump across each step: harmonics 1..12 in a window that ends at the step
// against one that starts 1.5 ms after it (the cutoff lag has settled), largest change.
// For the smooth render the same windows measure how far the ramp moves in that time.
std::vector<double> stepJumps(const std::vector<float>& x, const std::vector<int>& steps, int n) {
  std::vector<double> out;
  for (int s : steps) {
    const int a = s - n, b = s + int(0.0015f * kSr);
    if (a < int(0.3f * kSr) || b + n > int(x.size())) continue;
    double m = 0.0;
    for (int h = 1; h <= 12; ++h) m = std::max(m, std::fabs(harmonicDb(x, b, n, h) - harmonicDb(x, a, n, h)));
    out.push_back(m);
  }
  return out;
}

void stats(const char* what, std::vector<double> db) {
  std::sort(db.begin(), db.end());
  if (db.empty()) return;
  std::printf("    %-46s median %+5.1f dB, worst %+5.1f dB\n", what, db[db.size() / 2], db.back());
}

void save(const std::string& dir, const char* name, const std::vector<float>& x) {
  std::vector<float> y(x.size());
  for (size_t i = 0; i < x.size(); ++i) y[i] = x[i] * 0.7f;
  wav::writeFloat((dir + "/" + name).c_str(), y, int(kSr), 1);
}

}  // namespace

int main(int argc, char** argv) {
  const std::string dir = argc > 1 ? argv[1] : "build/zipper";
  ::mkdir(dir.c_str(), 0755);
  const int win = int(0.003f * kSr);
  std::printf("zipper_probe: CUT %d -> %d one value at a time (%d steps); WAVs in %s/\n", kC0, kC0 + kSteps, kSteps,
              dir.c_str());
  for (float secs : {4.0f, 1.0f}) {
    std::vector<int> steps;
    std::vector<float> cutS, cutR;
    const std::vector<float> a = renderBass(secs, false, &steps, &cutS);
    const std::vector<float> b = renderBass(secs, true, nullptr, &cutR);
    char n1[64], n2[64];
    std::snprintf(n1, sizeof(n1), "bass_steps_%.0fs.wav", double(secs));
    std::snprintf(n2, sizeof(n2), "bass_smooth_%.0fs.wav", double(secs));
    save(dir, n1, a);
    save(dir, n2, b);
    const float f0 = cutS[cutS.size() / 4], f1 = cutS[cutS.size() / 4 + size_t(secs * kSr / 16.0f / float(kSteps))];
    std::printf("  bass, %.0f s sweep (a step every %.0f ms): cutoff jump per CUT value %.0f cents (%.0f -> %.0f Hz)\n",
                double(secs), double(secs * 1000.0f / float(kSteps)), 1200.0 * std::log2(double(f1) / double(f0)),
                double(f0), double(f1));
    std::vector<double> vsRef, vsBetween;
    const int gap = int(secs * kSr / float(kSteps));
    for (int s : steps) {
      if (s + gap / 2 + win > int(a.size())) continue;
      const double ea = diffEnergy(a, s, win), eb = diffEnergy(b, s, win), em = diffEnergy(a, s + gap / 2, win);
      vsRef.push_back(10.0 * std::log10((ea + 1e-12) / (eb + 1e-12)));
      vsBetween.push_back(10.0 * std::log10((ea + 1e-12) / (em + 1e-12)));
    }
    stats("step transient vs the smooth sweep, same 3 ms:", vsRef);
    stats("step transient vs 3 ms between steps (same render):", vsBetween);
    double se = 0.0, sr = 0.0;
    for (size_t i = 0; i < a.size() && i < b.size(); ++i) {
      se += double(a[i] - b[i]) * double(a[i] - b[i]);
      sr += double(b[i]) * double(b[i]);
    }
    std::printf("    stepped - smooth: %.1f dB below the signal (the staircase itself)\n", 10.0 * std::log10(se / sr));
    const int n = gap > 4200 ? 2048 : 1024;
    std::vector<double> ja = stepJumps(a, steps, n), jb = stepJumps(b, steps, n);
    std::sort(ja.begin(), ja.end());
    std::sort(jb.begin(), jb.end());
    if (!ja.empty() && !jb.empty())
      std::printf("    timbre across a step (harmonics 1-12, %d-sample windows): median %.1f dB, worst %.1f dB;\n"
                  "      the smooth sweep over the same windows: median %.1f dB, worst %.1f dB\n",
                  n, ja[ja.size() / 2], ja.back(), jb[jb.size() / 2], jb.back());
  }
  for (float secs : {4.0f, 1.0f}) {
    std::vector<int> steps;
    const std::vector<float> a = renderLead(secs, &steps);
    char n1[64];
    std::snprintf(n1, sizeof(n1), "lead_steps_%.0fs.wav", double(secs));
    save(dir, n1, a);
    const float c0 = pg::lead::LeadVoice::cutoffHz(52), c1 = pg::lead::LeadVoice::cutoffHz(53);
    std::printf("  lead, %.0f s sweep: cutoff jump per CUT value %.0f cents (at CUT 52: %.0f -> %.0f Hz)\n",
                double(secs), 1200.0 * std::log2(double(c1) / double(c0)), double(c0), double(c1));
    std::vector<double> vsBetween;
    const int gap = int(secs * kSr / float(kSteps));
    for (int s : steps) {
      if (s + gap / 2 + win > int(a.size())) continue;
      vsBetween.push_back(10.0 * std::log10((diffEnergy(a, s, win) + 1e-12) / (diffEnergy(a, s + gap / 2, win) + 1e-12)));
    }
    stats("step transient vs 3 ms between steps (same render):", vsBetween);
  }
  return 0;
}
