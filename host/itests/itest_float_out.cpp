// The engine's two output formats (engine.h OUTPUT FORMATS): process() (int16) and
// processFloat() (float, +-1.0) render the same mix through the same master chain.
// Every case is rendered twice from a fresh init (the engine is deterministic), once
// per format, and compared sample by sample:
//   * |float * 32767 - int16| <= 0.5 (+ float rounding), and floatToInt16(float) is the
//     int16 bit for bit;
//   * the float never leaves [-1, 1];
//   * the output tap (resampling) sees the same int16 in both;
//   * the hot cases really are in the limiter (samples near full scale).
// Cases: the three demo songs (stereo, 48 kHz), a hot pattern (every level, drive and
// the master volume at the top, drive and volume moving: the gain-ramp paths) in stereo
// and in mono, and a reverb tail after Stop, where it also MEASURES what the int16 step
// costs (printed, not enforced): the int16 path's error against the float path while
// the tail decays through -60 .. -100 dBFS, and what a 24-bit converter fed each one
// would get (libDaisy's f2s24: clamp to 0.999985, x 2^23, truncate).
//
//   build/host/itest_float_out             the checks and the measurements
//   build/host/itest_float_out --dump DIR  also writes the tail's two renders as raw
//                                          float32 stereo (DIR/tail_f32.raw, tail_i16.raw)
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "common/grooves.h"
#include "itests/itest_util.h"
#include "pg/demo/demo.h"

using namespace pg;

namespace {

Instrument g_inst;

struct HashTap final : OutputTap {
  uint64_t hash = 1469598103934665603ull;
  long frames = 0;
  void quantum(const int16_t* stereo, int n, uint8_t) override {
    const uint8_t* b = reinterpret_cast<const uint8_t*>(stereo);
    for (size_t i = 0; i < size_t(n) * 4; ++i) hash = (hash ^ b[i]) * 1099511628211ull;
    frames += n;
  }
};

struct Case {
  const char* name;
  int demo;        // >= 0: that demo's song; -1: the hot pattern; -2: the reverb tail
  bool mono;
  float sr;
  int block;       // frames per process() call
};

const Case kCases[] = {
    {"acidjam song stereo 48k", 0, false, 48000.0f, 96},
    {"basshaus song stereo 48k", 1, false, 48000.0f, 96},
    {"warehous song stereo 48k", 2, false, 48000.0f, 256},
    {"hot pattern stereo 48k", -1, false, 48000.0f, 96},
    {"hot pattern mono 44.1k", -1, true, 44100.0f, 256},
    {"reverb tail stereo 48k", -2, false, 48000.0f, 96},
};

void post(CmdType t, int a = 0, int c = 0) {
  Command k{};
  k.type = t;
  k.a = uint8_t(a);
  k.c = uint8_t(c);
  g_inst.engine().post(k);
}

// Sets the case up from a fresh init; returns the frames to render and, through
// *events, when to post what (frame -> command), kept identical for both renders.
struct Event { long frame; CmdType type; int a, c; };

long setup(const Case& c, std::vector<Event>* events) {
  g_inst.init(c.sr, c.mono);
  Engine& eng = g_inst.engine();
  Project& p = g_inst.project();
  events->clear();
  if (c.demo >= 0) {
    demo::build(eng, p, c.demo);
    p.songMode = 1;
    eng.sanitizeProject(p);
    long steps = 0;
    for (int s = 0; s < kSongSlots && p.song[s].repeats; ++s)
      steps += long(p.patterns[p.song[s].pattern].length) * p.song[s].repeats;
    const double stepSamples = double(c.sr) * 15.0 / double(p.globals[kGTempo]);
    const long play = long(std::ceil(double(steps) * stepSamples / kQuantum)) * kQuantum;
    post(CmdType::Play);
    events->push_back({play, CmdType::Stop, 0, 0});
    return play + long(c.sr);   // and a second of tail
  }
  if (c.demo == -1) {
    grooves::programBusy(p);
    p.globals[kGMasterVolume] = 127;
    p.globals[kGMasterDrive] = 127;
    for (int t = 0; t < kNumTracks; ++t) p.tracks[t].params[kMixLevel] = 127;
    eng.sanitizeProject(p);
    post(CmdType::Play);
    const long s = long(c.sr);
    // Drive and volume moving (the master's ramp paths), and back up.
    events->push_back({s, CmdType::SetGlobal, kGMasterDrive, 0});
    events->push_back({s + s / 2, CmdType::SetGlobal, kGMasterVolume, 90});
    events->push_back({2 * s, CmdType::SetGlobal, kGMasterDrive, 100});
    events->push_back({2 * s + s / 3, CmdType::SetGlobal, kGMasterVolume, 127});
    events->push_back({3 * s, CmdType::SetGlobal, kGMasterFilter, 100});
    return 4 * s;
  }
  // The reverb tail: the first demo's groove with a long reverb on everything, then
  // Stop and 8 s of decay.
  demo::build(eng, p, 0);
  p.songMode = 0;
  p.globals[kGReverbSize] = 120;
  for (int t = 0; t < kNumTracks; ++t) p.tracks[t].params[kMixReverbSend] = 100;
  eng.sanitizeProject(p);
  post(CmdType::Play);
  const long play = long(2.0f * c.sr) / kQuantum * kQuantum;
  events->push_back({play, CmdType::Stop, 0, 0});
  return play + long(8.0f * c.sr);
}

// One render of the case in one format. Exactly one of i16 / f32 is filled.
long render(const Case& c, bool asFloat, std::vector<int16_t>* i16, std::vector<float>* f32, HashTap* tap,
            long* stopFrame) {
  std::vector<Event> ev;
  const long total = setup(c, &ev) / c.block * c.block;
  g_inst.engine().setOutputTap(tap);
  size_t next = 0;
  *stopFrame = -1;
  for (const Event& e : ev)
    if (e.type == CmdType::Stop) *stopFrame = e.frame / c.block * c.block;
  if (asFloat) f32->assign(size_t(total) * 2, 0.0f);
  else i16->assign(size_t(total) * 2, 0);
  for (long f = 0; f < total; f += c.block) {
    while (next < ev.size() && ev[next].frame <= f) {
      post(ev[next].type, ev[next].a, ev[next].c);
      ++next;
    }
    if (asFloat) g_inst.processFloat(f32->data() + 2 * f, c.block);
    else g_inst.process(i16->data() + 2 * f, c.block);
  }
  g_inst.engine().setOutputTap(nullptr);
  return total;
}

double db(double x) { return x > 0.0 ? 20.0 * std::log10(x) : -999.0; }

// libDaisy's float -> 24-bit conversion (daisy_core.h f2s24), as a float again.
double codec24(float x) {
  const float m = 0.999985f;
  x = x <= -m ? -m : x;
  x = x >= m ? m : x;
  return double(int32_t(x * 8388608.0f)) / 8388608.0;
}

// The tail's measurements: windows of 50 ms after Stop, grouped by the float
// render's level; in each group the int16 path's error (int16 / 32767 - float) and
// the 24-bit converter's error for each path's input.
void measureTail(const std::vector<int16_t>& i16, const std::vector<float>& f32, long stop, float sr) {
  const long win = long(0.05f * sr);
  struct Band { double lo, hi, sig = 0, err16 = 0, codecF = 0, codec16 = 0; long n = 0, zero16 = 0; };
  Band bands[] = {{-60, -50}, {-70, -60}, {-80, -70}, {-90, -80}, {-100, -90}, {-110, -100}, {-130, -110}};
  long lastNonZero16 = stop, lastNonZeroF = stop;
  const long total = long(f32.size() / 2);
  for (long w = stop; w + win <= total; w += win) {
    double s = 0, e = 0, cf = 0, c16 = 0;
    long z = 0;
    for (long i = 2 * w; i < 2 * (w + win); ++i) {
      const double f = double(f32[size_t(i)]), q = double(i16[size_t(i)]) / 32767.0;
      s += f * f;
      e += (q - f) * (q - f);
      const double cq = codec24(f32[size_t(i)]);
      cf += (cq - f) * (cq - f);
      if (f32[size_t(i)] != 0.0f) lastNonZeroF = i / 2;
      // The int16 path on the Daisy before this change: int16 / 32768 into f2s24.
      const double old = codec24(float(i16[size_t(i)]) / 32768.0f);
      c16 += (old - f) * (old - f);
      if (i16[size_t(i)] == 0) ++z;
      else lastNonZero16 = i / 2;
    }
    const double lv = db(std::sqrt(s / double(2 * win)));
    for (Band& b : bands)
      if (lv >= b.lo && lv < b.hi) {
        b.sig += s; b.err16 += e; b.codecF += cf; b.codec16 += c16; b.n += 2 * win; b.zero16 += z;
      }
  }
  std::printf("  reverb tail after Stop (50 ms windows grouped by the float render's RMS level):\n");
  std::printf("    level band     tail RMS   int16 err RMS   int16 SNR   int16 zeros   24-bit codec err: float in / int16 in\n");
  for (const Band& b : bands) {
    if (!b.n) continue;
    const double n = double(b.n);
    std::printf("    %4.0f..%4.0f dB  %7.1f dB  %9.1f dBFS  %7.1f dB  %9.1f %%   %7.1f dBFS / %7.1f dBFS\n", b.lo, b.hi,
                db(std::sqrt(b.sig / n)), db(std::sqrt(b.err16 / n)), db(std::sqrt(b.sig / n)) - db(std::sqrt(b.err16 / n)),
                100.0 * double(b.zero16) / n, db(std::sqrt(b.codecF / n)), db(std::sqrt(b.codec16 / n)));
  }
  std::printf("    the last non-zero sample after Stop: int16 %.2f s, float %.2f s (the engine's own denormal guards end it)\n",
              double(lastNonZero16 - stop) / double(sr), double(lastNonZeroF - stop) / double(sr));
}

}  // namespace

int main(int argc, char** argv) {
  std::string dump;
  for (int i = 1; i + 1 < argc; ++i)
    if (!std::strcmp(argv[i], "--dump")) dump = argv[i + 1];
  // floatToInt16 at the edges: the ceiling, beyond it, NaN-free rounding half up.
  {
    const float in[] = {1.0f, -1.0f, 1.5f, -2.0f, 0.5f / 32767.0f, -0.5f / 32767.0f, 0.0f, 0.4999f / 32767.0f};
    int16_t o[8];
    floatToInt16(in, o, 8);
    const int16_t want[] = {32767, -32767, 32767, -32767, 1, -1, 0, 0};
    for (int i = 0; i < 8; ++i) it::check(o[i] == want[i], "floatToInt16(%g) = %d, want %d", double(in[i]), o[i], want[i]);
  }
  std::vector<int16_t> i16;
  std::vector<float> f32;
  for (const Case& c : kCases) {
    HashTap tapI, tapF;
    long stopI = 0, stopF = 0;
    const long nI = render(c, false, &i16, nullptr, &tapI, &stopI);
    const long nF = render(c, true, nullptr, &f32, &tapF, &stopF);
    it::check(nI == nF && nI > 0, "%s: frame counts %ld / %ld", c.name, nI, nF);
    long beyond = 0, mismatch = 0, hot = 0, nonZero = 0;
    double worst = 0.0, maxAbs = 0.0;
    std::vector<int16_t> back(f32.size());
    floatToInt16(f32.data(), back.data(), int(f32.size()));
    for (size_t i = 0; i < f32.size(); ++i) {
      const float f = f32[i];
      const double fd = double(f);
      if (!(f >= -1.0f && f <= 1.0f)) ++beyond;
      const double d = std::fabs(fd * 32767.0 - double(i16[i]));
      worst = d > worst ? d : worst;
      if (back[i] != i16[i]) ++mismatch;
      if (std::abs(int(i16[i])) >= 32000) ++hot;
      if (i16[i]) ++nonZero;
      maxAbs = std::fabs(fd) > maxAbs ? std::fabs(fd) : maxAbs;
    }
    it::check(beyond == 0, "%s: %ld float samples outside [-1, 1]", c.name, beyond);
    it::check(worst <= 0.5 + 1e-3, "%s: |float * 32767 - int16| reaches %.4f", c.name, worst);
    it::check(mismatch == 0, "%s: floatToInt16(float) differs from int16 in %ld samples", c.name, mismatch);
    it::check(tapI.hash == tapF.hash && tapI.frames == tapF.frames && tapI.frames == nI,
              "%s: the output tap saw different audio (%ld / %ld frames)", c.name, tapI.frames, tapF.frames);
    it::check(nonZero > nI / 4, "%s: hardly any sound (%ld non-zero samples)", c.name, nonZero);
    if (c.demo == -1) it::check(hot > 1000, "%s: only %ld samples near full scale: not in the limiter", c.name, hot);
    std::printf("  %-26s %8ld frames: max |float*32767 - int16| %.4f, peak %.6f, %ld samples >= 32000\n", c.name, nI,
                worst, maxAbs, hot);
    if (c.demo == -2) {
      measureTail(i16, f32, stopI, c.sr);
      if (!dump.empty()) {
        std::vector<float> q(i16.size());
        for (size_t i = 0; i < i16.size(); ++i) q[i] = float(i16[i]) / 32767.0f;
        FILE* a = std::fopen((dump + "/tail_f32.raw").c_str(), "wb");
        FILE* b = std::fopen((dump + "/tail_i16.raw").c_str(), "wb");
        if (a && b) {
          std::fwrite(f32.data(), sizeof(float), f32.size(), a);
          std::fwrite(q.data(), sizeof(float), q.size(), b);
          std::printf("    dumped %s/tail_{f32,i16}.raw (float32 stereo, Stop at frame %ld)\n", dump.c_str(), stopI);
        }
        if (a) std::fclose(a);
        if (b) std::fclose(b);
      }
    }
  }
  return it::finish("itest_float_out");
}
