// The core at 48 kHz (the Daisy's rate; docs/ARCHITECTURE.md "Sample rates"), against
// the same things at 44.1 kHz:
//
//   pitch     every voice across its range: the bass, the lead (every wave, chords),
//             each drum's tuned parts (the kick, the snare's body, the hats' metal,
//             the perc models), both samplers on a ROM sound and on a sine stored
//             with a 22,050 / 44,100 / 48,000 Hz header. Absolute where the voice
//             has a clean fundamental (within 3 cents of the note); everywhere the
//             strongest spectral peaks at 48 kHz against the same peaks at 44.1
//   slices    the sampler's region (slices, start) lands on the same time in the
//             sample whatever its header rate; a click train plays its clicks
//             exactly where they are (in ms) at either engine rate
//   timing    bar lines at several tempos: within one quantum of the exact
//             16 * fs * 15 / BPM, no drift; swing: the off-16ths' delay in ms
//   times     envelopes (decays, releases), the delay's time, the reverb's decay,
//             the macro ramp, in ms within a few percent of 44.1 kHz
//   fx sizes  the delay line and the reverb's room are as long in seconds as at 44.1
//
// Prints a table of what it measured (tools read it for the docs).
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "itests/itest_util.h"
#include "itests/rate_util.h"
#include "pg/bass/bass.h"
#include "pg/drums/drums.h"
#include "pg/engine/delay.h"
#include "pg/engine/reverb.h"
#include "pg/engine/tables.h"
#include "pg/lead/lead.h"
#include "pg/macro/macro.h"
#include "pg/sampler/melodic.h"
#include "pg/sampler/rom_samples.h"
#include "pg/sampler/sample_pool.h"
#include "pg/sampler/sampler.h"
#include "pg/sampler/sampler_params.h"

using namespace pg;
using engine_detail::kNumDelayDivisions;

// Host test code: the measurements are in double; the voices take float.
#pragma GCC diagnostic ignored "-Wdouble-promotion"

namespace {

constexpr float kA = 44100.0f, kB = 48000.0f;
Instrument g_inst;
double g_worstAbs = 0.0, g_worstRel = 0.0;

// ---- pitch -------------------------------------------------------------------------

// The strongest peaks of the 44.1 kHz render in [lo, hi] Hz, each looked for in the
// 48 kHz render within +-2 %: the largest |cents| difference (a peak missing there
// counts as 100 cents).
double comparePeaks(const std::vector<float>& a, const std::vector<float>& b, double t0, double t1, double lo,
                    double hi, int k, double rangeDb = 25.0) {
  const size_t na = size_t((t1 - t0) * kA), nb = size_t((t1 - t0) * kB);
  const rt::Spectrum sa = rt::spectrum(a, kA, size_t(t0 * kA), na, 0.25);
  const rt::Spectrum sb = rt::spectrum(b, kB, size_t(t0 * kB), nb, 0.25);
  const std::vector<double> pa = rt::peaks(sa, lo, hi, k, rangeDb);
  double worst = 0.0;
  for (double f : pa) {
    const double g = rt::peakHz(sb, f * 0.98, f * 1.02);
    const double c = g > 0.0 ? std::fabs(rt::cents(g, f)) : 100.0;
    worst = std::max(worst, c);
  }
  return worst;
}

// The fundamental near `hz` (+-40 cents), in cents from it.
double absCents(const std::vector<float>& x, float fs, double t0, double t1, double hz) {
  const rt::Spectrum s = rt::spectrum(x, fs, size_t(t0 * fs), size_t((t1 - t0) * fs), 0.1);
  const double f = rt::peakHz(s, hz * 0.977, hz * 1.023);
  return f > 0.0 ? rt::cents(f, hz) : 1000.0;
}

void checkPitch(const char* what, double absA, double absB, double rel, double absTol, double relTol) {
  const bool hasAbs = absA < 999.0 || absB < 999.0;
  if (hasAbs) {
    g_worstAbs = std::max(g_worstAbs, std::fabs(absB));
    it::check(std::fabs(absA) <= absTol && std::fabs(absB) <= absTol, "%s: %.2f cents at 44.1, %.2f at 48", what,
              absA, absB);
  }
  g_worstRel = std::max(g_worstRel, rel);
  it::check(rel <= relTol, "%s: peaks at 48 kHz differ from 44.1 by up to %.2f cents", what, rel);
}

void testBass() {
  bass::BassVoice v;
  uint8_t p[kSoundParams];
  rt::defaults(v, p);
  double worstA = 0, worstR = 0;
  for (int n = 24; n <= 72; n += 7) {
    const NoteEvent e{uint8_t(n), 100, false, false, 0};
    const auto a = rt::renderNote(v, kA, p, e, 0.9, 1.0);
    const auto b = rt::renderNote(v, kB, p, e, 0.9, 1.0);
    const double f = rt::midiHz(n);
    const double ca = absCents(a, kA, 0.2, 0.8, f), cb = absCents(b, kB, 0.2, 0.8, f);
    const double r = comparePeaks(a, b, 0.2, 0.8, 20.0, 8000.0, 8);
    char w[64];
    std::snprintf(w, sizeof w, "bass note %d", n);
    checkPitch(w, ca, cb, r, 3.0, 2.0);
    worstA = std::max(worstA, std::fabs(cb));
    worstR = std::max(worstR, r);
  }
  std::printf("  pitch  bass   notes 24..72    abs %.2f c   48 vs 44.1 %.2f c\n", worstA, worstR);
}

void testLead() {
  lead::LeadVoice v;
  uint8_t p[kSoundParams];
  double worstA = 0, worstR = 0, worstChord = 0;
  for (int wave = 0; wave < lead::kNumWaves; ++wave) {
    for (int n = 36; n <= 96; n += 12) {
      rt::defaults(v, p);
      p[lead::kPWave] = uint8_t(wave);
      p[lead::kPCutoff] = 127; p[lead::kPReso] = 0; p[lead::kPEnvAmt] = 0; p[lead::kPKeyTrack] = 0;
      p[lead::kPSustain] = 127; p[lead::kPVibrato] = 0; p[lead::kPLfoCutoff] = 0;
      const NoteEvent e{uint8_t(n), 100, false, false, 0};
      const auto a = rt::renderNote(v, kA, p, e, 0.9, 1.0);
      const auto b = rt::renderNote(v, kB, p, e, 0.9, 1.0);
      const double r = comparePeaks(a, b, 0.2, 0.8, 30.0, 10000.0, 8);
      double ca = 1000.0, cb = 1000.0;
      // a clean fundamental: the FM sine (shape 0) has nothing but its carrier
      if (wave == lead::kWaveFm1) {
        uint8_t q[kSoundParams];
        std::memcpy(q, p, sizeof q);
        q[lead::kPShape] = 0; q[lead::kPOscMix] = 0; q[lead::kPDetune] = 0; q[lead::kPWidth] = 0;
        const auto sa = rt::renderNote(v, kA, q, e, 0.9, 1.0), sb = rt::renderNote(v, kB, q, e, 0.9, 1.0);
        ca = absCents(sa, kA, 0.2, 0.8, rt::midiHz(n));
        cb = absCents(sb, kB, 0.2, 0.8, rt::midiHz(n));
        worstA = std::max(worstA, std::fabs(cb));
      }
      char w[64];
      std::snprintf(w, sizeof w, "lead wave %d note %d", wave, n);
      checkPitch(w, ca, cb, r, 3.0, 2.0);
      worstR = std::max(worstR, r);
    }
  }
  // chords: every type on C4, the default (saw) patch with the filter open
  for (int c = 1; c < kChordNum; ++c) {
    rt::defaults(v, p);
    p[lead::kPCutoff] = 127; p[lead::kPEnvAmt] = 0; p[lead::kPSustain] = 127; p[lead::kPVibrato] = 0;
    const NoteEvent e{60, 100, false, false, uint8_t(c)};
    const auto a = rt::renderNote(v, kA, p, e, 0.9, 1.0);
    const auto b = rt::renderNote(v, kB, p, e, 0.9, 1.0);
    const double r = comparePeaks(a, b, 0.2, 0.8, 100.0, 4000.0, 10);
    char w[64];
    std::snprintf(w, sizeof w, "lead chord %d", c);
    checkPitch(w, 1000, 1000, r, 3.0, 2.0);
    worstChord = std::max(worstChord, r);
  }
  std::printf("  pitch  lead   8 waves x 36..96 abs (sine) %.2f c   48 vs 44.1 %.2f c; chords %.2f c\n", worstA,
              worstR, worstChord);
}

// A drum voice at notes 48 / 60 / 72: the strongest peaks of the attack (5..50 ms)
// and the body (50..250 ms), and the spectral centroid of the whole hit.
// noisy: the drum's sound is mostly noise or a fast sweep (no stable peak to follow), or
// a bank of plain squares whose aliases fold to other places at another rate (the
// hats): only the centroid is compared.
template <class V>
void testDrum(const char* name, V& v, int modelParam = -1, int models = 1, const char* const* modelNames = nullptr,
              uint32_t noisyModels = 0) {
  uint8_t p[kSoundParams];
  for (int m = 0; m < models; ++m) {
    double worstR = 0, worstCen = 0;
    for (int n = 48; n <= 72; n += 12) {
      rt::defaults(v, p);
      if (modelParam >= 0) p[modelParam] = uint8_t(m);
      const NoteEvent e{uint8_t(n), 110, false, false, 0};
      const auto a = rt::renderNote(v, kA, p, e, 0.05, 0.6);
      const auto b = rt::renderNote(v, kB, p, e, 0.05, 0.6);
      const double r1 = comparePeaks(a, b, 0.005, 0.05, 30.0, 12000.0, 4, 12.0);
      const double r2 = comparePeaks(a, b, 0.05, 0.25, 30.0, 12000.0, 4, 12.0);
      const rt::Spectrum sa = rt::spectrum(a, kA, 0, size_t(0.4 * kA), 2.0);
      const rt::Spectrum sb = rt::spectrum(b, kB, 0, size_t(0.4 * kB), 2.0);
      const double ca = rt::centroid(sa, 20, 15000), cb = rt::centroid(sb, 20, 15000);
      const double dc = std::fabs(cb / ca - 1.0) * 100.0;
      worstR = std::max(worstR, std::max(r1, r2));
      worstCen = std::max(worstCen, dc);
    }
    char w[64];
    std::snprintf(w, sizeof w, "%s%s%s", name, modelNames ? " " : "", modelNames ? modelNames[m] : "");
    const bool noisy = (noisyModels >> m) & 1u;
    std::printf("  pitch  %-12s peaks 48 vs 44.1 %6.2f c%s  centroid %.2f %%\n", w, worstR, noisy ? " (noisy)" : "         ", worstCen);
    if (!noisy) {
      g_worstRel = std::max(g_worstRel, worstR);
      it::check(worstR <= 6.0, "%s: drum peaks differ by %.2f cents", w, worstR);
    }
    it::check(worstCen <= 5.0, "%s: drum centroid differs by %.2f %%", w, worstCen);
  }
}

void testDrums() {
  drums::KickVoice kick;
  drums::SnareVoice snare;
  drums::HatVoice hat;
  drums::PercVoice perc;
  testDrum("kick", kick);
  testDrum("snare", snare);
  testDrum("hat", hat, -1, 1, nullptr, 1u);
  drums::ClapVoice clap;
  testDrum("clap", clap, -1, 1, nullptr, 1u);
  static const char* const kModels[] = {"TOM", "RIM", "COWB", "CLAV", "SHKR", "ZAP"};
  testDrum("perc", perc, drums::PercVoice::kModel, drums::PercVoice::kNumModels, kModels,
           (1u << drums::PercVoice::kShaker) | (1u << drums::PercVoice::kZap));
}

// ---- samplers ----------------------------------------------------------------------

std::vector<int16_t> g_sine[3];
sampler::MemoryPool g_pool;
const uint32_t kRates[3] = {22050, 44100, 48000};

void makeSines() {
  for (int k = 0; k < 3; ++k) {
    const uint32_t n = std::min<uint32_t>(kRates[k] * 2, sampler::kSlotMaxSamples);   // 2 s (or the slot)
    g_sine[k].resize(n);
    for (uint32_t i = 0; i < n; ++i)
      g_sine[k][i] = int16_t(std::lround(20000.0 * std::sin(2.0 * 3.14159265358979 * 440.0 * double(i) / kRates[k])));
    it::check(g_pool.set(1 + k, g_sine[k].data(), n, "SINE", kRates[k]), "pool takes a %u Hz sample", kRates[k]);
  }
}

template <class V>
std::vector<float> samplerNote(V& v, float fs, const uint8_t* p, int slot, int note, double sec) {
  v.setPool(&g_pool);
  v.init(fs);
  v.setTempo(120.0f);
  const long quanta = long(std::ceil(sec * double(fs) / kQuantum));
  std::vector<float> out;
  float l[kQuantum], r[kQuantum];
  for (long q = 0; q < quanta; ++q) {
    v.setParams(p);
    if (q == 0) {
      v.setSampleSlot(uint8_t(slot));
      v.noteOn(NoteEvent{uint8_t(note), 100, false, false, 0});
    }
    if (v.active()) {
      v.render(l, r, kQuantum);
      for (int i = 0; i < kQuantum; ++i) out.push_back(0.5f * (l[i] + r[i]));
    } else {
      for (int i = 0; i < kQuantum; ++i) out.push_back(0.0f);
    }
  }
  return out;
}

template <class V>
void testSampler(const char* name, V& v, int naturalNote) {
  uint8_t p[kSoundParams];
  rt::defaults(v, p);
  double worstA = 0, worstR = 0;
  for (int k = 0; k < 3; ++k)
    for (int dn = -12; dn <= 12; dn += 12) {
      const int note = naturalNote + dn;
      const double hz = 440.0 * std::pow(2.0, dn / 12.0);
      const auto a = samplerNote(v, kA, p, 1 + k, note, 0.6);
      const auto b = samplerNote(v, kB, p, 1 + k, note, 0.6);
      const double ca = absCents(a, kA, 0.05, 0.45, hz), cb = absCents(b, kB, 0.05, 0.45, hz);
      char w[80];
      std::snprintf(w, sizeof w, "%s: a %u Hz sample, note %d", name, kRates[k], note);
      checkPitch(w, ca, cb, 0.0, 3.0, 2.0);
      worstA = std::max(worstA, std::max(std::fabs(ca), std::fabs(cb)));
    }
  // the ROM sounds (empty slots 0, 4, 5 play ROM 0, 1, 2), 22,050 Hz: at 48 kHz as at 44.1
  for (int slot : {0, 4, 5}) {
    const auto a = samplerNote(v, kA, p, slot, naturalNote, 0.8);
    const auto b = samplerNote(v, kB, p, slot, naturalNote, 0.8);
    const double r = std::max(comparePeaks(a, b, 0.05, 0.30, 60, 5000, 4, 15.0), comparePeaks(a, b, 0.30, 0.55, 60, 5000, 4, 15.0));
    char w[80];
    std::snprintf(w, sizeof w, "%s: ROM sound of slot %d", name, slot);
    checkPitch(w, 1000, 1000, r, 3.0, 3.0);
    worstR = std::max(worstR, r);
  }
  std::printf("  pitch  %-12s 22.05/44.1/48 kHz sines abs %.2f c; ROM sounds 48 vs 44.1 %.2f c\n", name, worstA,
              worstR);
}

// The region of the same sample, stored at three rates, starts and ends at the same
// time; and a click every 100 ms plays its clicks 100 ms apart at either engine rate.
void testSlices() {
  uint8_t p[kSoundParams];
  sampler::SamplerVoice sv;
  rt::defaults(sv, p);
  p[sampler::kSlices] = 4;
  p[sampler::kSlice] = 2;
  p[sampler::kStart] = 32;
  p[sampler::kLength] = 100;
  double worst = 0.0;
  for (int k = 0; k < 3; ++k) {
    int32_t a = 0, b = 0;
    const uint32_t len = uint32_t(2.0 * kRates[k]);
    sampler::SamplerVoice::region(p, len, kRates[k], &a, &b);
    const double ta = double(a) / kRates[k], tb = double(b) / kRates[k];
    int32_t a0 = 0, b0 = 0;
    sampler::SamplerVoice::region(p, 2 * 22050, 22050, &a0, &b0);
    worst = std::max(worst, std::max(std::fabs(ta - a0 / 22050.0), std::fabs(tb - b0 / 22050.0)) * 1000.0);
  }
  it::check(worst < 0.1, "sampler region moves by %.3f ms between header rates", worst);
  // the click train: 1 s at each header rate, a click (one full-scale sample) every 100 ms
  static std::vector<int16_t> clicks[3];
  for (int k = 0; k < 3; ++k) {
    clicks[k].assign(kRates[k], 0);
    for (uint32_t i = 0; i < kRates[k]; i += kRates[k] / 10) clicks[k][i] = 30000;
    g_pool.set(7 + k, clicks[k].data(), kRates[k], "CLICK", kRates[k]);
  }
  rt::defaults(sv, p);
  p[sampler::kAttack] = 0;
  double worstClick = 0.0;
  for (int k = 0; k < 3; ++k)
    for (float fs : {kA, kB}) {
      const auto x = samplerNote(sv, fs, p, 7 + k, 60, 0.7);
      // each click's arrival: the first sample after the previous click's ring above half its peak
      std::vector<double> at;
      size_t i = 0;
      float pk = 0.0f;
      for (float s : x) pk = std::max(pk, std::fabs(s));
      while (i < x.size() && at.size() < 6) {
        if (std::fabs(x[i]) >= 0.5f * pk) {
          at.push_back(double(i) * 1000.0 / fs);
          i += size_t(0.05 * fs);
        } else {
          ++i;
        }
      }
      it::check(at.size() >= 5, "click train: %zu clicks", at.size());
      for (size_t c = 1; c < at.size(); ++c) worstClick = std::max(worstClick, std::fabs(at[c] - at[c - 1] - 100.0));
    }
  // the linear interpolation smears a click over two samples: half a sample at 22,050 Hz is 0.023 ms
  it::check(worstClick < 0.1, "click spacing off by %.3f ms", worstClick);
  std::printf("  slices region at 22.05/44.1/48 kHz headers: %.4f ms apart; clicks every 100 ms: worst %.4f ms\n", worst,
              worstClick);
}

// ---- timing ------------------------------------------------------------------------

struct BarTap final : OutputTap {
  long frame = 0;
  std::vector<long> bars;
  void quantum(const int16_t*, int frames, uint8_t flags) override {
    if (flags & kTapBar) bars.push_back(frame);
    frame += frames;
  }
};

void quanta(long n, it::Stats* st = nullptr) {
  int16_t buf[2 * kQuantum];
  for (long i = 0; i < n; ++i) {
    g_inst.process(buf, kQuantum);
    if (st) st->add(buf, kQuantum);
  }
}

void testBars() {
  std::printf("  timing bar length (frames, 48 kHz): BPM exact measured(mean) worst-bar-line-error\n");
  for (int bpm : {60, 97, 120, 124, 133, 174, 200})
    for (int swing : {0, 25, 50}) {
      g_inst.init(kB, false);
      Project& p = g_inst.project();
      p.globals[kGTempo] = uint8_t(bpm);
      p.globals[kGSwing] = uint8_t(swing);
      g_inst.engine().sanitizeProject(p);
      static BarTap tap;
      tap = BarTap{};
      g_inst.engine().setOutputTap(&tap);
      it::post(g_inst, CmdType::Play);
      const double bar = 16.0 * double(kB) * 15.0 / double(bpm);
      quanta(long(9.2 * bar / kQuantum));
      g_inst.engine().setOutputTap(nullptr);
      double worst = 0.0;
      for (size_t k = 0; k < tap.bars.size(); ++k)
        worst = std::max(worst, std::fabs(double(tap.bars[k] - tap.bars[0]) - double(k) * bar));
      const double mean = tap.bars.size() > 1 ? double(tap.bars.back() - tap.bars[0]) / double(tap.bars.size() - 1) : 0;
      it::check(tap.bars.size() >= 9, "BPM %d: %zu bar lines", bpm, tap.bars.size());
      it::check(worst < kQuantum, "BPM %d swing %d: a bar line %.1f frames from the exact grid", bpm, swing, worst);
      if (swing == 0) std::printf("         %3d %10.2f %10.2f %5.1f\n", bpm, bar, mean, worst);
    }
}

// The fire times of a track that plays every 16th, in ms after the first.
std::vector<double> fireTimes(float fs, int bpm, int swing) {
  g_inst.init(fs, false);
  Project& p = g_inst.project();
  p.globals[kGTempo] = uint8_t(bpm);
  p.globals[kGSwing] = uint8_t(swing);
  for (int i = 0; i < 16; ++i) {
    Step& s = p.patterns[0].tracks[kTrackHatClosed].steps[i];
    s = kDefaultStep;
    s.setFlags(kStepOn);
  }
  g_inst.engine().sanitizeProject(p);
  it::post(g_inst, CmdType::Play);
  std::vector<double> at;
  uint8_t last = g_inst.engine().status().trig[kTrackHatClosed];
  const long n = long(16.0 * fs * 15.0 / float(bpm) / kQuantum) + 4;
  for (long q = 0; q < n; ++q) {
    quanta(1);
    const uint8_t t = g_inst.engine().status().trig[kTrackHatClosed];
    if (t != last) at.push_back(double(q) * kQuantum * 1000.0 / fs);   // fired in quantum q
    last = t;
  }
  for (size_t i = 1; i < at.size(); ++i) at[i] -= at[0];
  if (!at.empty()) at[0] = 0.0;
  return at;
}

void testSwing() {
  double worst = 0.0;
  for (int bpm : {90, 124, 160})
    for (int swing : {0, 20, 50}) {
      const auto a = fireTimes(kA, bpm, swing), b = fireTimes(kB, bpm, swing);
      it::check(a.size() >= 16 && b.size() >= 16, "swing: %zu / %zu steps fired", a.size(), b.size());
      for (size_t i = 0; i < std::min<size_t>(16, std::min(a.size(), b.size())); ++i) {
        const double exact = double(i) * 15000.0 / bpm + ((i & 1) ? double(swing) / 50.0 * 0.5 * 15000.0 / bpm : 0.0);
        worst = std::max(worst, std::fabs(b[i] - exact));
        // one quantum at either rate: 0.36 / 0.33 ms
        it::check(std::fabs(b[i] - a[i]) < 0.7, "BPM %d swing %d step %zu: %.3f ms at 48, %.3f at 44.1", bpm, swing,
                  i, b[i], a[i]);
      }
    }
  it::check(worst < 0.34, "swing: a step %.3f ms from the swung grid at 48 kHz", worst);
  std::printf("  timing swing 0/20/50 at 90/124/160 BPM: steps within %.3f ms of the swung grid (one quantum = 0.333 ms)\n",
              worst);
}

// ---- times in ms ------------------------------------------------------------------

void timeCheck(const char* what, double a, double b, double tolPct) {
  const double d = a > 0 ? (b / a - 1.0) * 100.0 : 1e9;
  std::printf("  times  %-40s %8.1f ms at 44.1  %8.1f ms at 48  %+6.2f %%\n", what, a, b, d);
  it::check(a > 0 && b > 0 && std::fabs(d) <= tolPct, "%s: %.2f ms at 44.1, %.2f at 48 (%+.2f %%)", what, a, b, d);
}

void testEnvelopes() {
  {   // the lead: decay to sustain, release (a sine, so the envelope is all there is)
    lead::LeadVoice v;
    uint8_t p[kSoundParams];
    rt::defaults(v, p);
    p[lead::kPWave] = lead::kWaveFm1; p[lead::kPShape] = 0; p[lead::kPCutoff] = 127; p[lead::kPEnvAmt] = 0;
    p[lead::kPVibrato] = 0; p[lead::kPSustain] = 0; p[lead::kPDecay] = 60; p[lead::kPAttack] = 0;
    const NoteEvent e{60, 100, false, false, 0};
    const auto a = rt::renderNote(v, kA, p, e, 3.0, 3.0), b = rt::renderNote(v, kB, p, e, 3.0, 3.0);
    timeCheck("lead decay (to -40 dB)", rt::decayMs(a, kA, 40), rt::decayMs(b, kB, 40), 3.0);
    p[lead::kPSustain] = 127; p[lead::kPRelease] = 70;
    const auto c = rt::renderNote(v, kA, p, e, 0.3, 3.0), d = rt::renderNote(v, kB, p, e, 0.3, 3.0);
    timeCheck("lead release (to -40 dB)", rt::decayMs(c, kA, 40, size_t(0.3 * kA), true),
              rt::decayMs(d, kB, 40, size_t(0.3 * kB), true), 3.0);
  }
  {
    bass::BassVoice v;
    uint8_t p[kSoundParams];
    rt::defaults(v, p);
    const NoteEvent e{36, 100, false, false, 0};
    const auto a = rt::renderNote(v, kA, p, e, 0.1, 2.0), b = rt::renderNote(v, kB, p, e, 0.1, 2.0);
    timeCheck("bass note, default patch (to -30 dB)", rt::decayMs(a, kA, 30), rt::decayMs(b, kB, 30), 3.0);
  }
  {
    drums::KickVoice v;
    uint8_t p[kSoundParams];
    rt::defaults(v, p);
    const NoteEvent e{60, 110, false, false, 0};
    const auto a = rt::renderNote(v, kA, p, e, 0.05, 2.0), b = rt::renderNote(v, kB, p, e, 0.05, 2.0);
    timeCheck("kick decay (to -40 dB)", rt::decayMs(a, kA, 40), rt::decayMs(b, kB, 40), 3.0);
  }
  {
    drums::HatVoice v;
    uint8_t p[kSoundParams];
    rt::defaults(v, p);
    const NoteEvent e{60, 110, false, false, 0};
    const auto a = rt::renderNote(v, kA, p, e, 0.05, 1.0), b = rt::renderNote(v, kB, p, e, 0.05, 1.0);
    // noise and a square bank (its aliases land elsewhere at 48 kHz): the slope, 10 ms windows
    timeCheck("hat decay (-10 to -40 dB)", rt::fallMs(a, kA, 10, 40, 10.0), rt::fallMs(b, kB, 10, 40, 10.0), 5.0);
  }
  {
    sampler::SamplerVoice v;
    uint8_t p[kSoundParams];
    rt::defaults(v, p);
    p[sampler::kRelease] = 80;
    p[sampler::kLoop] = 1;
    // a held note, released after 0.3 s (renderNote's gate)
    v.setPool(&g_pool);
    const auto a = [&](float fs) {
      v.init(fs);
      std::vector<float> out;
      float l[kQuantum], r[kQuantum];
      const long n = long(2.5 * fs / kQuantum), off = long(0.3 * fs / kQuantum);
      for (long q = 0; q < n; ++q) {
        v.setParams(p);
        if (q == 0) { v.setSampleSlot(3); v.noteOn(NoteEvent{60, 100, false, false, 0}); }
        if (q == off) v.noteOff(60);
        if (v.active()) { v.render(l, r, kQuantum); for (int i = 0; i < kQuantum; ++i) out.push_back(l[i]); }
        else for (int i = 0; i < kQuantum; ++i) out.push_back(0.0f);
      }
      return out;
    };
    const auto x = a(kA), y = a(kB);
    timeCheck("sampler release (to -40 dB)", rt::decayMs(x, kA, 40, size_t(0.3 * kA), true),
              rt::decayMs(y, kB, 40, size_t(0.3 * kB), true), 3.0);
  }
}

// The engine's delay: its time in ms for every division at a few tempos (exact), and
// that the line holds what it holds at 44.1 kHz (nothing halved at 48 that is not).
void testDelayTimes() {
  double worst = 0.0;
  for (int bpm : {60, 80, 86, 100, 124, 200})
    for (int div = 0; div < kNumDelayDivisions; ++div) {
      double ms[2];
      for (int r = 0; r < 2; ++r) {
        const float fs = r ? kB : kA;
        g_inst.init(fs, false);
        Project& p = g_inst.project();
        p.globals[kGTempo] = uint8_t(bpm);
        p.globals[kGDelayTime] = uint8_t(div);
        g_inst.engine().sanitizeProject(p);
        quanta(2);
        ms[r] = double(engine_detail::delayTimeSamples()) * 1000.0 / fs;
      }
      const double d = std::fabs(ms[1] - ms[0]);
      worst = std::max(worst, d);
      it::check(d < 0.05, "delay BPM %d division %d: %.3f ms at 44.1, %.3f at 48", bpm, div, ms[0], ms[1]);
    }
  std::printf("  times  delay time, every division at 60..200 BPM: 48 vs 44.1 within %.4f ms\n", worst);
  // and the line: 750 ms fit at both rates (they would be halved past the line)
  static engine_detail::Delay d;
  for (float fs : {kA, kB}) {
    d.init(fs * 0.5f);
    const float want = 0.7495f * fs * 0.5f;
    const int got = d.setTime(want);
    it::check(std::fabs(float(got) - want) < 1.0f, "delay line at %.0f Hz: 749.5 ms became %.1f ms", double(fs),
              got * 2000.0 / fs);
  }
  std::printf("  fx     delay line: 749.5 ms fits at 44.1 and at 48 kHz (line %d / %d half-rate samples)\n",
              engine_detail::delay_len::lineFor(kA * 0.5f), engine_detail::delay_len::lineFor(kB * 0.5f));
}

// The reverb's lines and its decay: a click into the reverb alone (its return only).
void testReverb() {
  engine_detail::Reverb rv;
  double lenMs[2] = {0, 0};
  std::vector<float> tail[2];
  for (int r = 0; r < 2; ++r) {
    const float half = (r ? kB : kA) * 0.5f;
    rv.init(half);
    rv.setParams(1.5f, 0.6f);
    lenMs[r] = rv.totalDelaySamples() * 1000.0 / half;
    const int n = int(3.0f * half);
    float in[8], L[8], R[8];
    for (int i = 0; i < n; i += 8) {
      for (int k = 0; k < 8; ++k) { in[k] = (i + k == 0) ? 0.8f : 0.0f; L[k] = R[k] = 0.0f; }
      rv.process(in, L, R, 8);
      for (int k = 0; k < 8; ++k) tail[r].push_back(0.5f * (L[k] + R[k]));
    }
  }
  std::printf("  fx     reverb lines: %.2f ms in all at 44.1, %.2f ms at 48\n", lenMs[0], lenMs[1]);
  it::check(std::fabs(lenMs[1] / lenMs[0] - 1.0) < 0.01, "reverb room %.2f ms at 48 vs %.2f", lenMs[1], lenMs[0]);
  timeCheck("reverb decay, RT 1.5 s (to -30 dB)", rt::decayMs(tail[0], kA * 0.5, 30), rt::decayMs(tail[1], kB * 0.5, 30),
            5.0);
}

// A SetMacro's default ramp (kMacroRampMs) takes as long at 48 kHz as at 44.1.
double rampMs(float fs) {
  g_inst.init(fs, false);
  const macro::MacroSet* s = macro::instrumentAt(kTrackBass, 0);
  uint8_t pos[4] = {0, 0, 0, 0};
  g_inst.engine().post(setInstrumentCommand(kTrackBass, s->id, pos));
  quanta(8);
  g_inst.engine().post(setMacroCommand(kTrackBass, 0, 255));
  long q = 0;
  for (; q < 2000; ++q) {
    quanta(1);
    if (engine_detail::macroPosition(kTrackBass, 0) >= 254.999f) break;
  }
  return double(q) * kQuantum * 1000.0 / fs;
}

void testMacroRamp() {
  const double a = rampMs(kA), b = rampMs(kB);
  timeCheck("macro ramp (kMacroRampMs = 72)", a, b, 1.5);
}

// The scope tap reports the engine's rate (the picture's time scale follows it).
void testScope() {
  g_inst.init(kB, false);
  g_inst.engine().setScope(kTrackKick, 4);
  it::post(g_inst, CmdType::Play);
  quanta(400);
  float x[kScopeReadMax];
  ScopeInfo in{};
  g_inst.engine().readScope(x, kScopeReadMax, &in);
  it::check(in.sampleRate == kB && in.decimation == 4, "scope: rate %.0f, decimation %d", double(in.sampleRate),
            in.decimation);
}

}  // namespace

int main() {
  std::printf("itest_rate48: 48 kHz against 44.1 kHz\n");
  testBass();
  testLead();
  testDrums();
  makeSines();
  {
    sampler::SamplerVoice sv;
    testSampler("sampler", sv, 60);
    sampler::MelodicSampler ms;
    testSampler("melodic", ms, 48);
  }
  testSlices();
  testBars();
  testSwing();
  testEnvelopes();
  testDelayTimes();
  testReverb();
  testMacroRamp();
  testScope();
  std::printf("  worst: absolute pitch %.2f cents, 48 vs 44.1 %.2f cents\n", g_worstAbs, g_worstRel);
  return it::finish("itest_rate48");
}
