// System tests on the full instrument (8 real voices + engine):
//   1. busy pattern, 4 scenes (stereo / mono device x 44.1 / 32 kHz): finite
//      (no engine faults), bounded, no int16 wrap, limiter not pinned,
//      deterministic for a given seed and different for another seed;
//   2. every param of every voice (and the mixer) p-locked on steps, at
//      min, max and random values: no NaN / absurd output anywhere;
//   3. mono device mode == (L+R)/2 of stereo for centred sources, and L == R.
#include <vector>

#include "common/grooves.h"
#include "itests/itest_util.h"

using namespace pg;

namespace {

Instrument g_inst;

struct Scene { const char* name; float sr; bool mono; };
const Scene kScenes[] = {{"stereo 44.1k", 44100.0f, false}, {"mono 44.1k", 44100.0f, true},
                         {"stereo 32k", 32000.0f, false}, {"mono 32k", 32000.0f, true}};

it::Stats runBusy(const Scene& sc, uint32_t seed, double seconds) {
  g_inst.init(sc.sr, sc.mono);
  grooves::programBusy(g_inst.project());
  engine_detail::setRngSeed(seed);
  it::post(g_inst, CmdType::Play);
  it::Stats st;
  it::render(g_inst, long(seconds * double(sc.sr)) / kQuantum * kQuantum, st);
  return st;
}

void testBusy() {
  for (const Scene& sc : kScenes) {
    const uint32_t f0 = engine_detail::faultCount();
    const it::Stats a = runBusy(sc, 1234u, 20.0);
    const uint32_t faults = engine_detail::faultCount();
    const it::Stats b = runBusy(sc, 1234u, 20.0);
    const it::Stats c = runBusy(sc, 98765u, 20.0);
    std::printf("  busy %-12s peak %6.2f dBFS  rms %6.1f dBFS  max step %5d  full-scale samples %ld  faults %u\n", sc.name,
                a.peakDb(), a.rmsDb(), a.maxJump, a.fullScale, faults - f0);
    it::check(faults == 0 && f0 == 0, "%s: engine faults %u (non-finite / absurd signal)", sc.name, faults - f0);
    it::check(a.peak > 3000, "%s: busy pattern is (nearly) silent: peak %d", sc.name, a.peak);
    it::check(a.minInt == 0, "%s: %ld samples at -32768 (int16 wrap)", sc.name, a.minInt);
    it::check(a.maxJump < 50000, "%s: sample step %d in a musical pattern looks like a wrap", sc.name, a.maxJump);
    it::check(double(a.fullScale) < 1e-3 * double(2 * a.frames), "%s: limiter pinned at full scale (%ld samples)", sc.name, a.fullScale);
    it::check(a.hash == b.hash, "%s: not deterministic for the same seed", sc.name);
    it::check(a.hash != c.hash, "%s: a different seed changed nothing (probability steps ignored?)", sc.name);
    if (sc.mono) it::check(a.lrDiffer == 0, "%s: mono device mode has %ld frames with L != R", sc.name, a.lrDiffer);
  }
}

// Every existing param index of every track (sound 0..23 where the voice has
// it, mixer 24..28) gets locked on a step, at min / max / random values.
void testLockEverything() {
  for (const Scene& sc : {kScenes[0], kScenes[3]}) {
    for (int pass = 0; pass < 3; ++pass) {
      g_inst.init(sc.sr, sc.mono);
      Project& p = g_inst.project();
      p.globals[kGTempo] = 170;
      p.patterns[0].length = 64;
      it::Rng rng(77u + uint32_t(pass));
      int locks = 0;
      for (int t = 0; t < kNumTracks; ++t) {
        int idx[kParamsPerTrack], n = 0;
        for (int i = 0; i < kParamsPerTrack; ++i)
          if (g_inst.engine().trackParamInfo(t, i)) idx[n++] = i;
        TrackPattern& tp = p.patterns[0].tracks[t];
        tp.length = 64;
        for (int s = 0; s < 64; ++s) {
          const int i = idx[(s + pass * 7) % n];
          const ParamInfo* info = g_inst.engine().trackParamInfo(t, i);
          Step st = grooves::on(t == kTrackBass ? 30 + rng.range(0, 30) : 36 + rng.range(0, 60), rng.range(1, 127),
                                rng.range(1, 12), uint8_t(rng.range(0, 3) == 0 ? kStepAccent : 0));
          if (t >= kTrackBass && rng.range(0, 3) == 0) st.flags |= kStepSlide;
          if (rng.range(0, 7) == 0) st.flags |= uint8_t(rng.range(1, 3) << kStepRatchetShift);
          if (t == kTrackLead) st.chord = uint8_t(rng.range(0, kChordNum - 1));
          st.lockParam = uint8_t(i + 1);
          st.lockValue = pass == 0 ? info->min : (pass == 1 ? info->max : uint8_t(rng.range(info->min, info->max)));
          tp.steps[s] = st;
          ++locks;
        }
      }
      const uint32_t f0 = engine_detail::faultCount();
      it::post(g_inst, CmdType::Play);
      it::Stats st;
      const double stepSec = 15.0 / 170.0;
      it::render(g_inst, long(2.0 * 64.0 * stepSec * double(sc.sr)) / kQuantum * kQuantum, st);
      it::post(g_inst, CmdType::Stop);
      it::render(g_inst, long(2.0 * double(sc.sr)) / kQuantum * kQuantum, st);
      const uint32_t faults = engine_detail::faultCount() - f0;
      std::printf("  p-lock %-12s pass %d (%s): %d locked steps, peak %6.2f dBFS, faults %u\n", sc.name, pass,
                  pass == 0 ? "min" : (pass == 1 ? "max" : "random"), locks, st.peakDb(), faults);
      it::check(faults == 0, "p-lock %s pass %d: %u engine faults (a voice went non-finite or absurd)", sc.name, pass, faults);
      // Extreme settings legitimately make full-scale square-ish waves (big
      // steps); a wrap would show as -32768, which the master never emits.
      it::check(st.minInt == 0, "p-lock %s pass %d: %ld samples at -32768 (int16 wrap)", sc.name, pass, st.minInt);
      it::check(st.peak > 1000, "p-lock %s pass %d: silent", sc.name, pass);
    }
  }
}

// Mono device mode against the stereo render's (L+R)/2, all tracks centred
// (pan is ignored in mono). Master volume pulled down so the limiter's knee
// (the one stage that acts differently on L/R than on their sum) stays out.
void testMonoEquivalence() {
  for (float sr : {44100.0f, 32000.0f}) {
    for (int vol : {80, 100}) {
      std::vector<int16_t> st[2];
      long frames = 0;
      for (int m = 0; m < 2; ++m) {
        g_inst.init(sr, m == 1);
        Project& p = g_inst.project();
        grooves::programGroove(p);
        for (int t = 0; t < kNumTracks; ++t) p.tracks[t].params[kMixPan] = 64;
        p.globals[kGMasterVolume] = uint8_t(vol);
        engine_detail::setRngSeed(4242u);
        it::post(g_inst, CmdType::Play);
        frames = long(8.0 * 16.0 * double(sr) * 15.0 / 124.0) / kQuantum * kQuantum;
        st[m].resize(size_t(frames) * 2);
        it::Stats s;
        it::render(g_inst, frames, s, st[m].data());
        if (m == 1) it::check(s.lrDiffer == 0, "mono %.0f: L != R in %ld frames", double(sr), s.lrDiffer);
      }
      double maxErr = 0.0, sumErr2 = 0.0, sumSig2 = 0.0;
      for (long f = 0; f < frames; ++f) {
        const double ref = 0.5 * (double(st[0][size_t(2 * f)]) + double(st[0][size_t(2 * f + 1)]));
        const double e = double(st[1][size_t(2 * f)]) - ref;
        maxErr = std::fmax(maxErr, std::fabs(e));
        sumErr2 += e * e;
        sumSig2 += ref * ref;
      }
      const double errDb = 10.0 * std::log10(sumErr2 / (sumSig2 + 1e-9) + 1e-30);
      std::printf("  mono vs stereo (L+R)/2, %.0f Hz, volume %d: max error %.1f LSB, error %.1f dB re signal\n", double(sr), vol,
                  maxErr, errDb);
      if (vol == 80) {
        it::check(maxErr <= 8.0, "mono %.0f vol %d: max error %.1f LSB vs (L+R)/2", double(sr), vol, maxErr);
        it::check(errDb < -70.0, "mono %.0f vol %d: error %.1f dB re signal", double(sr), vol, errDb);
      } else {
        // At unity volume the loudest hits touch the limiter knee, which acts on
        // L and R separately in stereo: still close, not bit-near.
        it::check(errDb < -40.0, "mono %.0f vol %d: error %.1f dB re signal", double(sr), vol, errDb);
      }
    }
  }
}

// ParamInfo tables of every voice + mixer + globals: names <= 4 chars, long
// names <= 12, enum names <= 5 (contract v2), defaults inside min..max.
void testTables() {
  g_inst.init(44100.0f, false);
  int bad = 0;
  auto checkInfo = [&](const ParamInfo* p, const char* where, int idx) {
    bool ok = p->name && std::strlen(p->name) <= 4 && p->longName && std::strlen(p->longName) <= 12 && p->min <= p->def && p->def <= p->max;
    if (p->fmt == ParamFormat::Enum) {
      ok &= p->enumNames != nullptr;
      for (int v = p->min; ok && v <= p->max; ++v) ok &= p->enumNames[v - p->min] && std::strlen(p->enumNames[v - p->min]) <= 5;
    }
    if (!ok && ++bad < 6) std::printf("    bad ParamInfo: %s index %d (%s)\n", where, idx, p->name ? p->name : "?");
  };
  for (int t = 0; t < kNumTracks; ++t)
    for (int i = 0; i < kParamsPerTrack; ++i)
      if (const ParamInfo* p = g_inst.engine().trackParamInfo(t, i)) checkInfo(p, "track", t * 100 + i);
  for (int i = 0; i < kGNumUsed; ++i) checkInfo(Engine::globalParamInfo(i), "global", i);
  const ParamInfo* l7 = g_inst.engine().trackParamInfo(kTrackLead, 7);
  const ParamInfo* l9 = g_inst.engine().trackParamInfo(kTrackLead, 9);
  it::check(l7 && l9 && !std::strcmp(l7->name, "DCY") && !std::strcmp(l9->name, "WID"), "lead page 1 must end with DCY, WID at index 9");
  it::check(bad == 0, "%d ParamInfo entries break the naming / range rules", bad);
}

}  // namespace

int main() {
  testTables();
  std::printf("== busy pattern: bounded, finite, deterministic ==\n");
  testBusy();
  std::printf("== every param p-locked ==\n");
  testLockEverything();
  std::printf("== mono device mode vs stereo ==\n");
  testMonoEquivalence();
  return it::finish("itest_full_instrument");
}
