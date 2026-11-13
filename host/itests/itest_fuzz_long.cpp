// Long run: 10 minutes of audio (6 min stereo 44.1 kHz, 4 min mono device
// 44.1 kHz) with random Commands of EVERY CmdType (and invalid type values)
// posted between blocks, with random arguments that are often out of range,
// random step contents, WriteTrack / WritePattern from random-byte buffers
// (honouring the writesDone hand-shake), and every 30 s a random-bytes
// project loaded through suspend / sanitizeProject / resume.
// Checks: no crash, no engine faults (NaN / absurd signal), no int16 wrap,
// status fields in range, and RAM around the instrument and around the write
// buffers untouched (canaries; the ASan/UBSan build checks the rest).
#include "common/grooves.h"
#include "itests/itest_util.h"

using namespace pg;

namespace {

constexpr int kCanary = 4096;
struct Guarded {
  uint8_t pre[kCanary];
  Instrument inst;
  uint8_t mid[kCanary];
  TrackPattern trackBuf;
  Pattern patBuf;
  uint8_t post[kCanary];
};
Guarded g;
Project g_scratch;

void fillCanaries() {
  for (int i = 0; i < kCanary; ++i) g.pre[i] = g.mid[i] = g.post[i] = uint8_t(0xA5 ^ i);
}
bool canariesOk() {
  for (int i = 0; i < kCanary; ++i)
    if (g.pre[i] != uint8_t(0xA5 ^ i) || g.mid[i] != uint8_t(0xA5 ^ i) || g.post[i] != uint8_t(0xA5 ^ i)) return false;
  return true;
}

uint8_t arg(it::Rng& r, int legalMax) {
  // Mostly legal values, often just past the edge, sometimes anything.
  switch (r.range(0, 5)) {
    case 0: return r.byte();
    case 1: return uint8_t(legalMax + r.range(0, 3));
    default: return uint8_t(r.range(0, legalMax > 0 ? legalMax - 1 : 0));
  }
}

constexpr int kNumCmdTypes = int(CmdType::WritePattern) + 1;
uint8_t g_writesExpected = 0;
bool g_writePending = false;
long g_posted[kNumCmdTypes + 1] = {};

void randomCommand(it::Rng& r) {
  Instrument& inst = g.inst;
  const EngineStatus st = inst.engine().status();
  if (g_writePending && st.writesDone == g_writesExpected) g_writePending = false;
  Command c{};
  int ty = r.range(0, kNumCmdTypes + 2);   // a few invalid type values too
  if (ty >= kNumCmdTypes) ty = 200 + r.range(0, 55);
  // Bias towards keeping the transport running.
  if (r.range(0, 9) == 0) ty = int(CmdType::Play);
  c.type = CmdType(ty);
  c.a = arg(r, 16);
  c.b = arg(r, 32);
  c.c = arg(r, 128);
  for (size_t i = 0; i < sizeof(Step); ++i) reinterpret_cast<uint8_t*>(&c.step)[i] = r.byte();
  if (r.range(0, 2)) c.step.setProb(uint8_t(r.range(0, 100)));
  c.ptr = nullptr;
  if (c.type == CmdType::WriteTrack || c.type == CmdType::WritePattern) {
    if (g_writePending) return;             // the buffer still belongs to the engine
    if (r.range(0, 4) != 0) {
      uint8_t* dst = c.type == CmdType::WriteTrack ? reinterpret_cast<uint8_t*>(&g.trackBuf) : reinterpret_cast<uint8_t*>(&g.patBuf);
      const size_t n = c.type == CmdType::WriteTrack ? sizeof(TrackPattern) : sizeof(Pattern);
      for (size_t i = 0; i < n; ++i) dst[i] = r.byte();
      c.ptr = dst;
    }
    if (!inst.engine().post(c)) return;
    g_writePending = true;
    g_writesExpected = uint8_t(st.writesDone + 1);
    ++g_posted[ty];
    return;
  }
  if (c.type == CmdType::SetGlobal && r.range(0, 1)) c.a = uint8_t(r.range(0, kGNumUsed - 1));
  if (c.type == CmdType::SetTrackParam && r.range(0, 1)) { c.a = uint8_t(r.range(0, 7)); c.b = uint8_t(r.range(0, 31)); c.c = r.byte(); }
  if (inst.engine().post(c)) ++g_posted[ty < kNumCmdTypes ? ty : kNumCmdTypes];
}

void loadRandomProject(it::Rng& r) {
  uint8_t* b = reinterpret_cast<uint8_t*>(&g_scratch);
  for (size_t i = 0; i < sizeof(Project); ++i) b[i] = r.byte();
  Engine& eng = g.inst.engine();
  eng.suspend();
  std::memcpy(&g.inst.project(), &g_scratch, sizeof(Project));
  eng.sanitizeProject(g.inst.project());
  eng.resume();
  // Random mute bytes sanitize to "muted" almost always: unmute so it plays.
  for (int t = 0; t < kNumTracks; ++t) it::post(g.inst, CmdType::SetMute, t, 0, 0);
  it::post(g.inst, CmdType::Play);
}

void run(float sr, bool mono, double minutes, uint32_t seed) {
  g.inst.init(sr, mono);
  grooves::programBusy(g.inst.project());
  it::post(g.inst, CmdType::Play);
  it::Rng r(seed);
  it::Stats st;
  const uint32_t f0 = engine_detail::faultCount();
  const long blocks = long(minutes * 60.0 * double(sr)) / 256;
  const long loadEvery = long(30.0 * double(sr)) / 256;
  bool statusOk = true;
  long playingChecks = 0, checks = 0;
  for (long b = 0; b < blocks; ++b) {
    const int n = r.range(0, 9) < 7 ? 0 : r.range(1, 6);
    for (int k = 0; k < n; ++k) randomCommand(r);
    if (b % loadEvery == loadEvery - 1) loadRandomProject(r);
    it::render(g.inst, 256, st);
    if ((b & 63) == 0) {
      const EngineStatus s = g.inst.engine().status();
      statusOk &= s.pattern < kNumPatterns && s.songSlot < kSongSlots && s.masterStep < kMaxSteps &&
                  (s.queuedPattern < kNumPatterns || s.queuedPattern == 0xFF) && s.duckGain >= 0.0f && s.duckGain <= 1.0f &&
                  s.peakL >= 0.0f && s.peakL <= 1.0f && s.peakR >= 0.0f && s.peakR <= 1.0f;
      for (int t = 0; t < kNumTracks; ++t) statusOk &= s.step[t] < kMaxSteps;
      ++checks;
      playingChecks += s.playing ? 1 : 0;
    }
  }
  const uint32_t faults = engine_detail::faultCount() - f0;
  std::printf("  %s %.0f Hz, %.1f min: playing %.0f %% of the time, peak %.2f dBFS, rms %.1f dBFS, full-scale samples %ld, "
              "faults %u, L!=R frames %ld\n",
              mono ? "mono  " : "stereo", double(sr), minutes, 100.0 * double(playingChecks) / double(checks ? checks : 1),
              st.peakDb(), st.rmsDb(), st.fullScale, faults, st.lrDiffer);
  it::check(faults == 0, "%s: %u engine faults (NaN / absurd signal)", mono ? "mono" : "stereo", faults);
  it::check(st.minInt == 0, "%s: int16 wrap (%ld samples at -32768)", mono ? "mono" : "stereo", st.minInt);
  it::check(statusOk, "%s: status() field out of range", mono ? "mono" : "stereo");
  it::check(canariesOk(), "%s: memory around the instrument / write buffers was modified", mono ? "mono" : "stereo");
  if (mono) it::check(st.lrDiffer == 0, "mono: L != R");
  it::check(st.frames > 0 && st.peak > 0, "silent run");
}

}  // namespace

int main(int argc, char** argv) {
  // Default: the full 10 minutes. "--minutes M" scales it (6/10 stereo, 4/10 mono).
  double minutes = 10.0;
  if (argc == 3 && !std::strcmp(argv[1], "--minutes")) minutes = std::atof(argv[2]);
  fillCanaries();
  std::printf("== %.1f minutes of audio with random commands ==\n", minutes);
  run(44100.0f, false, 0.6 * minutes, 0xC0FFEEu);
  run(44100.0f, true, 0.4 * minutes, 0xBADF00Du);
  std::printf("  commands posted per type:");
  for (int i = 0; i <= kNumCmdTypes; ++i) std::printf(" %ld", g_posted[i]);
  std::printf(" (last = invalid type values)\n");
  for (int i = 0; i <= kNumCmdTypes; ++i) it::check(g_posted[i] > 0, "command type %d never posted", i);
  return it::finish("itest_fuzz_long");
}
