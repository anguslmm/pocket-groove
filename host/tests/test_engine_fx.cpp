// Mixer, send FX and master bus: level taper / pan law / mute ramps, delay
// time accuracy and stability, reverb decay / density / colour, DJ filter
// bypass transparency and sweep stability, limiter, mono-sum health
// (the device's codec plays (L+R)/2), renders.
//
// "Wet" signals are isolated by rendering the same deterministic scene twice,
// with and without the send, and subtracting.
#include <algorithm>
#include <complex>

#include "tests/engine_test_util.h"

using namespace et;
using namespace pg;

static Rig rig;

struct Stereo {
  std::vector<double> l, r;
  float sr;
  size_t size() const { return l.size(); }
  double mono(size_t i) const { return 0.5 * (l[i] + r[i]); }
};

static Stereo toStereo(const std::vector<int16_t>& o, float sr) {
  Stereo s;
  s.sr = sr;
  s.l.resize(o.size() / 2);
  s.r.resize(o.size() / 2);
  for (size_t i = 0; i < s.l.size(); ++i) {
    s.l[i] = double(o[2 * i]) / 32767.0;
    s.r[i] = double(o[2 * i + 1]) / 32767.0;
  }
  return s;
}

// Render `seconds` of a scene. `setup` edits the project/voices after the
// neutral mix and before init; the transport is started at t = 0.
template <class F>
static Stereo render(F setup, double seconds, float sr = 44100.0f, int bpm = 124) {
  rig.init(sr, bpm);
  rig.neutralMix();
  setup(rig);
  rig.eng.init(sr, &rig.proj);
  rig.runSeconds(0.1);              // let the mixer gains / sends ramp in from silence
  rig.out.clear();
  rig.capture = true;
  rig.post(CmdType::Play);
  rig.runSeconds(seconds);
  rig.capture = false;
  return toStereo(rig.out, sr);
}
static Stereo diff(const Stereo& a, const Stereo& b) {
  Stereo d;
  d.sr = a.sr;
  const size_t n = std::min(a.size(), b.size());
  d.l.resize(n);
  d.r.resize(n);
  for (size_t i = 0; i < n; ++i) { d.l[i] = a.l[i] - b.l[i]; d.r[i] = a.r[i] - b.r[i]; }
  return d;
}
static double energy(const std::vector<double>& x, size_t a, size_t b) {
  double e = 0;
  for (size_t i = a; i < b && i < x.size(); ++i) e += x[i] * x[i];
  return e;
}
static double monoEnergy(const Stereo& s, size_t a, size_t b) {
  double e = 0;
  for (size_t i = a; i < b && i < s.size(); ++i) { const double m = s.mono(i); e += m * m; }
  return e;
}

// ------------------------------------------------------------------ mixer --

static void testMixer() {
  // Level taper: (v/100)^3, 100 = unity. Pan: constant power, unity at centre.
  for (int level : {0, 50, 80, 100, 127}) {
    const Stereo s = render([&](Rig& r) {
      r.v[kTrackSnare].mode = StubVoice::Dc;
      r.v[kTrackSnare].amp = 0.2f;
      r.proj.tracks[kTrackSnare].params[kMixLevel] = uint8_t(level);
    }, 0.1);
    const double expect = 0.2 * std::pow(level / 100.0, 3.0);
    CHECKF(std::fabs(s.l.back() - expect) < 0.0005, "level %d: gain %.4f expected %.4f", level, s.l.back() / 0.2, expect / 0.2);
  }
  double worstPow = 0;
  for (int pan = 0; pan <= 127; pan += 9) {
    const Stereo s = render([&](Rig& r) {
      r.v[kTrackSnare].mode = StubVoice::Dc;
      r.v[kTrackSnare].amp = 0.2f;
      r.proj.tracks[kTrackSnare].params[kMixPan] = uint8_t(pan);
    }, 0.1);
    const double gl = s.l.back() / 0.2, gr = s.r.back() / 0.2;
    worstPow = std::fmax(worstPow, std::fabs(gl * gl + gr * gr - 2.0));
    if (pan == 0) CHECK(std::fabs(gr) < 0.001 && std::fabs(gl - 1.41421) < 0.002);
  }
  CHECKF(worstPow < 0.004, "pan law is not constant power (worst L^2+R^2 error %.4f)", worstPow);

  // Mute / unmute: a ~4 ms ramp, never a step.
  rig.init();
  rig.neutralMix();
  rig.v[kTrackSnare].mode = StubVoice::Dc;
  rig.v[kTrackSnare].amp = 0.5f;
  rig.eng.init(44100.0f, &rig.proj);
  rig.capture = true;
  rig.runSeconds(0.1);
  rig.post(CmdType::SetMute, kTrackSnare, 0, 1);
  rig.runSeconds(0.1);
  rig.post(CmdType::SetMute, kTrackSnare, 0, 0);
  rig.runSeconds(0.1);
  rig.post(CmdType::SetTrackParam, kTrackSnare, kMixLevel, 0);
  rig.runSeconds(0.1);
  rig.post(CmdType::SetTrackParam, kTrackSnare, kMixLevel, 127);
  rig.runSeconds(0.1);
  rig.capture = false;
  const Stereo s = toStereo(rig.out, 44100.0f);
  double maxStep = 0;
  for (size_t i = 1; i < s.size(); ++i) maxStep = std::fmax(maxStep, std::fabs(s.l[i] - s.l[i - 1]));
  CHECKF(maxStep < 0.01, "mute/level change step %.4f per sample", maxStep);
  CHECK(std::fabs(s.l[size_t(0.19 * 44100)]) < 1e-4);                // muted
  CHECK(std::fabs(s.l[size_t(0.29 * 44100)] - 0.5) < 1e-3);          // back
  // level 127 = +6.2 dB on 0.5 -> 1.02 at the limiter input -> soft clipped, never above full scale
  CHECK(s.l.back() > 0.9 && s.l.back() <= 1.0);
  std::printf("  mixer: taper/pan law ok, mute + level ramps max step %.4f/sample\n", maxStep);
}

// ------------------------------------------------------------------ delay --

static const double kDivSteps[9] = {0.5, 2.0 / 3.0, 1.0, 4.0 / 3.0, 1.5, 2.0, 8.0 / 3.0, 3.0, 4.0};

static size_t peakIndex(const std::vector<double>& x, size_t a, size_t b) {
  size_t best = a;
  for (size_t i = a; i < b && i < x.size(); ++i) if (std::fabs(x[i]) > std::fabs(x[best])) best = i;
  return best;
}

template <class F>
static Stereo delayWet(F extra, double seconds, float sr = 44100.0f, int bpm = 124) {
  auto base = [&](Rig& r, bool send) {
    r.v[kTrackPerc].mode = StubVoice::Impulse;
    r.v[kTrackPerc].amp = 0.6f;
    r.step(0, kTrackPerc, 0) = mkStep(60);
    r.proj.patterns[0].length = 64;
    r.proj.patterns[0].tracks[kTrackPerc].length = 64;
    r.proj.tracks[kTrackPerc].params[kMixDelaySend] = send ? 127 : 0;
    r.proj.globals[kGDelayTone] = 96;
    extra(r);
  };
  const Stereo a = render([&](Rig& r) { base(r, true); }, seconds, sr, bpm);
  const Stereo b = render([&](Rig& r) { base(r, false); }, seconds, sr, bpm);
  return diff(a, b);
}

static void testDelayTime() {
  struct Case { float sr; int bpm, div; };
  std::vector<Case> cases;
  for (int d = 0; d < 9; ++d) cases.push_back({44100.0f, 124, d});
  cases.push_back({44100.0f, 60, 8});    // 1/4 at 60 = 1 s: halved to 0.5 s
  cases.push_back({44100.0f, 60, 7});    // dotted 1/8 at 60 = 0.75 s: must still fit
  cases.push_back({44100.0f, 200, 0});   // shortest: 1/32 at 200 = 37.5 ms
  cases.push_back({32000.0f, 97, 5});
  cases.push_back({32000.0f, 60, 8});
  int k = 0;
  for (const Case& c : cases) {
    const double stepS = double(c.sr) * 15.0 / double(c.bpm);
    double expect = stepS * kDivSteps[c.div];
    const double maxD = 0.75 * 44100.0 * (double(c.sr) / 44100.0) * (c.sr < 40000.0f ? 1.385 : 1.0);   // informative only
    (void)maxD;
    const double cap = (c.sr == 44100.0f) ? 0.7501 * 44100.0 : 1.03 * 32000.0;
    while (expect > cap) expect *= 0.5;
    const Stereo w = delayWet([&](Rig& r) {
      r.proj.globals[kGDelayTime] = uint8_t(c.div);
      r.proj.globals[kGDelayFeedback] = 64;
      r.proj.globals[kGDelayWidth] = 0;
    }, (3.7 * expect) / double(c.sr) + 0.05, c.sr, c.bpm);
    const int hook = engine_detail::delayTimeSamples();
    const size_t D = size_t(expect + 0.5);
    std::vector<double> m(w.size());
    for (size_t i = 0; i < w.size(); ++i) m[i] = w.mono(i);
    const size_t e1 = peakIndex(m, D / 2, D + D / 2), e2 = peakIndex(m, D + D / 2, 2 * D + D / 2);
    const size_t e3 = peakIndex(m, 2 * D + D / 2, 3 * D + D / 2);
    const double spacing = double(e2) - double(e1);
    // Every echo is one more pass through the in-loop tone filter, whose phase
    // may move an echo's peak by up to ~0.1 ms: allow that on top of 2 samples.
    const double spacingTol = std::fmax(2.0, 1.0e-4 * double(c.sr));
    CHECKF(std::fabs(double(hook) - expect) <= 2.0, "sr %.0f bpm %d div %d: delay %d samples, expected %.1f", double(c.sr), c.bpm, c.div, hook, expect);
    CHECKF(std::fabs(spacing - expect) <= spacingTol, "sr %.0f bpm %d div %d: echo spacing %.0f, expected %.1f", double(c.sr), c.bpm, c.div, spacing, expect);
    CHECKF(std::fabs(double(e1) - expect) <= 8.0, "sr %.0f bpm %d div %d: first echo at %zu, expected %.1f (+latency)", double(c.sr), c.bpm, c.div, e1, expect);
    // Each echo is the previous one times the per-echo feedback and one more
    // pass of the tone filter (the high-pass trims a click's peak a little):
    // echo 2 / echo 1 is at most the feedback gain and not far below it at
    // this tone setting, and the third echo keeps decaying the same way.
    const double fbGain = 0.95 * 64.0 / 127.0;
    const double ratio = std::fabs(m[e2]) / std::fmax(1e-9, std::fabs(m[e1]));
    const double ratio23 = std::fabs(m[e3]) / std::fmax(1e-9, std::fabs(m[e2]));
    CHECKF(ratio > 0.8 * fbGain && ratio < fbGain + 0.03, "div %d: echo ratio %.3f for feedback %.3f", c.div, ratio, fbGain);
    CHECKF(ratio23 > 0.8 * fbGain && ratio23 < fbGain + 0.03, "div %d: echo 3/2 ratio %.3f for feedback %.3f", c.div, ratio23, fbGain);
    if (k++ % 3 == 0 || c.bpm == 60) std::printf("  delay sr %.0f bpm %3d div %d: %d samples (%.1f ms), echo spacing %.0f, first echo +%.0f\n",
                                                 double(c.sr), c.bpm, c.div, hook, 1000.0 * hook / double(c.sr), spacing, double(e1) - expect);
  }
}

static void testDelayStability() {
  // Max feedback, every tone extreme, full ping-pong, continuous loud noise for
  // 8 s: bounded and finite; then the input stops and it decays.
  for (int tone : {0, 64, 127}) {
    rig.init();
    rig.neutralMix();
    rig.v[kTrackPerc].mode = StubVoice::Noise;
    rig.v[kTrackPerc].amp = 0.8f;
    rig.proj.tracks[kTrackPerc].params[kMixDelaySend] = 127;
    rig.proj.tracks[kTrackPerc].params[kMixLevel] = 0;       // post-fader send: keep a little dry
    rig.proj.tracks[kTrackPerc].params[kMixLevel] = 40;
    rig.proj.globals[kGDelayFeedback] = 127;
    rig.proj.globals[kGDelayTone] = uint8_t(tone);
    rig.proj.globals[kGDelayWidth] = 127;
    rig.proj.globals[kGDelayTime] = 2;                        // 1/16: many recirculations per second
    rig.eng.init(44100.0f, &rig.proj);
    rig.capture = true;
    rig.runSeconds(8.0);
    rig.v[kTrackPerc].mode = StubVoice::Silent;
    rig.runSeconds(6.0);
    rig.capture = false;
    const Stereo s = toStereo(rig.out, 44100.0f);
    double peak = 0;
    bool finite = true;
    for (size_t i = 0; i < s.size(); ++i) { peak = std::fmax(peak, std::fabs(s.l[i])); finite &= std::isfinite(s.l[i]) && std::isfinite(s.r[i]); }
    const double e0 = energy(s.l, size_t(8.2 * 44100), size_t(9.2 * 44100)) + energy(s.r, size_t(8.2 * 44100), size_t(9.2 * 44100));
    const double e1 = energy(s.l, size_t(12.8 * 44100), size_t(13.8 * 44100)) + energy(s.r, size_t(12.8 * 44100), size_t(13.8 * 44100));
    const double steady = rmsOf(rig.out, long(6.0 * 44100), long(8.0 * 44100), 0);
    CHECK(finite && peak <= 1.0);
    CHECKF(e1 < e0 * 0.1, "tone %d: max-feedback tail does not decay (%.3g -> %.3g)", tone, e0, e1);
    CHECK(engine_detail::faultCount() == 0);
    std::printf("  delay fb max, tone %3d: steady rms %.1f dBFS, peak %.3f, tail decays %.1f dB in 4.6 s\n", tone, dB(steady), peak,
                10.0 * std::log10(std::fmax(e1, 1e-30) / e0));
  }
  // Time changes while echoes are running: cross-faded, no click. A sustained
  // tone through the delay; compare the largest sample step of the wet signal
  // with what the tone itself produces.
  rig.init();
  rig.neutralMix();
  rig.v[kTrackLead].mode = StubVoice::Tone;
  rig.v[kTrackLead].freq = 330.0f;
  rig.v[kTrackLead].amp = 0.3f;
  rig.proj.tracks[kTrackLead].params[kMixPan] = 0;            // dry hard left: right channel = wet only
  rig.proj.tracks[kTrackLead].params[kMixDelaySend] = 127;
  rig.proj.globals[kGDelayFeedback] = 90;
  rig.proj.globals[kGDelayWidth] = 0;
  rig.eng.init(44100.0f, &rig.proj);
  rig.capture = true;
  rig.post(CmdType::Play);
  rig.runSeconds(2.0);
  for (int i = 0; i < 12; ++i) {
    rig.post(CmdType::SetGlobal, kGDelayTime, 0, (i * 5) % 9);
    rig.runSeconds(0.13 + 0.05 * (i % 3));
    if (i == 5) rig.post(CmdType::SetGlobal, kGTempo, 0, 90);
  }
  rig.runSeconds(1.0);
  rig.capture = false;
  const Stereo s = toStereo(rig.out, 44100.0f);
  // Click detector: x[n+1] - 2cos(w)x[n] + x[n-1] is ~0 for a 330 Hz sinusoid
  // (also with slowly varying amplitude/phase); a discontinuity shows up as
  // a residual of about its own size.
  const double c2 = 2.0 * std::cos(6.283185307179586 * 330.0 / 44100.0);
  double steadyStep = 0, changeStep = 0;
  for (size_t i = size_t(1.0 * 44100); i < size_t(2.0 * 44100); ++i) steadyStep = std::fmax(steadyStep, std::fabs(s.r[i + 1] - c2 * s.r[i] + s.r[i - 1]));
  for (size_t i = size_t(2.0 * 44100); i + 1 < s.size(); ++i) changeStep = std::fmax(changeStep, std::fabs(s.r[i + 1] - c2 * s.r[i] + s.r[i - 1]));
  CHECKF(changeStep < 0.004, "delay time change: click residual %.4f (steady %.4f)", changeStep, steadyStep);
  std::printf("  delay time changes (12 changes + tempo change): click residual %.5f vs %.5f steady (wet ~0.3 peak)\n", changeStep, steadyStep);
}

// ----------------------------------------------------------------- reverb --

// Wet reverb return for an impulse (or a noise burst) on the perc track.
static Stereo reverbWet(int size, int damp, bool noise = false, double seconds = 3.0, float sr = 44100.0f, bool sustain = false) {
  auto base = [&](Rig& r, bool send) {
    r.v[kTrackPerc].mode = noise ? StubVoice::Gate : StubVoice::Impulse;
    r.v[kTrackPerc].amp = 0.6f;
    r.v[kTrackPerc].freq = 0.0f;
    r.step(0, kTrackPerc, 0) = mkStep(60, kStepOn, 100, 2);
    r.proj.patterns[0].length = 64;
    r.proj.patterns[0].tracks[kTrackPerc].length = 64;
    r.proj.tracks[kTrackPerc].params[kMixReverbSend] = send ? 127 : 0;
    r.proj.globals[kGReverbSize] = uint8_t(size);
    r.proj.globals[kGReverbDamp] = uint8_t(damp);
  };
  if (noise) {
    // Noise burst: the Noise stub is always active, so gate it from here.
    auto run = [&](bool send) {
      rig.init(sr, 124);
      rig.neutralMix();
      base(rig, send);
      rig.v[kTrackPerc].mode = StubVoice::Noise;
      rig.v[kTrackPerc].mode = StubVoice::Silent;
      rig.eng.init(sr, &rig.proj);
      rig.runSeconds(0.1);
      rig.v[kTrackPerc].mode = StubVoice::Noise;
      rig.capture = true;
      rig.runSeconds(0.05);
      if (!sustain) rig.v[kTrackPerc].mode = StubVoice::Silent;
      rig.runSeconds(seconds);
      rig.capture = false;
      return toStereo(rig.out, sr);
    };
    const Stereo a = run(true), b = run(false);
    return diff(a, b);
  }
  const Stereo a = render([&](Rig& r) { base(r, true); }, seconds, sr);
  const Stereo b = render([&](Rig& r) { base(r, false); }, seconds, sr);
  return diff(a, b);
}

// RT60 from the Schroeder integral of the mono-summed tail (fit -5 .. -25 dB).
static double rt60(const Stereo& w) {
  std::vector<double> e(w.size() + 1, 0.0);
  for (size_t i = w.size(); i-- > 0;) { const double l = w.l[i], r = w.r[i]; e[i] = e[i + 1] + 0.5 * (l * l + r * r); }
  const double e0 = e[0];
  double t5 = -1, t25 = -1;
  for (size_t i = 0; i < w.size(); ++i) {
    const double d = 10.0 * std::log10(std::fmax(e[i], 1e-300) / e0);
    if (t5 < 0 && d <= -5.0) t5 = double(i);
    if (t25 < 0 && d <= -25.0) { t25 = double(i); break; }
  }
  if (t5 < 0 || t25 < 0) return -1;
  return 3.0 * (t25 - t5) / double(w.sr);
}

// Normalised echo density (Abel & Huang): fraction of samples outside one
// standard deviation in a window, divided by the Gaussian value 0.3173.
static double echoDensity(const std::vector<double>& x, size_t a, size_t b) {
  double s2 = 0;
  for (size_t i = a; i < b; ++i) s2 += x[i] * x[i];
  const double sd = std::sqrt(s2 / double(b - a));
  size_t out = 0;
  for (size_t i = a; i < b; ++i) out += std::fabs(x[i]) > sd;
  return double(out) / double(b - a) / 0.3173;
}

// Welch-averaged power spectrum (naive DFT, bins lo..hi of an n-point frame).
__attribute__((noinline)) static std::vector<double> welch(const std::vector<double>& x, size_t start, int n, int frames, int lo, int hi) {
  std::vector<double> p(size_t(hi - lo), 0.0);
  std::vector<double> win(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) win[size_t(i)] = 0.5 - 0.5 * std::cos(6.283185307179586 * i / n);
  for (int f = 0; f < frames; ++f) {
    const size_t o = start + size_t(f) * size_t(n / 2);
    for (int k = lo; k < hi; ++k) {
      double re = 0, im = 0;
      const double w = 6.283185307179586 * k / n;
      // recurrence for cos/sin
      double c = 1, s = 0;
      const double cw = std::cos(w), sw = std::sin(w);
      for (int i = 0; i < n; ++i) {
        const double v = x[o + size_t(i)] * win[size_t(i)];
        re += v * c; im -= v * s;
        const double c2 = c * cw - s * sw;
        s = s * cw + c * sw;
        c = c2;
      }
      p[size_t(k - lo)] += re * re + im * im;
    }
  }
  return p;
}
// Largest ratio of a bin over the median of its neighbourhood (+-24 bins), in dB.
static double peakiness(const std::vector<double>& p) {
  double worst = 0;
  std::vector<double> nb;
  for (size_t k = 24; k + 24 < p.size(); ++k) {
    nb.assign(p.begin() + long(k) - 24, p.begin() + long(k) + 25);
    std::nth_element(nb.begin(), nb.begin() + 24, nb.end());
    worst = std::fmax(worst, p[k] / nb[24]);
  }
  return 10.0 * std::log10(worst);
}

static void testReverb() {
  // Decay: finite, never grows, RT60 follows the size knob.
  double prevRt = 0;
  for (int size : {0, 32, 64, 96, 127}) {
    const double nominal = 0.3 * std::pow(2.0, 4.0589 * size / 127.0);
    // RT by the interrupted-noise method (50 ms burst): an impulse puts only
    // a few dozen LSB into each int16 line, too little range for a -25 dB fit.
    const Stereo nb = reverbWet(size, 0, true, std::fmin(3.0 * nominal + 0.6, 12.0));
    const double rt = rt60(nb);
    // Impulse: finite, never grows, decays to nothing.
    const Stereo w = reverbWet(size, 0, false, std::fmin(3.0 * nominal + 0.6, 7.5));   // the 64-step pattern re-fires at 7.7 s
    bool finite = true;
    for (size_t i = 0; i < w.size(); ++i) finite &= std::isfinite(w.l[i]) && std::isfinite(w.r[i]);
    CHECK(finite);
    CHECKF(rt > 0.6 * nominal && rt < 1.25 * nominal, "size %d: RT60 %.2f s, nominal %.2f s", size, rt, nominal);
    CHECKF(rt > prevRt * 1.2, "size %d: RT60 %.2f s does not grow with size (prev %.2f)", size, rt, prevRt);
    prevRt = rt;
    // no growth: 50 ms window energies after the build-up only go down (allow small fluctuation)
    const size_t win = size_t(0.05 * 44100);
    double prev = 1e300, worstRise = 0;
    for (size_t a = size_t(0.12 * 44100); a + win < w.size(); a += win) {
      const double e = energy(w.l, a, a + win) + energy(w.r, a, a + win);
      if (e < 1e-10) break;
      worstRise = std::fmax(worstRise, e / prev);
      prev = e;
    }
    CHECKF(worstRise < 1.25, "size %d: tail energy rose by %.2fx between windows", size, worstRise);
    std::printf("  reverb size %3d: RT60 %.2f s (nominal %.2f), worst window-to-window energy ratio %.2f\n", size, rt, nominal, worstRise);
  }
  // Damping: high damping shortens the high-frequency decay (the tail gets darker).
  {
    const Stereo bright = reverbWet(64, 0, true, 1.5), dark = reverbWet(64, 127, true, 1.5);
    auto hfShare = [](const Stereo& w) {
      double hf = 0, all = 0;
      for (size_t i = size_t(0.3 * 44100); i + 1 < size_t(0.8 * 44100); ++i) {
        const double d = w.l[i + 1] - w.l[i];
        hf += d * d;
        all += w.l[i] * w.l[i];
      }
      return hf / all;
    };
    CHECKF(hfShare(dark) < 0.5 * hfShare(bright), "damping does not darken the tail (%.4f vs %.4f)", hfShare(dark), hfShare(bright));
  }
  // Density and colour at the default setting: dense within ~60 ms, and the
  // tail spectrum has no isolated resonances (metallic ringing).
  {
    const ParamInfo* sz = Engine::globalParamInfo(kGReverbSize);
    const ParamInfo* dm = Engine::globalParamInfo(kGReverbDamp);
    const Stereo w = reverbWet(sz->def, dm->def, false, 1.6);
    const double ned = echoDensity(w.l, size_t(0.06 * 44100), size_t(0.08 * 44100));
    const double ned2 = echoDensity(w.l, size_t(0.15 * 44100), size_t(0.2 * 44100));
    CHECKF(ned > 0.75 && ned2 > 0.85, "echo density %.2f at 60-80 ms, %.2f at 150-200 ms", ned, ned2);
    const std::vector<double> p = welch(w.l, size_t(0.1 * 44100), 2048, 10, 6, 400);   // 130 Hz .. 8.6 kHz
    const double pk = peakiness(p);
    CHECKF(pk < 9.0, "reverb tail has a resonance %.1f dB above its neighbourhood", pk);
    std::printf("  reverb default: echo density %.2f (60-80 ms), %.2f (150-200 ms); worst spectral peak %.1f dB over local median\n", ned, ned2, pk);
  }
  // Idle detection: the tail reaches exact digital silence and the FX are skipped.
  {
    rig.init();
    rig.neutralMix();
    rig.v[kTrackPerc].mode = StubVoice::Impulse;
    rig.step(0, kTrackPerc, 0) = mkStep(60);
    rig.proj.patterns[0].length = 64;
    rig.proj.patterns[0].tracks[kTrackPerc].length = 64;
    rig.proj.tracks[kTrackPerc].params[kMixReverbSend] = 127;
    rig.proj.globals[kGReverbSize] = 127;
    rig.eng.init(44100.0f, &rig.proj);
    rig.runSeconds(0.1);
    rig.post(CmdType::Play);
    rig.run(4);
    rig.post(CmdType::Stop);
    double t = 0;
    while (!engine_detail::fxIdle() && t < 40.0) { rig.runSeconds(0.25); t += 0.25; }
    CHECKF(engine_detail::fxIdle(), "reverb at max size never went idle");
    std::printf("  reverb max size: digital silence and FX idle %.1f s after one impulse\n", t);
  }
}

// ----------------------------------------------------------------- master --

static void testMaster() {
  // Bypass (64) is bit-transparent: noise at 0.3 comes out as round(x * 32767).
  // Also after the filter has been swept away and back.
  auto expectNoise = [](size_t n) {
    std::vector<int16_t> v(n);
    uint32_t seed = 12345u;
    for (size_t i = 0; i < n; ++i) {
      seed = seed * 1664525u + 1013904223u;
      const float x = 0.3f * (float(int32_t(seed)) / 2147483648.0f);
      v[i] = int16_t(int32_t(x * 32767.0f + (x < 0 ? -0.5f : 0.5f)));
    }
    return v;
  };
  for (float sr : {44100.0f, 32000.0f}) {
    rig.init(sr);
    rig.neutralMix();
    rig.v[kTrackSnare].mode = StubVoice::Noise;
    rig.v[kTrackSnare].amp = 0.3f;
    rig.eng.init(sr, &rig.proj);
    rig.capture = true;
    rig.runSeconds(0.5);
    const long n0 = rig.frames();
    std::vector<int16_t> ex = expectNoise(size_t(n0) * 4);
    long bad = 0;
    for (long f = 4410; f < n0; ++f) bad += std::abs(int(rig.out[size_t(f) * 2]) - int(ex[size_t(f)])) > 1 || rig.out[size_t(f) * 2] != rig.out[size_t(f) * 2 + 1];
    CHECKF(bad == 0, "sr %.0f: bypass not transparent in %ld samples", double(sr), bad);
    // sweep away (LP, then HP) and back
    for (int v = 64; v >= 0; v -= 4) { rig.post(CmdType::SetGlobal, kGMasterFilter, 0, v); rig.run(8); }
    for (int v = 0; v <= 127; v += 4) { rig.post(CmdType::SetGlobal, kGMasterFilter, 0, v); rig.run(8); }
    rig.post(CmdType::SetGlobal, kGMasterFilter, 0, 64);
    rig.runSeconds(0.3);
    const long n1 = rig.frames();
    rig.runSeconds(0.3);
    bad = 0;
    for (long f = n1; f < rig.frames(); ++f) bad += std::abs(int(rig.out[size_t(f) * 2]) - int(ex[size_t(f)])) > 1;
    CHECKF(bad == 0, "sr %.0f: not transparent after returning to 64 (%ld samples)", double(sr), bad);
    rig.capture = false;
  }
  // Filter response: LP at 0 kills highs, HP at 127 kills lows; the knob is monotonic.
  auto toneThrough = [](float freq, int filt, int reso) {
    const Stereo s = render([&](Rig& r) {
      r.v[kTrackSnare].mode = StubVoice::Tone;
      r.v[kTrackSnare].freq = freq;
      r.v[kTrackSnare].amp = 0.3f;
      r.proj.globals[kGMasterFilter] = uint8_t(filt);
      r.proj.globals[kGMasterReso] = uint8_t(reso);
    }, 0.6);
    double e = 0;
    for (size_t i = size_t(0.3 * 44100); i < s.size(); ++i) e += s.l[i] * s.l[i];
    return std::sqrt(e / double(s.size() - size_t(0.3 * 44100))) / (0.3 / std::sqrt(2.0));
  };
  CHECK(toneThrough(5000.0f, 0, 0) < 0.01 && toneThrough(60.0f, 0, 0) > 0.7);
  CHECK(toneThrough(60.0f, 127, 0) < 0.01 && toneThrough(14000.0f, 127, 0) > 0.7);
  double prev = 2;
  bool mono = true;
  for (int f = 63; f >= 0; f -= 7) { const double g = toneThrough(1000.0f, f, 0); mono &= g <= prev + 0.02; prev = g; }
  CHECKF(mono, "LP sweep is not monotonic at 1 kHz");
  const double peakRes = toneThrough(1000.0f, 40, 127);
  std::printf("  DJ filter: LP0 @5k %.3f, HP127 @60 %.3f, 1 kHz at max reso near cutoff %.2f\n", toneThrough(5000.0f, 0, 0), toneThrough(60.0f, 127, 0), peakRes);

  // Sweep stability: max resonance, full sweeps both ways at several speeds,
  // hot noise + low tone, both rates. Finite, bounded, no faults.
  for (float sr : {44100.0f, 32000.0f}) {
    rig.init(sr);
    rig.neutralMix();
    rig.v[kTrackSnare].mode = StubVoice::Noise;
    rig.v[kTrackSnare].amp = 0.6f;
    rig.v[kTrackBass].mode = StubVoice::Tone;
    rig.v[kTrackBass].freq = 55.0f;
    rig.v[kTrackBass].amp = 0.6f;
    rig.proj.globals[kGMasterReso] = 127;
    rig.proj.globals[kGMasterDrive] = 127;
    rig.eng.init(sr, &rig.proj);
    rig.capture = true;
    for (int speed : {1, 3, 16}) {
      for (int v = 0; v <= 127; ++v) { rig.post(CmdType::SetGlobal, kGMasterFilter, 0, v); rig.run(speed); }
      for (int v = 127; v >= 0; --v) { rig.post(CmdType::SetGlobal, kGMasterFilter, 0, v); rig.run(speed); }
    }
    for (int i = 0; i < 400; ++i) { rig.post(CmdType::SetGlobal, kGMasterFilter, 0, (i * 37) % 128); rig.post(CmdType::SetGlobal, kGMasterReso, 0, (i * 53) % 128); rig.run(1); }
    rig.capture = false;
    const EngineStatus st = rig.eng.status();
    CHECK(std::isfinite(st.peakL) && std::isfinite(st.peakR) && st.peakL <= 1.0f);
    CHECKF(engine_detail::faultCount() == 0, "sr %.0f: %u faults during filter sweeps", double(sr), engine_detail::faultCount());
    // the output did not latch onto a rail (a blown-up state would hold +-1)
    long rail = 0;
    for (size_t i = 0; i < rig.out.size(); ++i) rail += std::abs(int(rig.out[i])) >= 32767;
    CHECKF(rail < long(rig.out.size() / 50), "sr %.0f: %ld samples at the rail during sweeps", double(sr), rail);
  }
}

static void testLimiter() {
  // All 8 tracks hot (DC, noise, square-ish blips) at max level, max master
  // volume, with and without drive / filter resonance: the int16 output never
  // wraps (the sign always follows the input) and never exceeds full scale.
  for (int drive : {0, 127}) {
    for (int sign : {1, -1}) {
      rig.init();
      rig.neutralMix();
      for (int t = 0; t < kNumTracks; ++t) {
        rig.v[t].mode = StubVoice::Dc;
        rig.v[t].amp = 1.5f * float(sign);
        rig.proj.tracks[t].params[kMixLevel] = 127;
        rig.proj.tracks[t].params[kMixDelaySend] = 127;
        rig.proj.tracks[t].params[kMixReverbSend] = 127;
      }
      rig.proj.globals[kGMasterVolume] = 127;
      rig.proj.globals[kGMasterDrive] = uint8_t(drive);
      rig.proj.globals[kGDelayFeedback] = 127;
      rig.eng.init(44100.0f, &rig.proj);
      rig.capture = true;
      rig.runSeconds(2.0);
      rig.capture = false;
      long wrong = 0;
      // (the drive's gain compensation brings a fully saturated bus down to ~-3 dBFS)
      for (size_t i = 2 * 4410; i < rig.out.size(); ++i) wrong += (sign > 0 ? rig.out[i] < 16000 : rig.out[i] > -16000);
      CHECKF(wrong == 0, "drive %d sign %d: %ld samples wrapped or fell back from the rail", drive, sign, wrong);
    }
  }
  for (int drive : {0, 127}) {
    rig.init();
    rig.neutralMix();
    for (int t = 0; t < kNumTracks; ++t) {
      rig.v[t].mode = StubVoice::Noise;
      rig.v[t].seed = 777u + uint32_t(t) * 99u;
      rig.v[t].amp = 2.0f;
      rig.proj.tracks[t].params[kMixLevel] = 127;
      rig.proj.tracks[t].params[kMixReverbSend] = 127;
    }
    rig.proj.globals[kGMasterVolume] = 127;
    rig.proj.globals[kGMasterDrive] = uint8_t(drive);
    rig.proj.globals[kGMasterFilter] = 20;
    rig.proj.globals[kGMasterReso] = 127;
    rig.eng.init(44100.0f, &rig.proj);
    rig.capture = true;
    rig.runSeconds(2.0);
    rig.capture = false;
    const EngineStatus st = rig.eng.status();
    CHECK(st.peakL <= 1.0f && st.peakR <= 1.0f);
    int16_t mn = 0, mx = 0;
    for (int16_t x : rig.out) { mn = std::min(mn, x); mx = std::max(mx, x); }
    CHECK(mn >= -32767 && mx <= 32767 && mx > 30000 && mn < -30000);
  }
  // Transfer curve: transparent below the knee, monotonic, ceiling at full scale.
  double prevOut = -1;
  bool monotonic = true;
  for (int i = 0; i <= 40; ++i) {
    const float x = 0.05f * float(i);
    const Stereo s = render([&](Rig& r) { r.v[kTrackSnare].mode = StubVoice::Dc; r.v[kTrackSnare].amp = x; }, 0.02);
    const double y = s.l.back();
    if (x < 0.7f) CHECKF(std::fabs(y - double(x)) < 1.0 / 32767.0 + 1e-6, "limiter not transparent at %.2f (%.5f)", double(x), y);
    monotonic &= y >= prevOut;
    prevOut = y;
  }
  CHECK(monotonic && prevOut <= 1.0 && prevOut > 0.999);
}

// Drive is gain-compensated: louder and denser, but not runaway.
static void testDrive() {
  double rms[3];
  int k = 0;
  for (int drive : {0, 64, 127}) {
    const Stereo s = render([&](Rig& r) {
      r.v[kTrackBass].mode = StubVoice::Tone;
      r.v[kTrackBass].freq = 110.0f;
      r.v[kTrackBass].amp = 0.4f;
      r.proj.globals[kGMasterDrive] = uint8_t(drive);
    }, 0.5);
    rms[k++] = std::sqrt(energy(s.l, size_t(0.1 * 44100), s.size()) / double(s.size() - size_t(0.1 * 44100)));
  }
  CHECKF(rms[1] > rms[0] && rms[2] < rms[0] * 2.6, "drive levels %.3f %.3f %.3f", rms[0], rms[1], rms[2]);
  std::printf("  drive on a 0.4 sine: rms %.1f / %.1f / %.1f dBFS at drive 0 / 64 / 127\n", dB(rms[0]), dB(rms[1]), dB(rms[2]));
}

// ------------------------------------------------------------- mono sum --

static void testMonoSum() {
  // 1. Ping-pong: the mono sum of the wet delay is the same at any width.
  {
    auto wet = [](int width) {
      return delayWet([&](Rig& r) {
        r.proj.globals[kGDelayWidth] = uint8_t(width);
        r.proj.globals[kGDelayFeedback] = 100;
        r.proj.globals[kGDelayTime] = 5;
      }, 2.0);
    };
    const Stereo w0 = wet(0), w64 = wet(64), w127 = wet(127);
    double eM0 = monoEnergy(w0, 0, w0.size()), eM127 = monoEnergy(w127, 0, w127.size()), eM64 = monoEnergy(w64, 0, w64.size());
    double maxDiff = 0;
    for (size_t i = 0; i < w0.size(); ++i) maxDiff = std::fmax(maxDiff, std::fabs(w0.mono(i) - w127.mono(i)));
    double eSt = 0.5 * (energy(w127.l, 0, w127.size()) + energy(w127.r, 0, w127.size()));
    CHECKF(maxDiff < 2.5 / 32767.0, "ping-pong mono sum differs from mono delay by %.2e", maxDiff);
    CHECK(std::fabs(10.0 * std::log10(eM127 / eM0)) < 0.05 && std::fabs(10.0 * std::log10(eM64 / eM0)) < 0.05);
    // and it really is ping-pong in stereo: echo 1 on the left, echo 2 on the right
    const double L = 44100.0 * 15.0 / 124.0 * 2.0;
    const size_t e1 = size_t(L), e2 = size_t(2 * L);
    const double l1 = energy(w127.l, e1 - 200, e1 + 400), r1 = energy(w127.r, e1 - 200, e1 + 400);
    const double l2 = energy(w127.l, e2 - 200, e2 + 400), r2 = energy(w127.r, e2 - 200, e2 + 400);
    CHECKF(l1 > 100 * r1 && r2 > 100 * l2, "full width is not ping-pong (%.2e %.2e / %.2e %.2e)", l1, r1, l2, r2);
    std::printf("  mono sum, delay: width 0/64/127 mono energy %.2f / %.2f / %.2f dB re width 0 (max sample diff %.1e); stereo channels +%.1f dB\n",
                0.0, 10.0 * std::log10(eM64 / eM0), 10.0 * std::log10(eM127 / eM0), maxDiff, 10.0 * std::log10(eSt / eM127));
  }
  // 2. Reverb: summing to mono neither cancels (level) nor colours (comb
  //    notches) the return. Noise burst in, compare mono vs per-channel power,
  //    overall and per octave band.
  {
    // sustained noise into the send, measured once the tank is in steady state
    const Stereo w = reverbWet(Engine::globalParamInfo(kGReverbSize)->def, Engine::globalParamInfo(kGReverbDamp)->def, true, 4.0, 44100.0f, true);
    const size_t a = size_t(1.0 * 44100), b = w.size();
    const double eM = monoEnergy(w, a, b), eCh = 0.5 * (energy(w.l, a, b) + energy(w.r, a, b));
    double xy = 0;
    for (size_t i = a; i < b; ++i) xy += w.l[i] * w.r[i];
    const double rho = xy / std::sqrt(energy(w.l, a, b) * energy(w.r, a, b));
    const double lossDb = 10.0 * std::log10(eM / eCh);
    std::vector<double> m(w.size());
    for (size_t i = 0; i < w.size(); ++i) m[i] = w.mono(i);
    const std::vector<double> pm = welch(m, a, 1024, 240, 3, 200), pl = welch(w.l, a, 1024, 240, 3, 200), pr = welch(w.r, a, 1024, 240, 3, 200);
    double worstBand = 0, bestBand = -99;
    for (int lo = 3; lo < 200; lo *= 2) {
      const int hi = std::min(200, lo * 2);
      double sm = 0, sc = 0;
      for (int k = lo; k < hi; ++k) { sm += pm[size_t(k - 3)]; sc += 0.5 * (pl[size_t(k - 3)] + pr[size_t(k - 3)]); }
      const double d = 10.0 * std::log10(sm / sc);
      worstBand = std::fmin(worstBand, d);
      bestBand = std::fmax(bestBand, d);
    }
    std::printf("  mono sum, reverb: L/R correlation %.2f, mono power %.1f dB re channel power, octave bands %.1f .. %.1f dB\n", rho, lossDb,
                worstBand, bestBand);
    // Mono = the mid tap set; each stereo channel = mid + 0.75 side, so the
    // channels carry 1 + 0.75^2 = +1.9 dB more than the mono sum, flat across
    // the spectrum. (Two decorrelated channels summed would give -3 dB and
    // comb at low frequencies.)
    CHECKF(lossDb > -2.4 && lossDb < -1.4, "reverb mono sum %.1f dB re channel power (expect -1.9)", lossDb);
    CHECKF(bestBand - worstBand < 1.5 && worstBand > -3.0, "reverb mono sum is coloured (bands %.1f .. %.1f dB)", worstBand, bestBand);
    CHECK(rho < 0.75);    // still a stereo reverb
  }
  // 3. The default mix (8 blip voices through the default sends) keeps its
  //    level in mono: nothing in the engine is out of phase between channels.
  {
    const Stereo s = render([](Rig& r) {
      r.eng.initProject(r.proj);
      r.proj.globals[kGTempo] = 124;
      for (int t = 0; t < kNumTracks; ++t) {
        r.v[t].mode = StubVoice::Blip;
        r.v[t].amp = 0.45f;
        r.v[t].decaySec = t == kTrackBass || t == kTrackLead ? 0.15f : 0.05f;
        for (int i = 0; i < 16; ++i)
          if ((i + t) % (t == kTrackKick ? 4 : 3) == 0) r.step(0, t, i) = mkStep(36 + 7 * t);
      }
    }, 4.0);
    const double eM = monoEnergy(s, 0, s.size()), eCh = 0.5 * (energy(s.l, 0, s.size()) + energy(s.r, 0, s.size()));
    CHECKF(10.0 * std::log10(eM / eCh) > -1.0, "default mix loses %.2f dB in mono", 10.0 * std::log10(eM / eCh));
    std::printf("  mono sum, default mix: %.2f dB re channel power\n", 10.0 * std::log10(eM / eCh));
  }
}

// ---------------------------------------------------------- default mix --

static void testDefaultLoudness() {
  // Eight voices peaking at -7 dBFS (0.45) each, a busy house bar, default mixer
  // and globals: loud, peaks under full scale, limiter only grazed.
  rig.init();
  for (int t = 0; t < kNumTracks; ++t) {
    rig.v[t].mode = StubVoice::Blip;
    rig.v[t].amp = 0.45f;
    rig.v[t].decaySec = t == kTrackKick ? 0.25f : (t >= kTrackBass ? 0.12f : 0.06f);
  }
  rig.v[kTrackKick].freq = 55.0f;
  for (int i = 0; i < 16; ++i) {
    if (i % 4 == 0) rig.step(0, kTrackKick, i) = mkStep(33);
    if (i % 8 == 4) rig.step(0, kTrackClap, i) = mkStep(72);
    if (i % 8 == 4) rig.step(0, kTrackSnare, i) = mkStep(74);
    if (i % 2 == 0) rig.step(0, kTrackHatClosed, i) = mkStep(96);
    if (i % 4 == 2) rig.step(0, kTrackHatOpen, i) = mkStep(98);
    if (i % 3 == 1) rig.step(0, kTrackPerc, i) = mkStep(80);
    if (i % 4 != 0) rig.step(0, kTrackBass, i) = mkStep(45 + (i % 3) * 3);
    if (i % 6 == 3) rig.step(0, kTrackLead, i) = mkStep(64);
  }
  rig.eng.init(44100.0f, &rig.proj);
  rig.capture = true;
  rig.post(CmdType::Play);
  rig.runSeconds(8.0);
  rig.capture = false;
  const double peak = std::fmax(peakOf(rig.out, 0, rig.frames(), 0), peakOf(rig.out, 0, rig.frames(), 1));
  const double rms = rmsOf(rig.out, 0, rig.frames(), 0);
  long limited = 0;
  for (size_t i = 0; i < rig.out.size(); ++i) limited += std::abs(int(rig.out[i])) > int(0.7071 * 32767.0);
  const double frac = double(limited) / double(rig.out.size());
  std::printf("  default mix, 8 voices at -7 dBFS: peak %.1f dBFS, rms %.1f dBFS, %.2f %% of samples in the limiter knee\n", dB(peak), dB(rms), 100.0 * frac);
  CHECK(peak <= 1.0 && dB(rms) > -22.0 && dB(rms) < -9.0 && frac < 0.02);
  rig.writeWav("default_mix_blips.wav");
}

// ---------------------------------------------------------------- renders --

static void renderFx() {
  // Delay and reverb on short blips: two bars of dry-ish lead blips with the
  // default dotted-8th delay, then two bars of clap with a big reverb, then tails.
  rig.init();
  rig.v[kTrackLead].mode = StubVoice::Blip;
  rig.v[kTrackLead].amp = 0.45f;
  rig.v[kTrackLead].decaySec = 0.04f;
  rig.v[kTrackClap].mode = StubVoice::Blip;
  rig.v[kTrackClap].amp = 0.45f;
  rig.v[kTrackClap].decaySec = 0.02f;
  rig.proj.patterns[0].length = 32;
  rig.proj.patterns[0].tracks[kTrackLead].length = 32;
  rig.proj.patterns[0].tracks[kTrackClap].length = 32;
  rig.step(0, kTrackLead, 0) = mkStep(72);
  rig.step(0, kTrackLead, 6) = mkStep(79);
  rig.step(0, kTrackLead, 10) = mkStep(76);
  rig.step(0, kTrackClap, 20) = mkStep(84);
  rig.step(0, kTrackClap, 28) = mkStep(84);
  rig.proj.tracks[kTrackLead].params[kMixDelaySend] = 110;
  rig.proj.tracks[kTrackLead].params[kMixPan] = 64;
  rig.proj.tracks[kTrackClap].params[kMixReverbSend] = 120;
  rig.proj.globals[kGReverbSize] = 90;
  rig.proj.globals[kGDuckSource] = 0;
  rig.eng.init(44100.0f, &rig.proj);
  rig.capture = true;
  rig.post(CmdType::Play);
  const double bar = 4.0 * 60.0 / 124.0;
  rig.runSeconds(2.0 * bar);
  rig.post(CmdType::Stop);
  rig.runSeconds(3.0);
  rig.capture = false;
  CHECK(rig.writeWav("delay_reverb_blips.wav"));
  std::printf("  render build/renders/engine/delay_reverb_blips.wav, default_mix_blips.wav\n");
}

int main() {
  testMixer();
  testDelayTime();
  testDelayStability();
  testReverb();
  testMaster();
  testLimiter();
  testDrive();
  testMonoSum();
  testDefaultLoudness();
  renderFx();
  CHECK(engine_detail::faultCount() == 0);
  return finish("test_engine_fx");
}
