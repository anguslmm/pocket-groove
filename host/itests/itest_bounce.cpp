// System test for the stem bounce (host/common/bounce.h, pg/engine/stem_tap.h,
// host/tools/bounce.cpp):
//   1. the 8 track stems + delay return + reverb return sum to the pre-master
//      bus to float rounding, sample by sample, for every demo;
//   2. the tap only observes: the int16 output with the tap installed is
//      bit-identical to the same render without it, and the bounce's mix is
//      bit-identical to a render_demo-style render up to the song end;
//   3. the render is deterministic (two runs, same hash) and the song ends
//      where the chain says (steps x step length, to one quantum), with
//      nothing fired afterwards;
//   4. wet stems (one pass per track) sum to the dry mix within the delay
//      line's int16 quantisation; --no-duck stems keep the sequencing;
//   5. the bounce tool writes files that all have the same length and format
//      (16, 24, 32f), named and numbered as documented.
#include <cstdlib>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "common/bounce.h"
#include "itests/itest_util.h"
#include "pg/demo/demo.h"

using namespace pg;

namespace {

Instrument g_inst;
Project g_proj;

struct Capture {
  std::vector<int16_t> mix;
  std::vector<float> track[kNumTracks][2], delay[2], reverb[2], pre[2];
  void add(const bounce::Block& b) {
    mix.insert(mix.end(), b.mix, b.mix + 2 * b.n);
    for (int c = 0; c < 2; ++c) {
      for (int t = 0; t < kNumTracks; ++t) track[t][c].insert(track[t][c].end(), b.track[t][c], b.track[t][c] + b.n);
      delay[c].insert(delay[c].end(), b.delay[c], b.delay[c] + b.n);
      reverb[c].insert(reverb[c].end(), b.reverb[c], b.reverb[c] + b.n);
      pre[c].insert(pre[c].end(), b.pre[c], b.pre[c] + b.n);
    }
  }
  long frames() const { return long(mix.size() / 2); }
};

void buildDemo(int d) {
  g_inst.init(44100.0f, false);
  demo::build(g_inst.engine(), g_proj, d);
  g_inst.engine().sanitizeProject(g_proj);
}

bool run(const bounce::Options& o, const bounce::Pass& pass, Capture& cap, bounce::PassResult& res,
         bounce::Plan* planOut = nullptr) {
  bounce::Plan plan;
  std::string err;
  if (!bounce::makePlan(g_proj, o, plan, &err)) { it::check(false, "makePlan: %s", err.c_str()); return false; }
  if (planOut) *planOut = plan;
  const bool ok = bounce::renderPass(g_inst, g_proj, o, plan, pass, 0, [&](const bounce::Block& b) { cap.add(b); },
                                     res, &err);
  it::check(ok, "renderPass: %s", err.c_str());
  return ok;
}

// 1. sum of stems == pre-master
void testSum(int d, const bounce::Options& o, const char* what) {
  buildDemo(d);
  Capture cap;
  bounce::PassResult res;
  if (!run(o, bounce::Pass{}, cap, res)) return;
  double maxErr = 0.0, maxPre = 0.0, maxStem = 0.0;
  long bad = 0;
  for (int c = 0; c < 2; ++c) {
    for (long i = 0; i < cap.frames(); ++i) {
      double sum = 0.0, mag = 0.0;
      for (int t = 0; t < kNumTracks; ++t) {
        sum += double(cap.track[t][c][size_t(i)]);
        mag += double(std::fabs(cap.track[t][c][size_t(i)]));
        maxStem = std::max(maxStem, double(std::fabs(cap.track[t][c][size_t(i)])));
      }
      sum += double(cap.delay[c][size_t(i)]) + double(cap.reverb[c][size_t(i)]);
      mag += double(std::fabs(cap.delay[c][size_t(i)])) + double(std::fabs(cap.reverb[c][size_t(i)]));
      const double p = cap.pre[c][size_t(i)];
      const double e = std::fabs(sum - p);
      // float rounding of a 10-term sum: a few ulps of the summed magnitude
      if (e > 4.0e-7 * mag + 1.0e-9) ++bad;
      maxErr = std::max(maxErr, e);
      maxPre = std::max(maxPre, std::fabs(p));
    }
  }
  it::check(bad == 0, "%s: %ld samples where stems + returns != pre-master (max error %.3g)", what, bad, maxErr);
  it::check(maxPre > 0.1 && maxStem > 0.05, "%s: render is (nearly) silent: pre-master peak %.3f", what, maxPre);
  it::check(res.faults == 0 && !res.overflow, "%s: faults %u overflow %d", what, res.faults, int(res.overflow));
  std::printf("  sum %-26s %8ld frames, pre-master peak %.3f, max |stems+returns-pre| %.3g (%.1f dB below peak)\n",
              what, cap.frames(), maxPre, maxErr, 20.0 * std::log10(maxPre / std::max(maxErr, 1e-30)));
}

// render_demo's sequence of calls (demo build path, song mode set in the
// project, Play, 256-frame blocks), no tap, no end hold.
std::vector<int16_t> renderDemoStyle(int d, long frames) {
  g_inst.init(44100.0f, false);
  Engine& eng = g_inst.engine();
  Project& p = g_inst.project();
  demo::build(eng, p, d);
  eng.sanitizeProject(p);
  p.songMode = 1;
  Command play{};
  play.type = CmdType::Play;
  eng.post(play);
  std::vector<int16_t> pcm(size_t(frames) * 2);
  for (long f = 0; f < frames; f += 256) g_inst.process(pcm.data() + 2 * f, int(std::min<long>(256, frames - f)));
  return pcm;
}

// 2 + 3: the tap is a pure observer; mix == render_demo; determinism; end of song.
void testMixAndEnd() {
  buildDemo(0);
  bounce::Options o;
  o.tailSec = 3.0f;
  Capture a, b;
  bounce::PassResult ra, rb;
  bounce::Plan plan;
  if (!run(o, bounce::Pass{}, a, ra, &plan) || !run(o, bounce::Pass{}, b, rb)) return;
  it::check(plan.song && plan.steps == 1152, "ACIDJAM plan: song %d steps %ld", int(plan.song), plan.steps);
  it::check(ra.hash == rb.hash && a.mix == b.mix && a.pre[0] == b.pre[0] && a.track[6][1] == b.track[6][1],
            "two identical bounces differ");
  // Song end: steps * stepLen, landing on the quantum edge nearest the ideal time.
  const double ideal = double(plan.steps) * 44100.0 * 15.0 / double(plan.bpm);
  it::check(std::fabs(double(ra.endFrame) - ideal) <= kQuantum, "song ended at frame %ld, chain says %.1f", ra.endFrame, ideal);
  it::check(ra.frames >= ra.endFrame + long(3.0 * 44100) - kQuantum, "tail too short: %ld frames after the end",
            ra.frames - ra.endFrame);
  // Nothing fires after the end: the kick (a one-shot, no FX sends in ACIDJAM)
  // is silent from 0.6 s after the end.
  double kickAfter = 0.0;
  for (long i = ra.endFrame + 26460; i < a.frames(); ++i)
    kickAfter = std::max(kickAfter, double(std::fabs(a.track[0][0][size_t(i)])));
  it::check(kickAfter < 1.0e-4, "kick still sounds after the song end: %.3g", kickAfter);

  // Same render without the tap (end hold only): bit-identical int16.
  {
    g_inst.init(44100.0f, false);
    Engine& eng = g_inst.engine();
    eng.suspend();
    std::memcpy(&g_inst.project(), &g_proj, sizeof(Project));
    eng.sanitizeProject(g_inst.project());
    eng.resume();
    engine_detail::setEndAfterPatternPlays(plan.plays);
    for (const auto& tc : plan.timeline) eng.post(tc.cmd);
    std::vector<int16_t> pcm(size_t(a.frames()) * 2);
    for (long f = 0; f < a.frames(); f += 256) g_inst.process(pcm.data() + 2 * f, int(std::min<long>(256, a.frames() - f)));
    engine_detail::setEndAfterPatternPlays(-1);
    it::check(pcm == a.mix, "the stem tap changed the int16 output");
  }
  // render_demo: identical up to the song end (render_demo then sends Stop, the bounce lets it ring).
  {
    const std::vector<int16_t> ref = renderDemoStyle(0, ra.endFrame);
    long diff = 0;
    for (size_t i = 0; i < ref.size(); ++i) diff += ref[i] != a.mix[i];
    it::check(diff == 0, "bounce mix differs from render_demo in %ld samples before the song end", diff);
  }
}

// 4. wet stems and no-duck stems
void testWetAndNoDuck() {
  buildDemo(1);   // BASSHAUS: sends on several tracks, heavy ducking
  bounce::Options o;
  o.pattern = 0;
  o.loops = 1;
  o.tailSec = 2.0f;
  Capture dry;
  bounce::PassResult rd;
  if (!run(o, bounce::Pass{}, dry, rd)) return;
  std::vector<float> wetSum[2] = {std::vector<float>(size_t(dry.frames()), 0.0f), std::vector<float>(size_t(dry.frames()), 0.0f)};
  for (int t = 0; t < kNumTracks; ++t) {
    bounce::Pass p;
    p.sendMask = 1u << t;
    Capture w;
    bounce::PassResult rw;
    if (!run(o, p, w, rw)) return;
    it::check(rw.endFrame == rd.endFrame && w.frames() == dry.frames(), "wet pass %d: different length", t);
    it::check(w.track[t][0] == dry.track[t][0], "wet pass %d: the dry part of the track changed", t);
    for (int c = 0; c < 2; ++c)
      for (long i = 0; i < w.frames(); ++i)
        wetSum[c][size_t(i)] += w.track[t][c][size_t(i)] + w.delay[c][size_t(i)] + w.reverb[c][size_t(i)];
  }
  double maxErr = 0.0, maxPre = 0.0, errPow = 0.0, prePow = 0.0;
  for (int c = 0; c < 2; ++c)
    for (long i = 0; i < dry.frames(); ++i) {
      const double e = double(wetSum[c][size_t(i)]) - double(dry.pre[c][size_t(i)]);
      maxErr = std::max(maxErr, std::fabs(e));
      maxPre = std::max(maxPre, double(std::fabs(dry.pre[c][size_t(i)])));
      errPow += e * e;
      prePow += double(dry.pre[c][size_t(i)]) * double(dry.pre[c][size_t(i)]);
    }
  const double snr = 10.0 * std::log10(prePow / std::max(errPow, 1e-30));
  // The delay line stores int16 and its feedback path rounds, so the per-track
  // FX shares do not add up exactly; the residual must stay far below the mix.
  it::check(snr > 60.0, "wet stems sum to the mix only to %.1f dB", snr);
  std::printf("  wet stems: sum vs pre-master: residual %.1f dB below the mix (max error %.3g, peak %.3f)\n", snr,
              maxErr, maxPre);

  bounce::Pass nd;
  nd.noDuck = true;
  Capture n;
  bounce::PassResult rn;
  if (!run(o, nd, n, rn)) return;
  it::check(rn.endFrame == rd.endFrame && n.frames() == dry.frames(), "no-duck pass: different length");
  it::check(n.track[0][0] == dry.track[0][0] || g_proj.tracks[0].params[kMixDuck] != 0,
            "no-duck: the (unducked) kick stem changed, so sequencing differs");
  double dBass = 0.0;
  for (long i = 0; i < n.frames(); ++i)
    dBass = std::max(dBass, double(std::fabs(n.track[kTrackBass][0][size_t(i)] - dry.track[kTrackBass][0][size_t(i)])));
  it::check(g_proj.tracks[kTrackBass].params[kMixDuck] == 0 || dBass > 0.01, "no-duck: the ducked bass did not change");
}

// 5. the tool's files
struct WavInfo {
  int fmt = 0, ch = 0, sr = 0, bits = 0;
  long frames = -1;
};
WavInfo readWav(const std::string& path) {
  WavInfo w;
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return w;
  uint8_t h[12];
  if (std::fread(h, 1, 12, f) != 12 || std::memcmp(h, "RIFF", 4) || std::memcmp(h + 8, "WAVE", 4)) { std::fclose(f); return w; }
  for (;;) {
    uint8_t c[8];
    if (std::fread(c, 1, 8, f) != 8) break;
    const uint32_t len = uint32_t(c[4]) | uint32_t(c[5]) << 8 | uint32_t(c[6]) << 16 | uint32_t(c[7]) << 24;
    if (!std::memcmp(c, "fmt ", 4)) {
      uint8_t b[16];
      if (std::fread(b, 1, 16, f) != 16) break;
      w.fmt = b[0] | b[1] << 8;
      w.ch = b[2] | b[3] << 8;
      w.sr = int(uint32_t(b[4]) | uint32_t(b[5]) << 8 | uint32_t(b[6]) << 16);
      w.bits = b[14] | b[15] << 8;
      std::fseek(f, long(len) - 16, SEEK_CUR);
    } else if (!std::memcmp(c, "data", 4)) {
      if (w.ch && w.bits) w.frames = long(len / uint32_t(w.ch * w.bits / 8));
      std::fseek(f, 0, SEEK_END);
      const long end = std::ftell(f);
      std::fseek(f, 0, SEEK_SET);
      if (w.frames >= 0 && end < long(len)) w.frames = -2;   // truncated
      break;
    } else {
      std::fseek(f, long(len), SEEK_CUR);
    }
  }
  std::fclose(f);
  return w;
}

void testTool(const std::string& exeDir) {
  const std::string tool = exeDir + "/bounce";
  struct stat st;
  if (::stat(tool.c_str(), &st) != 0) {
    it::check(false, "bounce tool not found at %s", tool.c_str());
    return;
  }
  const std::string out = "build/itest_bounce";
  struct Variant { const char* args; const char* bitsName; int fmt, bits, files; };
  const Variant vs[] = {
      {"--pattern 2 --loops 1 --tail 2", "32f", 3, 32, 11},
      {"--pattern 2 --loops 1 --tail 2 --bits 24 --wet-stems", "24", 1, 24, 9},
      {"--pattern 2 --loops 1 --tail 1 --bits 16 --no-duck --sr 32000", "16", 1, 16, 11},
  };
  static const char* kFiles[] = {"01_kick", "02_snare", "03_clap", "04_closedhat", "05_openhat", "06_perc",
                                 "07_bass", "08_lead", "09_delay", "10_reverb"};
  for (const Variant& v : vs) {
    const std::string cmd = tool + " --demo 2 --out " + out + " " + v.args + " > /dev/null";
    const int rc = std::system(cmd.c_str());
    it::check(rc == 0, "%s failed (%d)", cmd.c_str(), rc);
    if (rc != 0) continue;
    const std::string pre = out + "/WAREHOUS_" + std::to_string(demo::tempo(2)) + "bpm_";
    std::vector<std::string> files;
    const int stems = v.files - 1;
    for (int k = 0; k < stems; ++k) files.push_back(pre + kFiles[k] + ".wav");
    files.push_back(pre + "mix.wav");
    long frames = -1;
    for (const std::string& f : files) {
      const WavInfo w = readWav(f);
      it::check(w.fmt == v.fmt && w.bits == v.bits && w.ch == 2, "%s: format %d bits %d ch %d", f.c_str(), w.fmt, w.bits, w.ch);
      it::check(w.frames > 0, "%s: no data (%ld)", f.c_str(), w.frames);
      if (frames < 0) frames = w.frames;
      it::check(w.frames == frames, "%s: %ld frames, others %ld", f.c_str(), w.frames, frames);
      it::check(w.sr == (std::strstr(v.args, "32000") ? 32000 : 44100), "%s: rate %d", f.c_str(), w.sr);
    }
    // No stale files of another layout (wet bounces have no return files).
    const bool hasDelay = ::stat((pre + "09_delay.wav").c_str(), &st) == 0;
    it::check(hasDelay == (v.files == 11), "%s: delay return file present=%d", v.args, int(hasDelay));
    it::check(::stat((pre + "info.txt").c_str(), &st) == 0, "%s: no info file", v.args);
  }
}

}  // namespace

int main(int, char** argv) {
  bounce::Options song;
  song.tailSec = 4.0f;
  testSum(0, song, "ACIDJAM song");
  bounce::Options pat;
  pat.pattern = 0;
  pat.loops = 2;
  pat.tailSec = 3.0f;
  testSum(1, pat, "BASSHAUS p1 x2");
  testSum(2, pat, "WAREHOUS p1 x2");
  pat.sampleRate = 32000.0f;
  testSum(1, pat, "BASSHAUS p1 x2 32k");
  testMixAndEnd();
  testWetAndNoDuck();
  std::string exe = argv[0];
  const size_t slash = exe.rfind('/');
  testTool(slash == std::string::npos ? "." : exe.substr(0, slash));
  return it::finish("itest_bounce");
}
