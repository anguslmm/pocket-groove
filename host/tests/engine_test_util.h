// Shared helpers for host/tests/test_engine_*.cpp (not a test itself).
// Stub voices that log every call with the sample time it arrived at, and a
// small rig that owns the Project and drives Engine::process one quantum at a
// time so every event can be checked against the sample clock.
#pragma once
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <vector>

#include "common/wav.h"
#include "pg/engine.h"
#include "pg/engine/engine_hooks.h"

namespace et {

inline long g_now = 0;        // sample time of the quantum currently being processed
inline int g_fail = 0;
inline int g_checks = 0;

inline void fail(const char* file, int line, const char* fmt, ...) {
  ++g_fail;
  if (g_fail > 40) return;
  std::fprintf(stderr, "FAIL %s:%d: ", file, line);
  va_list ap;
  va_start(ap, fmt);
  std::vfprintf(stderr, fmt, ap);
  va_end(ap);
  std::fprintf(stderr, "\n");
}
#define CHECK(cond)                                                     \
  do {                                                                  \
    ++et::g_checks;                                                     \
    if (!(cond)) et::fail(__FILE__, __LINE__, "%s", #cond);             \
  } while (0)
#define CHECKF(cond, ...)                                               \
  do {                                                                  \
    ++et::g_checks;                                                     \
    if (!(cond)) et::fail(__FILE__, __LINE__, __VA_ARGS__);             \
  } while (0)

inline int finish(const char* name) {
  std::printf("%s: %d checks, %d failed\n", name, g_checks, g_fail);
  return g_fail ? 1 : 0;
}

struct Call {
  enum Type { On, Off, Choke } type;
  long t;
  pg::NoteEvent ev;      // On
  uint8_t note;          // Off
  uint8_t p[8];          // first 8 sound params as last given to setParams before this call
};

class StubVoice : public pg::Voice {
 public:
  enum Mode {
    Silent,     // never active (pure call recorder)
    Blip,       // decaying sine blip per note-on
    Dc,         // constant +amp, always active (gain probe)
    Tone,       // constant sine, always active
    Gate,       // sine while gated (note-on .. note-off)
    Impulse,    // one-sample impulse per note-on
    Noise,      // white noise, always active
    Nan,        // emits NaN (robustness)
  };
  Mode mode = Silent;
  float amp = 0.5f;
  float freq = 220.0f;
  float decaySec = 0.05f;
  bool log = true;

  std::vector<Call> calls;
  uint8_t params[pg::kSoundParams] = {};
  long setParamsCalls = 0;
  long lastSetParamsT = -1;
  int setParamsThisQuantumMax = 0, setParamsThisQuantum = 0;
  float tempo = 0;
  float sr = 44100.0f;

  void init(float sampleRate) override { sr = sampleRate; }
  const pg::ParamInfo* paramInfo(int index) const override {
    static const pg::ParamInfo info[8] = {
        {"P0", "Param 0", 0, 127, 10, pg::ParamFormat::Number, nullptr},
        {"P1", "Param 1", 0, 127, 20, pg::ParamFormat::Number, nullptr},
        {"P2", "Param 2", 0, 127, 30, pg::ParamFormat::Number, nullptr},
        {"P3", "Param 3", 0, 127, 40, pg::ParamFormat::Number, nullptr},
        {"P4", "Param 4", 5, 50, 25, pg::ParamFormat::Number, nullptr},
        {"P5", "Param 5", 0, 127, 60, pg::ParamFormat::Number, nullptr},
        {"P6", "Param 6", 0, 127, 70, pg::ParamFormat::Number, nullptr},
        {"P7", "Param 7", 0, 127, 80, pg::ParamFormat::Number, nullptr},
    };
    return (index >= 0 && index < 8) ? &info[index] : nullptr;
  }
  void setParams(const uint8_t* sp) override {
    std::memcpy(params, sp, pg::kSoundParams);
    ++setParamsCalls;
    if (lastSetParamsT == g_now) {
      ++setParamsThisQuantum;
    } else {
      setParamsThisQuantum = 1;
      lastSetParamsT = g_now;
    }
    if (setParamsThisQuantum > setParamsThisQuantumMax) setParamsThisQuantumMax = setParamsThisQuantum;
  }
  void setTempo(float bpm) override { tempo = bpm; }
  void noteOn(const pg::NoteEvent& e) override {
    if (log) {
      Call c{};
      c.type = Call::On; c.t = g_now; c.ev = e;
      std::memcpy(c.p, params, 8);
      calls.push_back(c);
    }
    gated = true;
    env = float(e.vel) / 127.0f;
    if (mode == Blip) { phase = 0.0f; noteFreq = 440.0f * std::pow(2.0f, (float(e.note) - 69.0f) / 12.0f); }
    if (mode == Impulse) pendingImpulse = true;
  }
  void noteOff(uint8_t note) override {
    if (log) {
      Call c{};
      c.type = Call::Off; c.t = g_now; c.note = note;
      std::memcpy(c.p, params, 8);
      calls.push_back(c);
    }
    gated = false;
  }
  void choke() override {
    if (log) {
      Call c{};
      c.type = Call::Choke; c.t = g_now;
      calls.push_back(c);
    }
    gated = false;
    env = 0.0f;
    pendingImpulse = false;
  }
  void render(float* l, float* r, int frames) override {
    ++renderCalls;
    for (int i = 0; i < frames; ++i) {
      float x = 0.0f;
      switch (mode) {
        case Silent: break;
        case Blip:
          x = amp * env * std::sin(phase);
          phase += 6.2831853f * noteFreq / sr;
          if (phase > 6.2831853f) phase -= 6.2831853f;
          env *= std::exp(-1.0f / (decaySec * sr));
          break;
        case Dc: x = amp; break;
        case Tone:
        case Gate:
          x = amp * std::sin(phase);
          phase += 6.2831853f * freq / sr;
          if (phase > 6.2831853f) phase -= 6.2831853f;
          if (mode == Gate && !gated) x = 0.0f;
          break;
        case Impulse:
          if (pendingImpulse) { x = amp; pendingImpulse = false; }
          break;
        case Noise:
          seed = seed * 1664525u + 1013904223u;
          x = amp * (float(int32_t(seed)) / 2147483648.0f);
          break;
        case Nan: x = std::nanf(""); break;
      }
      l[i] = x;
      r[i] = x;
    }
  }
  bool active() const override {
    switch (mode) {
      case Silent: return false;
      case Blip: return env > 1.0e-4f;
      case Impulse: return pendingImpulse;
      case Gate: return gated;
      default: return true;
    }
  }

  // ---- helpers over the call log ----
  std::vector<Call> ons() const { return filter(Call::On); }
  std::vector<Call> offs() const { return filter(Call::Off); }
  std::vector<Call> chokes() const { return filter(Call::Choke); }
  std::vector<Call> filter(Call::Type ty) const {
    std::vector<Call> v;
    for (const Call& c : calls) if (c.type == ty) v.push_back(c);
    return v;
  }
  void clearLog() { calls.clear(); }

  bool gated = false, pendingImpulse = false;
  float env = 0.0f, phase = 0.0f, noteFreq = 220.0f;
  uint32_t seed = 12345u;
  long renderCalls = 0;
};

inline pg::Engine& engine() {
  static pg::Engine e;
  return e;
}

inline pg::Command cmd(pg::CmdType type, int a = 0, int b = 0, int c = 0) {
  pg::Command k{};
  k.type = type;
  k.a = uint8_t(a); k.b = uint8_t(b); k.c = uint8_t(c);
  k.step = pg::kDefaultStep;
  k.step.flags = 0;
  return k;
}

inline pg::Step mkStep(int note = 60, int flags = pg::kStepOn, int vel = 100, int gate = 2, int prob = 100) {
  pg::Step s = pg::kDefaultStep;
  s.note = uint8_t(note); s.flags = uint8_t(flags); s.vel = uint8_t(vel); s.gate = uint8_t(gate); s.prob = uint8_t(prob);
  return s;
}

struct Rig {
  pg::Engine& eng = engine();
  pg::Project proj;
  StubVoice v[pg::kNumTracks];
  float sr = 44100.0f;
  long time = 0;
  bool capture = false;
  std::vector<int16_t> out;          // interleaved stereo, when capture is on

  // Fresh engine + default project + 8 stub voices (all Silent unless changed after).
  void init(float sampleRate = 44100.0f, int bpm = 124) {
    sr = sampleRate;
    time = 0;
    g_now = 0;
    out.clear();
    for (int t = 0; t < pg::kNumTracks; ++t) {
      v[t] = StubVoice();
      v[t].init(sr);
      eng.setVoice(t, &v[t]);
    }
    eng.initProject(proj);
    proj.globals[pg::kGTempo] = uint8_t(bpm);
    eng.init(sr, &proj);
  }
  // Neutral mixer: unity level, centre, no sends, no duck; ducker off; FX-free master.
  void neutralMix() {
    for (int t = 0; t < pg::kNumTracks; ++t) {
      proj.tracks[t].params[pg::kMixLevel] = 100;
      proj.tracks[t].params[pg::kMixPan] = 64;
      proj.tracks[t].params[pg::kMixDelaySend] = 0;
      proj.tracks[t].params[pg::kMixReverbSend] = 0;
      proj.tracks[t].params[pg::kMixDuck] = 0;
    }
    proj.globals[pg::kGDuckSource] = 0;
    proj.globals[pg::kGMasterVolume] = 100;
    proj.globals[pg::kGMasterDrive] = 0;
    proj.globals[pg::kGMasterFilter] = 64;
  }
  pg::Step& step(int pattern, int track, int s) { return proj.patterns[pattern].tracks[track].steps[s]; }
  void post(const pg::Command& c) {
    if (!eng.post(c)) fail(__FILE__, __LINE__, "post() failed: queue full");
  }
  void post(pg::CmdType type, int a = 0, int b = 0, int c = 0) { post(cmd(type, a, b, c)); }
  void run(long quanta) {
    int16_t buf[2 * pg::kQuantum];
    for (long q = 0; q < quanta; ++q) {
      g_now = time;
      eng.process(buf, pg::kQuantum);
      if (capture) out.insert(out.end(), buf, buf + 2 * pg::kQuantum);
      time += pg::kQuantum;
    }
    g_now = time;
  }
  void runSamples(long samples) { run((samples + pg::kQuantum - 1) / pg::kQuantum); }
  void runSeconds(double sec) { runSamples(long(sec * double(sr))); }
  double stepLen() const { return double(sr) * 15.0 / double(proj.globals[pg::kGTempo]); }
  float sample(long frame, int ch) const { return float(out[size_t(frame) * 2 + size_t(ch)]) / 32767.0f; }
  long frames() const { return long(out.size() / 2); }
  bool writeWav(const char* name) const {
    mkdir("build", 0755);
    mkdir("build/renders", 0755);
    mkdir("build/renders/engine", 0755);
    char path[256];
    std::snprintf(path, sizeof(path), "build/renders/engine/%s", name);
    return wav::write(path, out, int(sr), 2);
  }
};

inline double rmsOf(const std::vector<int16_t>& o, long f0, long f1, int ch) {
  double acc = 0;
  for (long f = f0; f < f1; ++f) { double x = double(o[size_t(f) * 2 + size_t(ch)]) / 32767.0; acc += x * x; }
  return f1 > f0 ? std::sqrt(acc / double(f1 - f0)) : 0.0;
}
inline double peakOf(const std::vector<int16_t>& o, long f0, long f1, int ch) {
  double m = 0;
  for (long f = f0; f < f1; ++f) { double x = std::fabs(double(o[size_t(f) * 2 + size_t(ch)]) / 32767.0); if (x > m) m = x; }
  return m;
}
inline double dB(double x) { return 20.0 * std::log10(x > 1e-12 ? x : 1e-12); }

}  // namespace et
