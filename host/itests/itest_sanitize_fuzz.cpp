// Engine::sanitizeProject on random bytes: an engine fed a sanitized
// random-bytes project (as a corrupt / hostile file load would deliver) never
// produces NaN (no engine faults), never crashes and stays bounded; the
// sanitized project is in range field by field, sanitizing is idempotent, and
// a valid project (initProject, the built-in grooves) is left byte-identical.
#include "common/grooves.h"
#include "itests/itest_util.h"

using namespace pg;

namespace {

Instrument g_inst;
Project g_a, g_b;

// Field-by-field range check against the documented limits.
bool inRange(const Engine& eng, const Project& p, const char** why) {
  *why = "";
  if (std::memcmp(p.magic, "PGRV", 4) != 0 || p.version != kProjectVersion) { *why = "magic/version"; return false; }
  if (p.songMode > 1) { *why = "songMode"; return false; }
  bool nul = false;
  for (char c : p.name) nul |= c == 0;
  if (!nul || p.name[15] != 0) { *why = "name not NUL terminated"; return false; }
  for (int i = 0; i < kNumGlobals; ++i) {
    const ParamInfo* info = Engine::globalParamInfo(i);
    if (info ? (p.globals[i] < info->min || p.globals[i] > info->max) : p.globals[i] != 0) { *why = "global"; return false; }
  }
  for (int i = 0; i < kSongSlots; ++i)
    if (p.song[i].pattern >= kNumPatterns) { *why = "song slot"; return false; }
  for (int t = 0; t < kNumTracks; ++t) {
    const TrackSettings& ts = p.tracks[t];
    if (ts.mute > 1) { *why = "mute"; return false; }
    for (int i = 0; i < kParamsPerTrack; ++i) {
      const ParamInfo* info = eng.trackParamInfo(t, i);
      if (info ? (ts.params[i] < info->min || ts.params[i] > info->max) : ts.params[i] != 0) { *why = "track param"; return false; }
    }
  }
  for (int pt = 0; pt < kNumPatterns; ++pt) {
    const Pattern& pat = p.patterns[pt];
    if (pat.length < 1 || pat.length > kMaxSteps) { *why = "pattern length"; return false; }
    for (int t = 0; t < kNumTracks; ++t) {
      const TrackPattern& tp = pat.tracks[t];
      if (tp.length < 1 || tp.length > kMaxSteps) { *why = "track length"; return false; }
      for (const Step& s : tp.steps) {
        if (s.note > 127 || s.vel < 1 || s.vel > 127 || s.gate < 1 || s.prob > 100) { *why = "step field"; return false; }
        if (s.flags & ~(kStepOn | kStepAccent | kStepSlide | kStepRatchetMask)) { *why = "step flags"; return false; }
        if (s.chord >= kChordNum || (t != kTrackLead && s.chord)) { *why = "chord"; return false; }
        if (s.lockParam > kParamsPerTrack) { *why = "lockParam"; return false; }
        if (s.lockParam) {
          const ParamInfo* info = eng.trackParamInfo(t, s.lockParam - 1);
          if (!info || s.lockValue < info->min || s.lockValue > info->max) { *why = "lock"; return false; }
        }
      }
    }
  }
  return true;
}

void testValidUnchanged() {
  g_inst.init(44100.0f, false);
  const Engine& eng = g_inst.engine();
  eng.initProject(g_a);
  std::memcpy(&g_b, &g_a, sizeof(Project));
  eng.sanitizeProject(g_b);
  it::check(std::memcmp(&g_a, &g_b, sizeof(Project)) == 0, "sanitize changed a fresh initProject");
  grooves::programGroove(g_a);
  std::memcpy(&g_b, &g_a, sizeof(Project));
  eng.sanitizeProject(g_b);
  it::check(std::memcmp(&g_a, &g_b, sizeof(Project)) == 0, "sanitize changed a valid programmed project");
}

void testRandom(int iterations, float sr, bool mono, uint32_t seed) {
  it::Rng r(seed);
  int failsRange = 0, failsIdem = 0;
  uint32_t faults = 0;
  it::Stats all;
  for (int k = 0; k < iterations; ++k) {
    g_inst.init(sr, mono);
    Engine& eng = g_inst.engine();
    Project& p = g_inst.project();
    // Random bytes; every third one keeps a valid magic so it looks like a real file.
    uint8_t* b = reinterpret_cast<uint8_t*>(&p);
    for (size_t i = 0; i < sizeof(Project); ++i) b[i] = r.byte();
    if (k % 3 == 0) { std::memcpy(p.magic, "PGRV", 4); p.version = kProjectVersion; }
    eng.suspend();
    eng.sanitizeProject(p);
    eng.resume();
    const char* why;
    if (!inRange(eng, p, &why)) { if (++failsRange < 4) std::printf("    out of range after sanitize: %s\n", why); }
    std::memcpy(&g_a, &p, sizeof(Project));
    eng.sanitizeProject(g_a);
    if (std::memcmp(&g_a, &p, sizeof(Project)) != 0) ++failsIdem;
    // Play it as the bytes say (random mutes: almost everything muted), then
    // unmuted at unity master volume so the random content is really heard.
    const uint32_t f0 = engine_detail::faultCount();
    it::post(g_inst, CmdType::Play);
    it::Stats st;
    it::render(g_inst, long(1.0 * double(sr)) / 256 * 256, st);
    for (int t = 0; t < kNumTracks; ++t) it::post(g_inst, CmdType::SetMute, t, 0, 0);
    it::post(g_inst, CmdType::SetGlobal, kGMasterVolume, 0, 100);
    it::render(g_inst, long(3.0 * double(sr)) / 256 * 256, st);
    it::post(g_inst, CmdType::Stop);
    it::render(g_inst, long(0.5 * double(sr)) / 256 * 256, st);
    faults += engine_detail::faultCount() - f0;
    all.peak = all.peak > st.peak ? all.peak : st.peak;
    all.minInt += st.minInt;
    all.frames += st.frames;
    all.lrDiffer += st.lrDiffer;
  }
  std::printf("  %d random-bytes projects, %s %.0f Hz: out-of-range %d, not idempotent %d, faults %u, peak %.2f dBFS\n",
              iterations, mono ? "mono" : "stereo", double(sr), failsRange, failsIdem, faults, all.peakDb());
  it::check(failsRange == 0, "sanitized project out of range (%d)", failsRange);
  it::check(failsIdem == 0, "sanitize not idempotent (%d)", failsIdem);
  it::check(faults == 0, "engine faults %u on sanitized random projects", faults);
  it::check(all.minInt == 0, "int16 wrap");
  if (mono) it::check(all.lrDiffer == 0, "mono: L != R");
}

}  // namespace

int main() {
  std::printf("== sanitizeProject ==\n");
  testValidUnchanged();
  testRandom(60, 44100.0f, false, 0x5EEDu);
  testRandom(40, 32000.0f, true, 0xFACEu);
  return it::finish("itest_sanitize_fuzz");
}
