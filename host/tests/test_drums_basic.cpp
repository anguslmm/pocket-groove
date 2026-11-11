// Drum voices: behaviour at defaults. Levels, DC, decay to inactive, kick
// tuning and transposition, retrigger / choke continuity, velocity and
// accent response, and spectral sanity checks ("listening by measurement").
#include <string>
#include "tests/drums_testutil.h"

using namespace pg::drums;
using dt::Params;

namespace {
dt::Checker T;

struct VoiceCase {
  const char* name;
  pg::Voice* voice;
  Params params;
  float peakLo, peakHi;   // expected full-velocity peak
  float maxSeconds;       // must be inactive by then
};

Params percModel(PercVoice& p, int model) {
  Params q = dt::defaults(p);
  q[PercVoice::kModel] = uint8_t(model);
  return q;
}

// Time (s) at which the 5 ms RMS envelope last exceeds `db` relative to its maximum.
float timeAbove(const std::vector<float>& x, float fs, float db) {
  const size_t w = size_t(0.005f * fs);
  float mx = 0.0f;
  std::vector<float> env;
  for (size_t i = 0; i + w <= x.size(); i += w) {
    env.push_back(dt::rms(x, i, i + w));
    mx = env.back() > mx ? env.back() : mx;
  }
  const float thr = mx * std::pow(10.0f, db / 20.0f);
  size_t last = 0;
  for (size_t i = 0; i < env.size(); ++i) if (env[i] > thr) last = i;
  return float(last + 1) * float(w) / fs;
}

void testDefaults(float fs) {
  std::printf("\n== defaults at %.0f Hz: level, DC, decay ==\n", double(fs));
  KickVoice kick; SnareVoice snare; ClapVoice clap; HatVoice ch(false), oh(true);
  PercVoice tom, rim, cow, clave, shaker, zap;
  std::vector<VoiceCase> cases = {
      {"kick", &kick, dt::defaults(kick), 0.46f, 0.54f, 1.5f},
      {"snare", &snare, dt::defaults(snare), 0.26f, 0.40f, 1.0f},
      {"clap", &clap, dt::defaults(clap), 0.22f, 0.38f, 1.5f},
      {"closed hat", &ch, dt::defaults(ch), 0.12f, 0.30f, 0.4f},
      {"open hat", &oh, dt::defaults(oh), 0.12f, 0.30f, 2.5f},
      {"perc tom", &tom, percModel(tom, PercVoice::kTom), 0.24f, 0.34f, 1.5f},
      {"perc rim (default model)", &rim, dt::defaults(rim), 0.18f, 0.28f, 0.4f},
      {"perc cowbell", &cow, percModel(cow, PercVoice::kCowbell), 0.14f, 0.28f, 1.5f},
      {"perc clave", &clave, percModel(clave, PercVoice::kClave), 0.16f, 0.26f, 0.5f},
      {"perc shaker", &shaker, percModel(shaker, PercVoice::kShaker), 0.08f, 0.20f, 0.8f},
      {"perc zap", &zap, percModel(zap, PercVoice::kZap), 0.18f, 0.28f, 1.5f},
  };
  for (VoiceCase& c : cases) {
    std::printf("%s\n", c.name);
    c.voice->init(fs);
    T.check(!c.voice->active(), "inactive after init");
    T.check(dt::numParams(*c.voice) >= 6 && dt::numParams(*c.voice) <= 12, "6..12 params");
    for (int i = 0; i < dt::numParams(*c.voice); ++i) {
      const pg::ParamInfo* pi = c.voice->paramInfo(i);
      T.check(pi->min <= pi->def && pi->def <= pi->max, "default inside range");
      T.check(std::char_traits<char>::length(pi->name) <= 4, "short name <= 4 chars");
      T.check(std::char_traits<char>::length(pi->longName) <= 12, "long name <= 12 chars");
    }
    T.check(c.voice->paramInfo(dt::numParams(*c.voice)) == nullptr, "unused slots are nullptr");

    std::vector<char> act;
    const float hitAt = 0.01f;
    std::vector<float> x = dt::run(*c.voice, c.params, {dt::hit(hitAt, fs, 127)}, c.maxSeconds + 0.5f, fs, &act);
    T.check(dt::allFinite(x), "finite");
    const float pk = dt::peak(x);
    T.range(pk, c.peakLo, c.peakHi, "peak, vel 127");
    // DC: mean over the sounding part, relative to the peak.
    // act[q] is active() after quantum q, so the last rendered quantum is one past the last 1.
    size_t lastActive = 0;
    for (size_t q = 0; q + 1 < act.size(); ++q) if (act[q]) lastActive = q + 1;
    const float offTime = float(lastActive + 1) * float(pg::kQuantum) / fs - hitAt;
    std::vector<float> sounding(x.begin(), x.begin() + long((lastActive + 1) * pg::kQuantum));
    T.range(std::fabs(dt::mean(sounding)) / pk, 0.0f, 0.02f, "DC offset / peak");
    T.range(offTime, 0.01f, c.maxSeconds, "seconds until active() goes false");
    T.check(!act.back(), "inactive at the end");
    // Tail level when the voice switches off: about -80 dBFS or below.
    const float tail = dt::peak(x, lastActive * pg::kQuantum, (lastActive + 1) * pg::kQuantum);
    T.range(dt::dB(tail), -200.0f, -74.0f, "level of last active quantum (dBFS)");
    T.range(timeAbove(x, fs, -40.0f) - hitAt, 0.0f, c.maxSeconds, "seconds to -40 dB");
  }
}

void testKickTuning(float fs) {
  std::printf("\n== kick tuning at %.0f Hz ==\n", double(fs));
  KickVoice kick;
  Params p = dt::defaults(kick);
  p[KickVoice::kDecay] = 100;  // long tail so there are many cycles to measure
  p[KickVoice::kClick] = 0;
  const float want = 28.0f * std::pow(2.0f, 1.85f * float(p[KickVoice::kTune]) / 127.0f);
  auto measure = [&](uint8_t note) {
    kick.init(fs);
    std::vector<float> x = dt::run(kick, p, {dt::hit(0.0f, fs, 127, note)}, 1.2f, fs);
    // Late in the decay the pitch sweep has finished.
    return dt::zeroCrossFreq(x, size_t(0.4f * fs), size_t(1.1f * fs), fs);
  };
  const float f60 = measure(60);
  std::printf("  default tune: want %.2f Hz, measured %.2f Hz\n", double(want), double(f60));
  T.range(f60 / want, 0.995f, 1.005f, "fundamental / intended (note 60)");
  T.range(f60, 45.0f, 60.0f, "default kick fundamental in house range (Hz)");
  const int notes[5] = {48, 55, 67, 72, 84};
  for (int n : notes) {
    const float f = measure(uint8_t(n));
    const float ratio = std::pow(2.0f, float(n - 60) / 12.0f);
    char what[64];
    std::snprintf(what, sizeof what, "note %d: measured / expected ratio", n);
    T.range(f / (f60 * ratio), 0.997f, 1.003f, what);
  }
  // Early pitch must be well above the fundamental (the sweep), default params.
  kick.init(fs);
  Params d = dt::defaults(kick);
  d[KickVoice::kClick] = 0;
  std::vector<float> x = dt::run(kick, d, {dt::hit(0.0f, fs)}, 0.5f, fs);
  const float early = dt::zeroCrossFreq(x, 0, size_t(0.012f * fs), fs);
  T.range(early, 120.0f, 400.0f, "pitch in first 12 ms (Hz)");
  // Tune knob range.
  for (int v : {0, 127}) {
    p[KickVoice::kTune] = uint8_t(v);
    const float f = measure(60);
    const float w = 28.0f * std::pow(2.0f, 1.85f * float(v) / 127.0f);
    T.range(f / w, 0.99f, 1.01f, v == 0 ? "TUNE=0 (28 Hz) ratio" : "TUNE=127 (101 Hz) ratio");
  }
}

void testDeterminism(float fs) {
  std::printf("\n== kick: every hit identical ==\n");
  KickVoice kick;
  kick.init(fs);
  const Params p = dt::defaults(kick);
  std::vector<float> x = dt::run(kick, p, {dt::hit(0.0f, fs), dt::hit(2.0f, fs), dt::hit(4.0f, fs)}, 6.0f, fs);
  const size_t n = size_t(1.5f * fs), b = size_t(2.0f * fs / pg::kQuantum + 0.5f) * pg::kQuantum,
               c = size_t(4.0f * fs / pg::kQuantum + 0.5f) * pg::kQuantum;
  float worst = 0.0f;
  for (size_t i = 0; i < n; ++i) {
    worst = std::fmax(worst, std::fabs(x[i] - x[b + i]));
    worst = std::fmax(worst, std::fabs(x[i] - x[c + i]));
  }
  T.range(worst, 0.0f, 0.0f, "max difference between hits");
  T.check(x[0] == 0.0f || std::fabs(x[0]) < 0.08f, "starts near zero (phase reset)");
}

void testRetriggerAndChoke(float fs) {
  std::printf("\n== retrigger and choke continuity at %.0f Hz ==\n", double(fs));
  KickVoice kick; SnareVoice snare; ClapVoice clap; HatVoice ch(false), oh(true);
  PercVoice tom, cow, zap;
  Params kickSmooth = dt::defaults(kick);
  kickSmooth[KickVoice::kClick] = 0;
  Params kickLong = kickSmooth;
  kickLong[KickVoice::kDecay] = 100;
  kickLong[KickVoice::kTone] = 127;
  struct Case { const char* name; pg::Voice* v; Params p; float absLimit; };
  std::vector<Case> cases = {
      {"kick (no click)", &kick, kickSmooth, 0.06f},
      {"kick long, tone open", &kick, kickLong, 0.06f},
      {"kick default", &kick, dt::defaults(kick), 0.0f},
      {"snare", &snare, dt::defaults(snare), 0.0f},
      {"clap", &clap, dt::defaults(clap), 0.0f},
      {"closed hat", &ch, dt::defaults(ch), 0.0f},
      {"open hat", &oh, dt::defaults(oh), 0.0f},
      {"perc tom", &tom, percModel(tom, PercVoice::kTom), 0.0f},
      {"perc cowbell", &cow, percModel(cow, PercVoice::kCowbell), 0.0f},
      {"perc zap", &zap, percModel(zap, PercVoice::kZap), 0.0f},
  };
  const float offsets[6] = {0.0113f, 0.0237f, 0.0401f, 0.0672f, 0.1130f, 0.1717f};
  for (Case& c : cases) {
    std::printf("%s\n", c.name);
    c.v->init(fs);
    // Reference: the largest step the sound makes on its own, over several
    // well separated hits (the noisy voices differ from hit to hit).
    std::vector<dt::Event> sep;
    // 40 of them, so the reference has seen more hits than the 18 retriggered ones below.
    for (int h = 0; h < 40; ++h) sep.push_back(dt::hit(3.0f * float(h) + 0.0037f * float(h), fs));
    std::vector<float> single = dt::run(*c.v, c.p, sep, 121.0f, fs);
    const float ref = dt::maxJump(single);
    // Limit: what the sound does on its own, plus a little. For the smooth
    // kick settings an absolute limit applies as well.
    float limit = ref * 1.25f + 0.02f;
    const float absLimit = c.absLimit * 44100.0f / fs;  // slopes scale with the sample period
    if (absLimit > 0.0f && absLimit < limit) limit = absLimit;
    float worstRe = 0.0f, worstChoke = 0.0f, worstChokeTime = 0.0f;
    for (float off : offsets) {
      c.v->init(fs);
      std::vector<float> re = dt::run(*c.v, c.p, {dt::hit(0.0f, fs), dt::hit(off, fs), dt::hit(off * 2.0f, fs)}, 1.0f, fs);
      worstRe = std::fmax(worstRe, dt::maxJump(re));
      c.v->init(fs);
      std::vector<char> act;
      std::vector<float> ck = dt::run(*c.v, c.p, {dt::hit(0.0f, fs), dt::chokeAt(off, fs)}, 0.6f, fs, &act);
      const size_t chokeQ = size_t(off * fs / pg::kQuantum + 0.5f), chokeSample = chokeQ * pg::kQuantum;
      if (chokeQ == 0 || !act[chokeQ - 1]) continue;  // had already decayed: nothing to choke
      worstChoke = std::fmax(worstChoke, dt::maxJump(ck, chokeSample));
      size_t lastActive = 0;
      for (size_t q = 0; q + 1 < act.size(); ++q) if (act[q]) lastActive = q + 1;
      const float t = float((lastActive + 1) * pg::kQuantum - chokeSample) / fs;
      worstChokeTime = std::fmax(worstChokeTime, t);
      T.check(dt::peak(ck, (lastActive + 1) * pg::kQuantum) == 0.0f, "silent after choke");
    }
    std::printf("  single-hit max sample step %.4f -> limit %.4f\n", double(ref), double(limit));
    T.range(worstRe, 0.0f, limit, "retrigger: max sample-to-sample step");
    T.range(worstChoke, 0.0f, limit, "choke: max step after choke()");
    T.range(worstChokeTime * 1000.0f, 1.5f, 3.0f, "choke: ms until inactive (2 ms fade)");
  }
  // Choke followed immediately by a new hit (closed hat choking open hat on the same step).
  oh.init(fs);
  std::vector<float> x = dt::run(oh, dt::defaults(oh), {dt::hit(0.0f, fs), dt::chokeAt(0.1f, fs), dt::hit(0.1f, fs)}, 0.5f, fs);
  T.check(dt::rms(x, size_t(0.12f * fs), size_t(0.2f * fs)) > 0.005f, "hit right after choke still sounds");
}

void testVelocityAccent(float fs) {
  std::printf("\n== velocity and accent ==\n");
  KickVoice kick; SnareVoice snare; ClapVoice clap; HatVoice ch(false), oh(true); PercVoice perc;
  struct Case { const char* name; pg::Voice* v; float cutHz; };
  std::vector<Case> cases = {{"kick", &kick, 1500.0f}, {"snare", &snare, 3000.0f}, {"clap", &clap, 3000.0f},
                             {"closed hat", &ch, 9000.0f}, {"open hat", &oh, 9000.0f}, {"perc rim", &perc, 2000.0f}};
  for (Case& c : cases) {
    std::printf("%s\n", c.name);
    const Params p = dt::defaults(*c.v);
    // RMS over the first 60 ms averaged over several hits (noise voices vary hit to hit).
    auto level = [&](uint8_t vel, bool accent, double* hfFrac) {
      double acc = 0.0, hf = 0.0;
      const int hits = 6;
      for (int h = 0; h < hits; ++h) {
        c.v->init(fs);
        // different lead-in so free-running oscillators/noise differ per hit
        std::vector<float> x = dt::run(*c.v, p, {dt::hit(0.0f, fs, 1), dt::hit(0.5f + 0.013f * float(h), fs, vel, 60, accent)}, 0.9f, fs);
        const size_t s = size_t((0.5f + 0.013f * float(h)) * fs / pg::kQuantum + 0.5f) * pg::kQuantum;
        acc += double(dt::rms(x, s, s + size_t(0.06f * fs)));
        dt::Spectrum sp = dt::spectrum(x, double(fs), s, size_t(0.06f * fs));
        hf += dt::bandFraction(sp, double(c.cutHz), double(fs));
      }
      if (hfFrac) *hfFrac = hf / hits;
      return float(acc / hits);
    };
    double hfN = 0.0, hfA = 0.0;
    const float l127 = level(127, false, nullptr), l100 = level(100, false, &hfN), l64 = level(64, false, nullptr),
                l20 = level(20, false, nullptr), lAcc = level(100, true, &hfA);
    T.range(dt::dB(l100 / l127), -5.0f, -2.5f, "vel 100 vs 127 (dB)");
    T.range(dt::dB(l64 / l127), -13.0f, -7.0f, "vel 64 vs 127 (dB)");
    T.range(dt::dB(l20 / l127), -32.0f, -20.0f, "vel 20 vs 127 (dB)");
    T.range(dt::dB(lAcc / l100), 1.0f, 5.0f, "accent level gain (dB)");
    std::printf("  HF energy share above %.0f Hz: plain %.5f, accent %.5f\n", double(c.cutHz), hfN, hfA);
    T.check(hfA > hfN * 1.1, "accent is brighter");
  }
}

void testSpectra(float fs) {
  std::printf("\n== spectral sanity at %.0f Hz ==\n", double(fs));
  const double dfs = double(fs);
  auto spec = [&](pg::Voice& v, const Params& p, float seconds) {
    v.init(fs);
    std::vector<float> x = dt::run(v, p, {dt::hit(0.0f, fs)}, seconds, fs);
    return dt::spectrum(x, dfs);
  };
  {
    KickVoice v;
    dt::Spectrum s = spec(v, dt::defaults(v), 0.6f);
    T.range(float(dt::bandFraction(s, 0, 200)), 0.80f, 1.0f, "kick: energy below 200 Hz");
    T.range(float(dt::bandFraction(s, 80, 1000)), 0.05f, 0.6f, "kick: harmonics 80 Hz..1 kHz (reads on small phones)");
    // The click: there must be some energy up high during the first 10 ms.
    v.init(fs);
    std::vector<float> x = dt::run(v, dt::defaults(v), {dt::hit(0.0f, fs)}, 0.05f, fs);
    dt::Spectrum a = dt::spectrum(x, dfs, 0, size_t(0.01f * fs));
    Params noClick = dt::defaults(v);
    noClick[KickVoice::kClick] = 0;
    v.init(fs);
    std::vector<float> y = dt::run(v, noClick, {dt::hit(0.0f, fs)}, 0.05f, fs);
    dt::Spectrum b = dt::spectrum(y, dfs, 0, size_t(0.01f * fs));
    const double withClick = dt::bandFraction(a, 2000, dfs), without = dt::bandFraction(b, 2000, dfs);
    std::printf("  kick first 10 ms, share above 2 kHz: click %.5f, no click %.5f\n", withClick, without);
    T.check(withClick > 3.0 * without, "kick: click layer adds highs to the attack");
  }
  {
    SnareVoice v;
    dt::Spectrum s = spec(v, dt::defaults(v), 0.5f);
    T.range(float(dt::bandFraction(s, 120, 450)), 0.25f, 0.75f, "snare: body 120-450 Hz");
    T.range(float(dt::bandFraction(s, 1000, dfs)), 0.20f, 0.70f, "snare: noise above 1 kHz");
    T.range(float(dt::bandFraction(s, 0, 100)), 0.0f, 0.03f, "snare: little below 100 Hz");
  }
  {
    ClapVoice v;
    dt::Spectrum s = spec(v, dt::defaults(v), 0.6f);
    T.range(float(dt::bandFraction(s, 500, 5000)), 0.75f, 1.0f, "clap: energy 500 Hz..5 kHz (body)");
    T.range(float(dt::bandFraction(s, 8000, dfs)), 0.0f, 0.12f, "clap: not fizzy (above 8 kHz)");
    T.range(float(dt::centroid(s)), 1200.0f, 3500.0f, "clap: centroid (Hz)");
  }
  for (int open = 0; open < 2; ++open) {
    HatVoice v(open != 0);
    dt::Spectrum s = spec(v, dt::defaults(v), open ? 1.5f : 0.3f);
    const char* n = open ? "open hat" : "closed hat";
    std::printf("%s\n", n);
    T.range(float(dt::bandFraction(s, 4000, dfs)), 0.90f, 1.0f, "hat: energy above 4 kHz");
    T.range(float(dt::bandFraction(s, 0, 2000)), 0.0f, 0.01f, "hat: energy below 2 kHz");
    T.range(float(dt::centroid(s)), 7000.0f, 12500.0f, "hat: centroid (Hz)");
  }
  {
    // Metallic, not white noise: with the long open hat, the spectrum around
    // the passband should be made of lines. Compare spectral flatness
    // (geometric / arithmetic mean) of all-metal against all-noise.
    HatVoice v(true);
    auto flatness = [&](int metal) {
      Params p = dt::defaults(v);
      p[HatVoice::kMetal] = uint8_t(metal);
      p[HatVoice::kDecay] = 110;
      v.init(fs);
      std::vector<float> x = dt::run(v, p, {dt::hit(0.0f, fs)}, 0.5f, fs);
      // fine grid 5..12 kHz over 0.37 s
      double lg = 0.0, ar = 0.0;
      int n = 0;
      for (double f = 5000.0; f < 12000.0; f += 4.0) {
        const double pw = dt::goertzelPower(x, 0, size_t(0.37f * fs), f, dfs) + 1e-30;
        lg += std::log(pw); ar += pw; ++n;
      }
      return float(std::exp(lg / n) / (ar / n));
    };
    const float fNoise = flatness(0), fDef = flatness(dt::defaults(v)[HatVoice::kMetal]), fMetal = flatness(127);
    std::printf("  spectral flatness 5-12 kHz: noise %.3f, default %.3f, metal %.3f\n", double(fNoise), double(fDef), double(fMetal));
    T.check(fMetal < 0.5f * fNoise, "hat: MTL=127 is line-like (metallic), MTL=0 is noise");
    T.check(fDef < 0.8f * fNoise, "hat: default is clearly metallic");
  }
  {
    HatVoice c(false), o(true);
    c.init(fs); o.init(fs);
    std::vector<float> xc = dt::run(c, dt::defaults(c), {dt::hit(0.0f, fs)}, 0.5f, fs);
    std::vector<float> xo = dt::run(o, dt::defaults(o), {dt::hit(0.0f, fs)}, 2.5f, fs);
    const float tc = timeAbove(xc, fs, -40.0f), to = timeAbove(xo, fs, -40.0f);
    T.range(tc, 0.02f, 0.12f, "closed hat: seconds to -40 dB (tight)");
    T.range(to, 0.25f, 1.2f, "open hat: seconds to -40 dB (rings)");
  }
  {
    PercVoice v;
    dt::Spectrum s = spec(v, percModel(v, PercVoice::kTom), 0.8f);
    T.range(float(dt::bandFraction(s, 60, 400)), 0.85f, 1.0f, "tom: energy 60-400 Hz");
    s = spec(v, percModel(v, PercVoice::kRim), 0.2f);
    T.range(float(dt::bandFraction(s, 300, 5000)), 0.9f, 1.0f, "rim: energy 300 Hz..5 kHz");
    s = spec(v, percModel(v, PercVoice::kCowbell), 0.6f);
    T.range(float(dt::bandFraction(s, 450, 950)), 0.2f, 0.9f, "cowbell: the two fundamentals 450-950 Hz");
    T.range(float(dt::bandFraction(s, 0, 300)), 0.0f, 0.02f, "cowbell: nothing below 300 Hz");
    s = spec(v, percModel(v, PercVoice::kClave), 0.2f);
    T.range(float(dt::bandFraction(s, 2000, 3000)), 0.85f, 1.0f, "clave: energy 2-3 kHz");
    s = spec(v, percModel(v, PercVoice::kShaker), 0.4f);
    T.range(float(dt::bandFraction(s, 3000, dfs)), 0.85f, 1.0f, "shaker: energy above 3 kHz");
    s = spec(v, percModel(v, PercVoice::kZap), 0.5f);
    T.range(float(dt::bandFraction(s, 200, 4000)), 0.9f, 1.0f, "zap: energy 200 Hz..4 kHz");
  }
}

void testKitBalance(float fs) {
  std::printf("\n== kit balance (full velocity, all defaults) ==\n");
  KickVoice kick; SnareVoice snare; ClapVoice clap; HatVoice ch(false), oh(true); PercVoice perc;
  struct Case { const char* name; pg::Voice* v; };
  std::vector<Case> cases = {{"kick", &kick}, {"snare", &snare}, {"clap", &clap}, {"closed hat", &ch}, {"open hat", &oh}, {"perc (rim)", &perc}};
  float kickPk = 0.0f;
  for (Case& c : cases) {
    // Average peak over a handful of hits for the noisy voices.
    float pkSum = 0.0f, pkMax = 0.0f, rmsSum = 0.0f;
    const int hits = 8;
    for (int h = 0; h < hits; ++h) {
      c.v->init(fs);
      const float t0 = 0.3f + 0.0171f * float(h);
      std::vector<float> x = dt::run(*c.v, dt::defaults(*c.v), {dt::hit(0.0f, fs, 1), dt::hit(t0, fs, 127)}, t0 + 0.5f, fs);
      const size_t s = size_t(t0 * fs / pg::kQuantum + 0.5f) * pg::kQuantum;
      const float pk = dt::peak(x, s);
      pkSum += pk; pkMax = std::fmax(pkMax, pk);
      rmsSum += dt::rms(x, s, s + size_t(0.05f * fs));
    }
    const float pk = pkSum / hits;
    if (c.v == &kick) kickPk = pk;
    std::printf("  %-12s mean peak %.3f (%6.1f dBFS)  max peak %.3f  rms(50 ms) %6.1f dBFS  vs kick %+5.1f dB\n", c.name,
                double(pk), double(dt::dB(pk)), double(pkMax), double(dt::dB(rmsSum / hits)), double(dt::dB(pk / kickPk)));
    if (c.v == &kick) T.range(pk, 0.47f, 0.53f, "kick peak ~ -6 dBFS");
    else T.range(dt::dB(pk / kickPk), -12.0f, -1.5f, "peak relative to kick (dB)");
  }
}
}  // namespace

int main() {
  for (float fs : {44100.0f, 32000.0f}) {
    testDefaults(fs);
    testKickTuning(fs);
    testRetriggerAndChoke(fs);
    testSpectra(fs);
  }
  testDeterminism(44100.0f);
  testVelocityAccent(44100.0f);
  testKitBalance(44100.0f);
  T.check(dt::monoMismatches() == 0, "every voice writes identical L and R");
  return T.done("test_drums_basic");
}
