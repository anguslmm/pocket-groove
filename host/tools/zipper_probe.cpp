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
//
// THROUGH THE ENGINE (stage B, macro motion; engine.h "MACRO MOTION"):
//
//   zipper_probe --engine [--check] [OUTDIR]
//
// A real set's macro is swept through the real command path at the device's
// key-repeat cadence (a held key: +2 units every 40 ms, run in 33 ms UI frames,
// so SetMacro commands arrive 33 or 66 ms apart), and rendered three ways from
// the same start: "steps" = every SetMacro a jump (the 7-bit path, i.e. how
// stage A sounded), "glide" = the default ramps (what the UI does now), "ideal"
// = one continuous linear ramp over the whole sweep. Cases: bass ACID OPEN
// across its resonance peak on a held A2; lead HANDS OPEN (PUSH up) on a held
// A3; kick BOOM SIZE small -> big during a 1/16 roll. Measured:
//   path     every case: the voice's own value of the key parameter per quantum
//            (bass: the cutoff after its lag, lead: the smoothed CUT, kick: the
//            body pitch), cents; the largest change in one quantum during the
//            sweep. A staircase moves in bursts, a glide at the sweep's speed.
//   step     synths, on the track's stem (the engine's stem tap): the largest change
//            of the features (bass: harmonics 1-12 of the note; lead: octave bands,
//            because its two detuned saws beat) across each 7-bit step instant of
//            the steps render (a window ending at the step against one starting
//            1.5 ms after), median over the steps, for all three renders
//   flux     synths, stem: per 5.8 ms hop the largest change of any feature, 99th
//            percentile over the sweep (a staircase concentrates the change)
//   click    drums, stem: the largest sample step between hits (dBFS)
// --check fails (exit 1) unless the glide is as smooth as the ideal (limits below
// "Pass limits") and the steps render is measurably worse, so the probe still
// sees the staircase it guards against. On the lead the audio features are
// reported but not checked: the saws' beating moves them more than a 7-bit step.
// WAVs: OUTDIR/motion_<case>_<steps|glide|ideal>.wav.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "common/wav.h"
#include "pg/bass/bass.h"
#include "pg/engine/engine_hooks.h"
#include "pg/engine/stem_tap.h"
#include "pg/instrument/instrument.h"
#include "pg/lead/lead.h"
#include "pg/macro/macro.h"

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

// ============================================================ through the engine ====

pg::Instrument g_inst;

enum class Mode { Steps, Glide, Ideal };
const char* const kModeName[3] = {"steps", "glide", "ideal"};

struct Case {
  const char* name;
  int track;
  const char* set;
  int macro;              // the swept macro
  uint8_t pos[4];         // the macros' positions (the swept one's is `from`)
  int from, to;           // swept positions (bytes)
  int note;               // held note (synths) / roll note (drums)
  int keyParam;           // the sound parameter whose 7-bit steps are measured
};

// Device cadence: a held key repeats every 40 ms, run by 33 ms UI frames.
constexpr float kPreSec = 0.6f, kTailSec = 0.4f, kRepeatSec = 0.040f, kFrameSec = 0.033f;
constexpr int kUnits = 2;   // per repeat (the slowest held-key speed)

struct Render {
  std::vector<float> stem;
  std::vector<long> steps;   // frames where the key parameter's 7-bit value changed
  std::vector<double> traj;  // per quantum, the voice's own value of the key parameter, cents
  long sweepFrom = 0, sweepTo = 0;
  double hitLen = 0.0;       // samples per 1/16 at the project's tempo (roll hits)
};

Render renderCase(const Case& c, Mode mode) {
  using namespace pg;
  const int repeats = (c.to - c.from) / kUnits;
  const float sweepSec = float(repeats) * kRepeatSec;
  const long total = long((kPreSec + sweepSec + kTailSec) * kSr) / kQuantum * kQuantum;
  Render out;
  std::vector<float> l(size_t(total), 0.0f), r(size_t(total), 0.0f);
  engine_detail::StemTap tap;
  tap.track[c.track][0] = l.data();
  tap.track[c.track][1] = r.data();
  tap.capacity = total;
  engine_detail::setStemTap(&tap);
  g_inst.init(kSr, false);
  Engine& e = g_inst.engine();
  const macro::MacroSet* set = nullptr;
  for (int i = 0; i < macro::instrumentCount(c.track); ++i) {
    const macro::MacroSet* s = macro::instrumentAt(c.track, i);
    if (std::string(s->name) == c.set) set = s;
  }
  if (!set) { std::fprintf(stderr, "zipper_probe: no set %s\n", c.set); std::exit(2); }
  uint8_t pos[4];
  std::memcpy(pos, c.pos, 4);
  pos[c.macro] = uint8_t(c.from);
  e.post(setInstrumentCommand(c.track, set->id, pos));
  Command n{};
  if (c.track >= kNumDrumTracks) {
    n.type = CmdType::NoteOn;
    n.a = uint8_t(c.track);
    n.b = uint8_t(c.note);
    n.c = 110;
  } else {
    n.type = CmdType::Roll;   // free running while stopped: the first hit now, then every 1/16
    n.a = uint8_t(c.track);
    n.c = 2;
    n.step = kDefaultStep;
    n.step.note = uint8_t(c.note);
    n.step.vel = 110;
  }
  e.post(n);
  const long pre = long(kPreSec * kSr);
  out.sweepFrom = pre;
  out.sweepTo = pre + long((sweepSec + 0.08f) * kSr);
  int16_t buf[2 * kQuantum];
  int lastPosted = c.from, lastKey = -1;
  long nextFrame = pre;           // next UI frame
  bool idealSent = false;
  for (long f = 0; f < total; f += kQuantum) {
    if (f >= nextFrame) {
      nextFrame += long(kFrameSec * kSr);
      const int due = int(float(f - pre) / (kRepeatSec * kSr)) + 1;   // repeats due by this frame
      const int want = std::min(c.to, c.from + kUnits * std::min(due, repeats));
      if (mode == Mode::Ideal) {
        if (!idealSent) {
          e.post(setMacroCommand(c.track, c.macro, uint8_t(c.to), uint16_t(sweepSec * kSr / float(kQuantum))));
          idealSent = true;
        }
      } else if (want != lastPosted) {
        e.post(setMacroCommand(c.track, c.macro, uint8_t(want), mode == Mode::Steps ? kMacroJump : 0));
        lastPosted = want;
      }
    }
    g_inst.process(buf, kQuantum);
    uint8_t eff[kParamsPerTrack];
    engine_detail::effectiveParams(c.track, eff);
    if (lastKey >= 0 && eff[c.keyParam] != lastKey && f >= pre) out.steps.push_back(f);
    lastKey = eff[c.keyParam];
    // The voice's own (smoothed, where it smooths) value of the key parameter, in cents.
    Voice* v = g_inst.voice(c.track);
    double cents = 0.0;
    if (c.track == kTrackBass) cents = 1200.0 * std::log2(double(static_cast<bass::BassVoice*>(v)->debugCutoffHz()));
    if (c.track == kTrackLead) cents = 100.0 * double(static_cast<lead::LeadVoice*>(v)->debugCutoffPitch());
    if (c.track == kTrackKick) cents = 1200.0 * std::log2(double(static_cast<drums::KickVoice*>(v)->debugTuneHz()));
    out.traj.push_back(cents);
  }
  out.hitLen = double(kSr) * 15.0 / double(g_inst.project().globals[kGTempo]);
  engine_detail::setStemTap(nullptr);
  out.stem = l;
  return out;
}

double pct(std::vector<double> v, double p) {
  if (v.empty()) return 0.0;
  std::sort(v.begin(), v.end());
  return v[std::min(v.size() - 1, size_t(p * double(v.size() - 1) + 0.5))];
}

// What the metrics look at (all in dB):
//   kHarm   harmonics 1..12 of the held note (Goertzel, 2048-sample Hann window):
//           the bass, one oscillator, so a harmonic moves only with the filter
//   kOct    octave bands 250 Hz .. 8 kHz (1024-sample window): the lead, whose two
//           detuned saws make single harmonics beat; a band of several averages it
//   kThird  third-octave bands 100 Hz .. 10 kHz of a whole hit (4096 samples): drums
enum Kind { kHarm, kOct, kThird };
using Feat = std::vector<double>;

double goertzelDb(const std::vector<float>& x, long a, int n, double hz) {
  const double w = 2.0 * 3.14159265358979 * hz / double(kSr);
  const double c = 2.0 * std::cos(w);
  double s1 = 0.0, s2 = 0.0;
  for (int i = 0; i < n; ++i) {
    const long j = a + i;
    const double v = (j >= 0 && j < long(x.size())) ? double(x[size_t(j)]) : 0.0;
    const double s0 = v * (0.5 - 0.5 * std::cos(2.0 * 3.14159265358979 * i / (n - 1))) + c * s1 - s2;
    s2 = s1;
    s1 = s0;
  }
  return 10.0 * std::log10(s1 * s1 + s2 * s2 - c * s1 * s2 + 1e-20);
}

// Power spectrum of a Hann window (radix-2 FFT, n a power of two).
std::vector<double> power(const std::vector<float>& x, long a, int n) {
  std::vector<double> re(size_t(n), 0.0), im(size_t(n), 0.0);
  for (int i = 0; i < n; ++i) {
    const long j = a + i;
    const double s = (j >= 0 && j < long(x.size())) ? double(x[size_t(j)]) : 0.0;
    re[size_t(i)] = s * (0.5 - 0.5 * std::cos(2.0 * 3.14159265358979 * i / (n - 1)));
  }
  for (int i = 1, j = 0; i < n; ++i) {
    int bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) { std::swap(re[size_t(i)], re[size_t(j)]); std::swap(im[size_t(i)], im[size_t(j)]); }
  }
  for (int len = 2; len <= n; len <<= 1) {
    const double ang = -2.0 * 3.14159265358979 / len;
    for (int i = 0; i < n; i += len)
      for (int k = 0; k < len / 2; ++k) {
        const double wr = std::cos(ang * k), wi = std::sin(ang * k);
        const size_t u = size_t(i + k), v = size_t(i + k + len / 2);
        const double xr = re[v] * wr - im[v] * wi, xi = re[v] * wi + im[v] * wr;
        re[v] = re[u] - xr; im[v] = im[u] - xi;
        re[u] += xr; im[u] += xi;
      }
  }
  std::vector<double> p(size_t(n / 2));
  for (int k = 0; k < n / 2; ++k) p[size_t(k)] = re[size_t(k)] * re[size_t(k)] + im[size_t(k)] * im[size_t(k)];
  return p;
}

int windowOf(Kind k) { return k == kHarm ? 2048 : (k == kOct ? 1024 : 4096); }

Feat feat(const std::vector<float>& x, long a, Kind kind, double f0) {
  Feat f;
  if (kind == kHarm) {
    for (int h = 1; h <= 12; ++h) f.push_back(goertzelDb(x, a, 2048, f0 * h));
    return f;
  }
  const int n = windowOf(kind);
  const std::vector<double> p = power(x, a, n);
  const double lo0 = kind == kOct ? 250.0 : 100.0, perOct = kind == kOct ? 1.0 : 3.0;
  const int nb = kind == kOct ? 5 : 21;
  for (int b = 0; b < nb; ++b) {
    const double lo = lo0 * std::pow(2.0, double(b) / perOct), hi = lo0 * std::pow(2.0, double(b + 1) / perOct);
    double s = 0.0;
    for (int k = int(lo * n / double(kSr)) + 1; k <= int(hi * n / double(kSr)) && k < n / 2; ++k) s += p[size_t(k)];
    f.push_back(10.0 * std::log10(s + 1e-20));
  }
  return f;
}

// Largest (or mean) |a - b| over the features within `floor` dB of the loudest.
double change(const Feat& a, const Feat& b, bool largest, double floor) {
  double top = -1e9, m = 0.0, s = 0.0;
  int n = 0;
  for (size_t i = 0; i < a.size(); ++i) top = std::max(top, std::max(a[i], b[i]));
  for (size_t i = 0; i < a.size(); ++i)
    if (std::max(a[i], b[i]) > top - floor) {
      const double d = std::fabs(a[i] - b[i]);
      m = std::max(m, d);
      s += d;
      ++n;
    }
  return largest ? m : (n ? s / n : 0.0);
}

// The change across each 7-bit step instant of the steps render: a window ending
// at the step against one starting 1.5 ms after it (the voice's smoothing has
// settled), median over the steps. On a glide the same windows measure how far
// the sound moves in that time.
double stepMetric(const Render& x, const std::vector<long>& at, Kind kind, double f0) {
  const int n = windowOf(kind);
  std::vector<double> v;
  for (long s : at) {
    const long a = s - n, b = s + long(0.0015f * kSr);
    if (a < 0 || b + n > long(x.stem.size())) continue;
    v.push_back(change(feat(x.stem, a, kind, f0), feat(x.stem, b, kind, f0), true, 30.0));
  }
  return pct(v, 0.5);
}

// Per 5.8 ms hop over the sweep, the largest change: 99th percentile.
double fluxMetric(const Render& x, Kind kind, double f0) {
  const int n = windowOf(kind), hop = 256;
  std::vector<double> v;
  Feat prev;
  for (long a = x.sweepFrom; a + n < x.sweepTo && a + n < long(x.stem.size()); a += hop) {
    Feat f = feat(x.stem, a, kind, f0);
    if (!prev.empty()) v.push_back(change(prev, f, true, 30.0));
    prev = f;
  }
  return pct(v, 0.99);
}

// Drums: clicks. The largest sample-to-sample step of the stem during the sweep
// outside the first 3 ms of each roll hit (where the attack belongs), dBFS: a
// parameter that jumps inside a ringing hit (a gain, a filter) shows here.
double clickMetric(const Render& x) {
  double m = 0.0;
  const long guard = long(0.003f * kSr);
  for (long i = std::max(x.sweepFrom, 1L); i < x.sweepTo && i < long(x.stem.size()); ++i) {
    const double ph = std::fmod(double(i), x.hitLen);
    if (ph < double(guard) + double(pg::kQuantum)) continue;
    m = std::max(m, std::fabs(double(x.stem[size_t(i)]) - double(x.stem[size_t(i - 1)])));
  }
  return 20.0 * std::log10(m + 1e-12);
}

// The voice's own trajectory of the key parameter (cents per quantum, during the
// sweep): the largest change in one quantum. A staircase moves in bursts (a
// 7-bit step through the voice's smoothing, or at once where it does not
// smooth), a glide at about the sweep's speed.
double trajMetric(const Render& x) {
  double m = 0.0;
  for (size_t q = size_t(x.sweepFrom / pg::kQuantum) + 1; q < x.traj.size() && long(q) * pg::kQuantum < x.sweepTo; ++q)
    m = std::max(m, std::fabs(x.traj[q] - x.traj[q - 1]));
  return m;
}

// Pass limits for --check.
//   audio   (bass, where one oscillator lets the harmonics show the filter): the
//           glide's step and flux at most the ideal's + 1 dB, and the steps
//           render's step metric at least the glide's + 2 dB.
//   path    (every case): the glide's largest change per quantum at most 1.5 x
//           the ideal's + 0.5 cent, and the steps render's at least 3 x the glide's.
//   clicks  (drums): the glide's largest step between hits at most the ideal's + 1 dB.
constexpr double kOverIdeal = 1.0, kStepsWorse = 2.0;
constexpr double kPathRatio = 1.5, kPathSlack = 0.5, kPathStepsRatio = 3.0;
constexpr double kClickOverIdeal = 1.0;

int engineCases(const std::string& dir, bool check) {
  using namespace pg;
  struct Run {
    Case c;
    Kind kind;
    double f0;
    bool audioChecked;
  };
  const Run runs[] = {
      // bass ACID: OPEN 0.40 -> 0.65 (the resonance peaks at 0.6); SLIDE up = a long amp decay
      {{"bass_acid_open", kTrackBass, "ACID", 0, {0, 0, 0, 255}, 102, 166, 45, bass::kCutoff}, kHarm, 110.0, true},
      // lead HANDS: OPEN 0.30 -> 0.55, PUSH and HOLD up (resonance, sustain)
      {{"lead_hands_open", kTrackLead, "HANDS", 0, {0, 255, 0, 255}, 77, 141, 57, lead::kPCutoff}, kOct, 220.0, false},
      // kick BOOM: SIZE 0.25 -> 0.75 during a 1/16 roll
      {{"kick_boom_size", kTrackKick, "BOOM", 0, {0, 89, 102, 0}, 64, 192, 60, drums::KickVoice::kTune}, kThird, 0.0,
       false},
  };
  int fails = 0;
  std::printf("zipper_probe --engine: a held key (+%d every 40 ms, 33 ms UI frames) through the engine; WAVs in %s/\n",
              kUnits, dir.c_str());
  auto fail = [&](bool bad, const char* what) {
    if (check && bad) {
      std::printf("    FAIL: %s\n", what);
      ++fails;
    }
  };
  for (const Run& run : runs) {
    const Case& c = run.c;
    Render rr[3];
    for (int m = 0; m < 3; ++m) {
      rr[m] = renderCase(c, Mode(m));
      char nm[96];
      std::snprintf(nm, sizeof(nm), "motion_%s_%s.wav", c.name, kModeName[m]);
      save(dir, nm, rr[m].stem);
    }
    const std::vector<long>& at = rr[0].steps;
    std::printf("  %s: %s macro %d, %d -> %d (%zu 7-bit steps of its key parameter on the 7-bit path)\n", c.name, c.set,
                c.macro + 1, c.from, c.to, at.size());
    double pa[3];
    for (int m = 0; m < 3; ++m) pa[m] = trajMetric(rr[m]);
    std::printf("    path  (voice's own value, largest change per quantum, cents): steps %6.2f  glide %6.2f  ideal %6.2f\n",
                pa[0], pa[1], pa[2]);
    fail(pa[1] > kPathRatio * pa[2] + kPathSlack, "the glide's path moves in bursts (more than the ideal's)");
    fail(pa[0] < kPathStepsRatio * pa[1], "the 7-bit path no longer shows its staircase (probe broken?)");
    if (run.kind != kThird) {
      double st[3], fl[3];
      for (int m = 0; m < 3; ++m) {
        st[m] = stepMetric(rr[m], at, run.kind, run.f0);
        fl[m] = fluxMetric(rr[m], run.kind, run.f0);
      }
      std::printf("    step  (audio, median change across the 7-bit steps, dB):   steps %6.2f  glide %6.2f  ideal %6.2f\n",
                  st[0], st[1], st[2]);
      std::printf("    flux  (audio, p99 change per 5.8 ms hop, dB):               steps %6.2f  glide %6.2f  ideal %6.2f\n",
                  fl[0], fl[1], fl[2]);
      if (run.audioChecked) {
        fail(st[1] > st[2] + kOverIdeal, "the glide steps more than the ideal");
        fail(fl[1] > fl[2] + kOverIdeal, "the glide's flux is above the ideal's");
        fail(st[0] < st[1] + kStepsWorse, "the 7-bit path no longer shows its staircase in the audio (probe broken?)");
      } else {
        std::printf("    (audio not checked: the two detuned saws beat, which moves the bands more than one 7-bit step)\n");
      }
    } else {
      double ck[3];
      for (int m = 0; m < 3; ++m) ck[m] = clickMetric(rr[m]);
      std::printf("    click (audio, largest step between hits, dBFS):             steps %6.1f  glide %6.1f  ideal %6.1f\n",
                  ck[0], ck[1], ck[2]);
      fail(ck[1] > ck[2] + kClickOverIdeal, "the glide clicks between hits (more than the ideal)");
    }
  }
  if (check) std::printf("zipper_probe --engine: %s\n", fails ? "FAILED" : "all within the limits");
  return fails ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
  bool engine = false, check = false;
  std::string dir = "build/zipper";
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--engine") engine = true;
    else if (a == "--check") check = true;
    else dir = a;
  }
  ::mkdir("build", 0755);
  ::mkdir(dir.c_str(), 0755);
  if (engine) return engineCases(dir, check);
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
