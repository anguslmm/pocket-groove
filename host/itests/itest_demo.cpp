// System test for the factory demo songs (pg::demo):
//   1. every demo builds a project that Engine::sanitizeProject leaves
//      byte-identical (only legal values), with the right name, tempo, loop
//      mode at boot, a song chain of 1.5 .. 3.5 minutes and the ducker on;
//   2. each one plays through its whole chain in device mode (mono, 44.1 kHz)
//      without engine faults, int16 wrap or a pinned limiter, with integrated
//      loudness (BS.1770, gated) and sample peak inside the bounds the mix was
//      signed off at, so later DSP / default changes can't silently wreck them;
//   3. every used pattern looped on its own stays inside looser bounds, and the
//      song also plays cleanly at 32 kHz.
// Reference values when the songs were made (mono 44.1 kHz, ears.py): about
// -12 .. -13 LUFS integrated, sample peak around -2 dBFS.
#include <cmath>
#include <vector>

#include "itests/itest_util.h"
#include "pg/demo/demo.h"

using namespace pg;

namespace {

Instrument g_inst;
Project g_a, g_b;

// ---- BS.1770 integrated loudness (K-weighting, 400 ms blocks, 75 % overlap,
// absolute gate -70 LUFS, relative gate -10 LU). Both channels are summed, as
// ears.py does for the dual-mono device renders.
struct Biquad {
  double b0, b1, b2, a1, a2, z1 = 0.0, z2 = 0.0;
  double run(double x) {
    const double y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
  }
};

class Loudness {
 public:
  explicit Loudness(double fs) : fs_(fs) {
    const double kPi = 3.14159265358979323846;
    for (int c = 0; c < 2; ++c) {
      double K = std::tan(kPi * 1681.974450955533 / fs);
      const double Q = 0.7071752369554196, Vh = std::pow(10.0, 3.999843853973347 / 20.0);
      const double Vb = std::pow(Vh, 0.4996667741545416);
      double a0 = 1.0 + K / Q + K * K;
      shelf_[c] = Biquad{(Vh + Vb * K / Q + K * K) / a0, 2.0 * (K * K - Vh) / a0, (Vh - Vb * K / Q + K * K) / a0,
                         2.0 * (K * K - 1.0) / a0, (1.0 - K / Q + K * K) / a0};
      K = std::tan(kPi * 38.13547087602444 / fs);
      const double Q2 = 0.5003270373238773;
      a0 = 1.0 + K / Q2 + K * K;
      hp_[c] = Biquad{1.0, -2.0, 1.0, 2.0 * (K * K - 1.0) / a0, (1.0 - K / Q2 + K * K) / a0};
    }
    hop_ = long(fs * 0.1);
  }
  void add(const int16_t* buf, int frames) {
    for (int i = 0; i < frames; ++i) {
      double e = 0.0;
      for (int c = 0; c < 2; ++c) {
        const double y = hp_[c].run(shelf_[c].run(double(buf[2 * i + c]) / 32768.0));
        e += y * y;
      }
      acc_ += e;
      if (++n_ == hop_) {
        hops_.push_back(acc_ / double(hop_));
        acc_ = 0.0;
        n_ = 0;
      }
    }
  }
  double integrated() const {
    std::vector<double> blocks;
    for (size_t i = 3; i < hops_.size(); ++i)
      blocks.push_back(0.25 * (hops_[i] + hops_[i - 1] + hops_[i - 2] + hops_[i - 3]));
    auto lufs = [](double ms) { return -0.691 + 10.0 * std::log10(ms + 1e-30); };
    double sum = 0.0;
    int n = 0;
    for (double b : blocks)
      if (lufs(b) > -70.0) { sum += b; ++n; }
    if (!n) return -100.0;
    const double rel = lufs(sum / n) - 10.0;
    sum = 0.0;
    n = 0;
    for (double b : blocks)
      if (lufs(b) > -70.0 && lufs(b) > rel) { sum += b; ++n; }
    return n ? lufs(sum / n) : -100.0;
  }

 private:
  double fs_;
  Biquad shelf_[2], hp_[2];
  long hop_, n_ = 0;
  double acc_ = 0.0;
  std::vector<double> hops_;
};

long songSteps(const Project& p) {
  long steps = 0;
  for (int s = 0; s < kSongSlots && p.song[s].repeats; ++s)
    steps += long(p.patterns[p.song[s].pattern].length) * p.song[s].repeats;
  return steps;
}

long stepsToFrames(long steps, int bpm, float sr) {
  return long(std::ceil(double(steps) * double(sr) * 15.0 / double(bpm) / kQuantum)) * kQuantum;
}

// 1. legal values, metadata, song shape
void testStatic(int d) {
  g_inst.init(44100.0f, true);
  const Engine& eng = g_inst.engine();
  demo::build(eng, g_a, d);
  std::memcpy(&g_b, &g_a, sizeof(Project));
  eng.sanitizeProject(g_b);
  int firstDiff = -1;
  const uint8_t* a = reinterpret_cast<const uint8_t*>(&g_a);
  const uint8_t* b = reinterpret_cast<const uint8_t*>(&g_b);
  for (size_t i = 0; i < sizeof(Project); ++i)
    if (a[i] != b[i]) { firstDiff = int(i); break; }
  it::check(firstDiff < 0, "demo %d: sanitizeProject changed byte %d (an illegal value in the demo)", d, firstDiff);

  const char* nm = demo::name(d);
  it::check(nm && std::strncmp(nm, g_a.name, sizeof(g_a.name)) == 0, "demo %d: name() and Project::name differ", d);
  const size_t len = std::strlen(g_a.name);
  it::check(len >= 1 && len <= 8, "demo %d: name '%s' must be 1..8 characters", d, g_a.name);
  for (size_t i = 0; i < len; ++i) {
    const char c = g_a.name[i];
    it::check((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_',
              "demo %d: name '%s' has a character outside A-Z 0-9 - _", d, g_a.name);
  }
  it::check(demo::tempo(d) == g_a.globals[kGTempo], "demo %d: tempo() %d != project tempo %d", d, demo::tempo(d),
            g_a.globals[kGTempo]);
  const double sec = double(songSteps(g_a)) * 15.0 / double(g_a.globals[kGTempo]);
  it::check(sec >= 90.0 && sec <= 210.0, "demo %d: song chain lasts %.0f s (want 1.5 .. 3.5 min)", d, sec);
  // Jam patterns (P1-P3 and the P8 loop): the bass and lead CUT must not be
  // locked, or the CUT knob / SHIFT+[ ] bass sweep would do nothing.
  for (int pt : {0, 1, 2, 7})
    for (int t : {int(kTrackBass), int(kTrackLead)})
      for (int s = 0; s < kMaxSteps; ++s) {
        const Step& st = g_a.patterns[pt].tracks[t].steps[s];
        it::check(!((st.flags() & kStepOn) && st.lockParam() == 1), "demo %d P%d: %s CUT locked on step %d (jam pattern)",
                  d, pt + 1, t == kTrackBass ? "bass" : "lead", s + 1);
      }
  it::check(g_a.songMode == 0, "demo %d: must boot in loop mode (songMode 0) on pattern 1", d);
  it::check(g_a.song[0].repeats > 0, "demo %d: the song chain must be programmed for the SONG view", d);
  it::check(g_a.globals[kGDuckSource] != 0 && g_a.tracks[kTrackBass].params[kMixDuck] > 0,
            "demo %d: the sidechain ducker should be on and pulling the bass", d);
  std::printf("  demo %d %-8s %3d BPM  song %.0f s\n", d, g_a.name, g_a.globals[kGTempo], sec);
}

struct Result {
  it::Stats st;
  double lufs;
};

// Plays the song chain once (pattern < 0) or one pattern `loops` times.
Result play(int d, float sr, int pattern, int loops) {
  g_inst.init(sr, true);
  Engine& eng = g_inst.engine();
  Project& p = g_inst.project();
  demo::build(eng, p, d);
  eng.sanitizeProject(p);
  engine_detail::setRngSeed(4242u);
  long steps;
  if (pattern < 0) {
    p.songMode = 1;
    steps = songSteps(p);
  } else {
    p.songMode = 0;
    it::post(g_inst, CmdType::QueuePattern, pattern);
    steps = long(p.patterns[pattern].length) * loops;
  }
  it::post(g_inst, CmdType::Play);
  Result r;
  Loudness meter(sr);
  static int16_t buf[2 * 256];
  const long frames = stepsToFrames(steps, p.globals[kGTempo], sr);
  for (long done = 0; done < frames;) {
    const int n = int(frames - done < 256 ? frames - done : 256);
    g_inst.process(buf, n);
    r.st.add(buf, n);
    meter.add(buf, n);
    done += n;
  }
  r.lufs = meter.integrated();
  return r;
}

void checkClean(const Result& r, int d, const char* what, uint32_t faults) {
  it::check(faults == 0, "demo %d %s: %u engine faults (NaN / absurd signal)", d, what, faults);
  it::check(r.st.minInt == 0, "demo %d %s: %ld samples at -32768 (int16 wrap)", d, what, r.st.minInt);
  it::check(r.st.maxJump < 50000, "demo %d %s: sample step %d looks like a wrap", d, what, r.st.maxJump);
  it::check(r.st.lrDiffer == 0, "demo %d %s: mono device render has L != R", d, what);
  it::check(double(r.st.fullScale) < 1e-4 * double(2 * r.st.frames), "demo %d %s: limiter at full scale on %ld samples",
            d, what, r.st.fullScale);
}

// 2. the whole song, device mode
void testSong(int d) {
  const uint32_t f0 = engine_detail::faultCount();
  const Result r = play(d, 44100.0f, -1, 0);
  checkClean(r, d, "song", engine_detail::faultCount() - f0);
  std::printf("  demo %d song   %.1f s  %6.1f LUFS  peak %6.2f dBFS\n", d, double(r.st.frames) / 44100.0, r.lufs,
              r.st.peakDb());
  it::check(r.lufs > -16.0 && r.lufs < -10.0, "demo %d song: integrated %.1f LUFS outside -16 .. -10", d, r.lufs);
  it::check(r.st.peakDb() < -0.5, "demo %d song: sample peak %.2f dBFS above -0.5", d, r.st.peakDb());
  it::check(r.st.peakDb() > -8.0, "demo %d song: sample peak %.2f dBFS, suspiciously quiet", d, r.st.peakDb());

  const uint32_t f1 = engine_detail::faultCount();
  const Result r32 = play(d, 32000.0f, -1, 0);
  checkClean(r32, d, "song 32 kHz", engine_detail::faultCount() - f1);
  it::check(std::fabs(r32.lufs - r.lufs) < 1.5, "demo %d: 32 kHz song %.1f LUFS vs %.1f at 44.1 kHz", d, r32.lufs,
            r.lufs);
}

// 3. each pattern of the song on its own (a breakdown may be quieter)
void testPatterns(int d) {
  g_inst.init(44100.0f, true);
  demo::build(g_inst.engine(), g_a, d);
  bool used[kNumPatterns] = {};
  for (int s = 0; s < kSongSlots && g_a.song[s].repeats; ++s) used[g_a.song[s].pattern] = true;
  for (int pt = 0; pt < kNumPatterns; ++pt) {
    if (!used[pt]) continue;
    const uint32_t f0 = engine_detail::faultCount();
    const Result r = play(d, 44100.0f, pt, 2);
    char what[24];
    std::snprintf(what, sizeof(what), "pattern %d", pt + 1);
    checkClean(r, d, what, engine_detail::faultCount() - f0);
    std::printf("  demo %d P%-2d  %6.1f LUFS  peak %6.2f dBFS\n", d, pt + 1, r.lufs, r.st.peakDb());
    it::check(r.lufs > -24.0 && r.lufs < -9.5, "demo %d P%d: %.1f LUFS outside -24 .. -9.5", d, pt + 1, r.lufs);
    it::check(r.st.peakDb() < -0.5, "demo %d P%d: sample peak %.2f dBFS above -0.5", d, pt + 1, r.st.peakDb());
  }
}

}  // namespace

int main() {
  it::check(demo::count() >= 3, "expected at least 3 demos, got %d", demo::count());
  it::check(demo::name(demo::count()) == nullptr && demo::name(-1) == nullptr, "name() out of range must be nullptr");
  for (int d = 0; d < demo::count(); ++d) {
    testStatic(d);
    testSong(d);
    testPatterns(d);
  }
  return it::finish("itest_demo");
}
