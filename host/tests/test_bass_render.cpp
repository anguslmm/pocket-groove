// Renders the audition WAVs for the bass voice into build/renders/bass/ and
// sanity-checks each one (finite, bounded, not silent, no DC).
#include "bass_testutil.h"

using namespace bt;
using namespace pg::bass;

namespace {
const char* kDir = "build/renders/bass";
Checker ck;

void finish(Rig& rig, const char* name, float tail = 0.4f) {
  rig.runSeconds(tail);
  const double pk = peak(rig.out), r = rms(rig.out), dc = mean(rig.out);
  ck.check(allFinite(rig.out) && pk < 1.2 && r > 0.01 && std::fabs(dc) < 0.004,
           "%-28s %6.2f s  peak %6.2f dBFS  rms %6.2f dBFS  dc %+.5f", name,
           double(rig.out.size()) / double(rig.sr), dB(pk), dB(r), dc);
  writeMono(std::string(kDir) + "/" + name + ".wav", rig.out, rig.sr);
}

// note, accent, slide, vel, gate
const std::vector<StepEv> kAcidLine = {
    {33, true, false, 110, 0.5f},  {33, false, false, 100, 0.5f}, {45, false, true, 100, 0.5f}, {33, false, false, 100, 0.5f},
    {36, true, false, 110, 0.5f},  {33, false, false, 100, 0.5f}, {0, false, false, 0, 0.5f},   {31, false, true, 100, 0.5f},
    {33, true, false, 110, 0.5f},  {45, false, false, 100, 0.5f}, {33, false, true, 100, 0.5f}, {40, false, false, 100, 0.5f},
    {33, true, false, 110, 0.5f},  {0, false, false, 0, 0.5f},    {38, false, true, 100, 0.5f}, {36, true, false, 110, 0.5f},
};

void acidLine() {
  Rig rig(44100.0f, 126.0f);
  rig.set(kSub, 30);
  // 8 bars: cutoff rises over the first 4 then falls; resonance climbs throughout.
  playPattern(rig, kAcidLine, 126.0f, 8, [&](int, float f) {
    float tri = f < 0.6f ? f / 0.6f : 1.0f - (f - 0.6f) / 0.4f;
    rig.set(kCutoff, int(22.0f + 70.0f * tri));
    rig.set(kReso, int(60.0f + 60.0f * (f < 0.8f ? f / 0.8f : 1.0f)));
    rig.set(kEnvMod, int(60.0f + 30.0f * tri));
  });
  finish(rig, "acid_line_126bpm");
}

void houseBass() {
  Rig rig(44100.0f, 124.0f);
  // defaults = the plucky house bass. Off-beat pattern in F minor.
  const std::vector<StepEv> pat = {
      {0, false, false, 0, 0.5f},     {0, false, false, 0, 0.5f},     {41, false, false, 110, 0.6f}, {41, false, false, 90, 0.4f},
      {0, false, false, 0, 0.5f},     {41, false, false, 100, 0.5f},  {41, true, false, 115, 0.6f},  {0, false, false, 0, 0.5f},
      {0, false, false, 0, 0.5f},     {44, false, false, 100, 0.5f},  {41, false, false, 110, 0.6f}, {41, false, false, 90, 0.4f},
      {0, false, false, 0, 0.5f},     {39, false, false, 100, 0.5f},  {36, true, false, 115, 0.8f},  {48, false, false, 90, 0.4f},
  };
  playPattern(rig, pat, 124.0f, 4, [](int, float) {});
  finish(rig, "house_pluck_124bpm");
}
// ---- presets: (param, value) overrides on top of the defaults ------------------
struct Preset { const char* name; std::vector<std::pair<int, int>> v; };
void apply(Rig& rig, const Preset& p) {
  rig.defaults();
  for (auto& kv : p.v) rig.set(kv.first, kv.second);
}
// Documented in the bass report; keep in sync.
const Preset kGrowl = {"growl", {{kCutoff, 34}, {kReso, 70}, {kEnvMod, 30}, {kDecay, 70}, {kAccent, 60}, {kDrive, 85},
                                 {kLfoDepth, 62}, {kLfoRate, 7}, {kWave, 20}, {kSub, 90}, {kFmMix, 70}, {kFmDepth, 70},
                                 {kFmRatio, 1}, {kFold, 60}, {kTimbreEnv, 20}, {kLfoShape, 1}, {kAmpDecay, 127},
                                 {kRelease, 40}, {kKeyTrack, 70}, {kLfoTimbre, 70}, {kSubOct, 1}}};
const Preset kDonk = {"donk", {{kCutoff, 62}, {kReso, 112}, {kEnvMod, 40}, {kDecay, 12}, {kAccent, 90}, {kDrive, 45},
                               {kWave, 96}, {kSub, 40}, {kFmMix, 40}, {kFmDepth, 50}, {kFmRatio, 4}, {kTimbreEnv, 127},
                               {kAmpDecay, 26}, {kRelease, 30}, {kKeyTrack, 100}}};
const Preset kSubBass = {"sub", {{kCutoff, 0}, {kReso, 0}, {kEnvMod, 0}, {kDrive, 0}, {kSub, 127}, {kAmpDecay, 110},
                                 {kRelease, 40}, {kKeyTrack, 0}}};
const Preset kAcidSquelch = {"acid", {{kCutoff, 30}, {kReso, 112}, {kEnvMod, 95}, {kDecay, 50}, {kDrive, 50}, {kSub, 25}}};
const Preset kReeseFold = {"reese", {{kCutoff, 60}, {kReso, 40}, {kEnvMod, 20}, {kDecay, 90}, {kDrive, 70}, {kWave, 0},
                                     {kSub, 100}, {kFold, 100}, {kTimbreEnv, 0}, {kAmpDecay, 127}, {kKeyTrack, 60},
                                     {kLfoDepth, 25}, {kLfoRate, 2}, {kLfoTimbre, 60}}};
const Preset kFmMetal = {"fm_metal", {{kCutoff, 90}, {kReso, 50}, {kEnvMod, 30}, {kDecay, 40}, {kSub, 70}, {kFmMix, 127},
                                      {kFmDepth, 100}, {kFmRatio, 5}, {kTimbreEnv, 110}, {kAmpDecay, 70}, {kKeyTrack, 80}}};
const Preset kSelfOsc = {"selfosc", {{kCutoff, 50}, {kReso, 127}, {kEnvMod, 60}, {kDecay, 60}, {kDrive, 40}, {kSub, 20},
                                     {kAmpDecay, 127}}};

void growlRiff() {
  Rig rig(44100.0f, 126.0f);
  apply(rig, kGrowl);
  // F minor, long notes with slides; the LFO division changes every 2 bars.
  const std::vector<StepEv> pat = {
      {29, true, false, 120, 0.9f},  {0, false, false, 0, 0.5f},     {29, false, false, 100, 0.5f}, {0, false, false, 0, 0.5f},
      {29, false, true, 110, 1.0f},  {32, false, false, 110, 1.0f},  {0, false, false, 0, 0.5f},    {29, false, false, 100, 0.5f},
      {41, true, false, 120, 0.5f},  {0, false, false, 0, 0.5f},     {29, false, true, 110, 1.0f},  {31, false, true, 110, 1.0f},
      {32, false, false, 110, 1.0f}, {32, false, false, 110, 1.0f},  {27, false, false, 110, 0.9f}, {0, false, false, 0, 0.5f},
  };
  const int rates[] = {7, 9, 8, 4};   // 1/8, 1/16, 1/8T, 1/4
  playPattern(rig, pat, 126.0f, 8, [&](int step, float) { rig.set(kLfoRate, rates[(step / 32) % 4]); });
  finish(rig, "growl_riff_126bpm");
}

void subLine() {
  Rig rig(44100.0f, 124.0f);
  apply(rig, kSubBass);
  const std::vector<StepEv> pat = {
      {26, false, false, 115, 0.8f}, {0, false, false, 0, 0.5f}, {0, false, false, 0, 0.5f}, {26, false, false, 90, 0.5f},
      {0, false, false, 0, 0.5f},    {0, false, false, 0, 0.5f}, {29, false, false, 110, 0.8f}, {0, false, false, 0, 0.5f},
      {0, false, false, 0, 0.5f},    {26, false, false, 100, 0.5f}, {0, false, false, 0, 0.5f}, {24, false, true, 110, 1.0f},
      {26, false, false, 115, 1.0f}, {0, false, false, 0, 0.5f}, {33, false, false, 100, 0.6f}, {31, false, false, 90, 0.5f},
  };
  playPattern(rig, pat, 124.0f, 4, [](int, float) {});
  finish(rig, "sub_line_124bpm");
}

// Single-note timbre demos: four notes (A1, D2, A2, D3), held 0.6 s each.
void demo(const Preset& p, bool accentLast = false) {
  Rig rig(44100.0f, 126.0f);
  apply(rig, p);
  const int notes[] = {33, 38, 45, 50};
  for (int i = 0; i < 4; ++i) {
    rig.on(notes[i], 120, accentLast && i == 3);
    rig.runSeconds(0.6f);
    rig.off(notes[i]);
    rig.runSeconds(0.15f);
  }
  finish(rig, (std::string("demo_") + p.name).c_str());
}

// The acid line again at 32 kHz (the CPU fallback rate).
void acidLine32k() {
  Rig rig(32000.0f, 126.0f);
  rig.set(kSub, 30);
  playPattern(rig, kAcidLine, 126.0f, 2, [&](int, float f) {
    rig.set(kCutoff, int(30.0f + 50.0f * f));
    rig.set(kReso, 110);
  });
  finish(rig, "acid_line_32k");
}
}  // namespace

int main() {
  ensureDir(kDir);
  acidLine();
  houseBass();
  growlRiff();
  subLine();
  acidLine32k();
  demo({"pluck_default", {}});
  demo(kAcidSquelch, true);
  demo(kDonk);
  demo(kGrowl);
  demo(kReeseFold);
  demo(kFmMetal);
  demo(kSubBass);
  demo(kSelfOsc);
  return ck.result("test_bass_render");
}
