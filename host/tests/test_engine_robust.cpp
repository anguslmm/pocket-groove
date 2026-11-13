// Robustness: every global and every mixer param at its min and max, at
// 44100 and 32000, with all 8 tracks busy and hot. Afterwards every param goes
// back to its default and the voices fall silent: the output must decay to
// exact digital silence (any NaN/inf or blown-up state inside the delay,
// reverb, ducker or master filter would stay stuck instead). Plus all-min /
// all-max, random combinations under per-quantum automation, and a voice that
// emits NaN.
#include <cinttypes>

#include "tests/engine_test_util.h"

using namespace et;
using namespace pg;

static Rig rig;

static void busyScene(float sr) {
  rig.init(sr, 124);
  for (int t = 0; t < kNumTracks; ++t) {
    rig.v[t].log = false;
    rig.v[t].amp = 0.45f;
    rig.v[t].mode = StubVoice::Blip;
    rig.v[t].decaySec = 0.08f;
    for (int s = 0; s < 16; ++s)
      if ((s + t) % (t == kTrackKick ? 4 : 2) == 0) {
        Step st = mkStep(30 + 9 * t + s, kStepOn | (s % 5 == 0 ? (1 << kStepRatchetShift) : 0) | (s % 3 == 0 ? kStepSlide : 0), 120, 3);
        if (s == 6) { st.setLock(kMixPan + 1, uint8_t(s * 8)); }
        rig.step(0, t, s) = st;
      }
  }
  rig.v[kTrackBass].mode = StubVoice::Tone;    // sustained, so the ducker and drive always have something to chew on
  rig.v[kTrackBass].freq = 55.0f;
  rig.v[kTrackLead].mode = StubVoice::Noise;
  rig.v[kTrackLead].amp = 0.3f;
  for (int t = 0; t < kNumTracks; ++t) {
    rig.proj.tracks[t].params[kMixDelaySend] = 90;
    rig.proj.tracks[t].params[kMixReverbSend] = 90;
  }
}

static void restoreDefaults() {
  for (int i = 0; i < kGNumUsed; ++i) {
    const ParamInfo* info = Engine::globalParamInfo(i);
    rig.post(CmdType::SetGlobal, i, 0, i == kGTempo ? 124 : info->def);
  }
  for (int t = 0; t < kNumTracks; ++t)
    for (int p = kMixLevel; p <= kMixDuck; ++p) rig.post(CmdType::SetTrackParam, t, p, rig.eng.trackParamInfo(t, p)->def);
}

// After the scene: silence, defaults, wait for the tails; the output must be
// exactly zero and the send FX idle.
static bool settlesToSilence(const char* what) {
  rig.post(CmdType::Stop);
  restoreDefaults();
  for (int t = 0; t < kNumTracks; ++t) rig.v[t].mode = StubVoice::Silent;
  const bool cap = rig.capture;
  rig.capture = true;
  rig.out.clear();
  double waited = 0;
  while (!engine_detail::fxIdle() && waited < 60.0) { rig.runSeconds(0.5); waited += 0.5; rig.out.clear(); }
  rig.runSeconds(0.2);
  bool zero = true;
  for (int16_t x : rig.out) zero &= x == 0;
  rig.capture = cap;
  CHECKF(zero && engine_detail::fxIdle(), "%s: output did not return to digital silence (waited %.1f s, idle %d)", what, waited, int(engine_detail::fxIdle()));
  return zero;
}

static bool healthy(const char* what) {
  const EngineStatus st = rig.eng.status();
  const bool ok = std::isfinite(st.peakL) && std::isfinite(st.peakR) && st.peakL <= 1.0f && st.peakR <= 1.0f && std::isfinite(st.duckGain) &&
                  st.duckGain >= 0.0f && st.duckGain <= 1.0f && engine_detail::faultCount() == 0;
  CHECKF(ok, "%s: peak %.3f/%.3f duck %.3f faults %u", what, double(st.peakL), double(st.peakR), double(st.duckGain), engine_detail::faultCount());
  return ok;
}

static void testExtremes(float sr) {
  int cases = 0;
  char what[96];
  // Globals, one at a time.
  for (int g = 0; g < kGNumUsed; ++g) {
    const ParamInfo* info = Engine::globalParamInfo(g);
    for (int v : {int(info->min), int(info->max)}) {
      busyScene(sr);
      rig.proj.globals[g] = uint8_t(v);
      if (g != kGDuckSource) rig.proj.globals[kGDuckSource] = 2;
      rig.eng.init(sr, &rig.proj);
      rig.post(CmdType::Play);
      rig.runSeconds(1.5);
      std::snprintf(what, sizeof(what), "sr %.0f global %s = %d", double(sr), info->name, v);
      healthy(what);
      settlesToSilence(what);
      ++cases;
    }
  }
  // Mixer params, one at a time on every track.
  for (int t = 0; t < kNumTracks; ++t) {
    for (int p = kMixLevel; p <= kMixDuck; ++p) {
      const ParamInfo* info = rig.eng.trackParamInfo(t, p);
      for (int v : {int(info->min), int(info->max)}) {
        busyScene(sr);
        rig.proj.tracks[t].params[p] = uint8_t(v);
        rig.proj.globals[kGDuckSource] = 2;
        rig.eng.init(sr, &rig.proj);
        rig.post(CmdType::Play);
        rig.runSeconds(0.7);
        std::snprintf(what, sizeof(what), "sr %.0f track %d %s = %d", double(sr), t, info->name, v);
        healthy(what);
        settlesToSilence(what);
        ++cases;
      }
    }
  }
  // Everything at min, everything at max (globals and all mixer params).
  for (int extreme = 0; extreme < 2; ++extreme) {
    busyScene(sr);
    for (int g = 0; g < kGNumUsed; ++g) rig.proj.globals[g] = extreme ? Engine::globalParamInfo(g)->max : Engine::globalParamInfo(g)->min;
    for (int t = 0; t < kNumTracks; ++t)
      for (int p = kMixLevel; p <= kMixDuck; ++p) rig.proj.tracks[t].params[p] = extreme ? 127 : 0;
    rig.eng.init(sr, &rig.proj);
    rig.post(CmdType::Play);
    rig.runSeconds(3.0);
    std::snprintf(what, sizeof(what), "sr %.0f everything at %s", double(sr), extreme ? "max" : "min");
    healthy(what);
    settlesToSilence(what);
    cases += 1;
  }
  // Random combinations, and every param automated every quantum.
  uint32_t lcg = 0xC0FFEEu;
  auto rnd = [&](int n) { lcg = lcg * 1664525u + 1013904223u; return int((lcg >> 8) % uint32_t(n)); };
  busyScene(sr);
  rig.eng.init(sr, &rig.proj);
  rig.post(CmdType::Play);
  for (int q = 0; q < int(sr / kQuantum) * 12; ++q) {
    // a few param moves per quantum, biased to the extremes
    for (int k = 0; k < 3; ++k) {
      if (rnd(3) == 0) {
        const int g = rnd(kGNumUsed);
        const ParamInfo* info = Engine::globalParamInfo(g);
        const int r = rnd(4);
        rig.post(CmdType::SetGlobal, g, 0, r == 0 ? info->min : (r == 1 ? info->max : info->min + rnd(info->max - info->min + 1)));
      } else {
        const int t = rnd(kNumTracks), p = kMixLevel + rnd(5), r = rnd(4);
        rig.post(CmdType::SetTrackParam, t, p, r == 0 ? 0 : (r == 1 ? 127 : rnd(128)));
      }
    }
    rig.run(1);
  }
  std::snprintf(what, sizeof(what), "sr %.0f random automation", double(sr));
  healthy(what);
  settlesToSilence(what);
  ++cases;
  std::printf("  sr %.0f: %d extreme / combination scenes, all finite, bounded, and back to digital silence\n", double(sr), cases);
}

static void testNanVoice() {
  // A voice that emits NaN is choked and counted; the rest of the mix carries on
  // and nothing downstream is poisoned.
  rig.init();
  rig.neutralMix();
  rig.v[kTrackSnare].mode = StubVoice::Dc;
  rig.v[kTrackSnare].amp = 0.25f;
  rig.v[kTrackPerc].mode = StubVoice::Nan;
  rig.proj.tracks[kTrackPerc].params[kMixReverbSend] = 127;
  rig.proj.tracks[kTrackPerc].params[kMixDelaySend] = 127;
  rig.eng.init(44100.0f, &rig.proj);
  rig.capture = true;
  rig.runSeconds(0.5);
  rig.capture = false;
  CHECK(engine_detail::faultCount() >= 1);
  CHECK(!rig.v[kTrackPerc].chokes().empty());
  CHECK(std::fabs(double(rig.sample(rig.frames() - 1, 0)) - 0.25) < 0.002);
  const EngineStatus st = rig.eng.status();
  CHECK(std::isfinite(st.peakL) && st.peakL < 0.3f);
  std::printf("  NaN voice: choked (%u faults), rest of the mix intact\n", engine_detail::faultCount());
}

int main() {
  testExtremes(44100.0f);
  testExtremes(32000.0f);
  testNanVoice();
  return finish("test_engine_robust");
}
