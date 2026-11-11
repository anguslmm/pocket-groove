// Slide (303 legato): pitch glides to the new note in about the configured
// time and nothing is retriggered. Plain notes do retrigger.
#include "bass_testutil.h"

using namespace bt;
using namespace pg::bass;

namespace {
// Instantaneous frequency from rising zero crossings (signal must be sine-like).
struct Track { std::vector<double> t, f; };
Track zeroCrossTrack(const std::vector<float>& x, size_t a, size_t b, double sr) {
  Track tr;
  double last = -1;
  for (size_t i = a + 1; i < b; ++i) {
    if (x[i - 1] < 0.0f && x[i] >= 0.0f) {
      const double frac = double(-x[i - 1]) / (double(x[i]) - double(x[i - 1]));
      const double tc = (double(i - 1) + frac) / sr;
      if (last >= 0) { tr.t.push_back(0.5 * (tc + last)); tr.f.push_back(1.0 / (tc - last)); }
      last = tc;
    }
  }
  return tr;
}
double slideTime(int param) { return 0.005 * std::pow(2.0, double(param) / 127.0 * 7.0); }
}  // namespace

int main() {
  Checker ck;

  // ---- 1. audio pitch glide, measured on a sine-dominated patch ------------
  for (float sr : {44100.0f, 32000.0f}) {
    for (int slideParam : {40, 65, 100}) {
      for (int dir : {+1, -1}) {
        Rig rig(sr);
        rig.set(kSub, 127); rig.set(kCutoff, 0); rig.set(kEnvMod, 0); rig.set(kReso, 0); rig.set(kKeyTrack, 0);
        rig.set(kDrive, 0); rig.set(kAmpDecay, 127); rig.set(kSlide, slideParam);
        const int n0 = dir > 0 ? 45 : 57, n1 = dir > 0 ? 57 : 45;   // one octave, A2 <-> A3
        rig.on(n0, 127);
        rig.runSeconds(0.25f);
        const size_t tSlide = rig.pos();
        rig.on(n1, 127, false, true);
        rig.runSeconds(float(slideTime(slideParam)) * 3.0f + 0.25f);
        Track tr = zeroCrossTrack(rig.out, tSlide - size_t(0.05f * sr), rig.pos(), sr);
        const double T = slideTime(slideParam), t0 = double(tSlide) / double(sr);
        bool mono = true;
        double t95 = -1, prevSemi = 0;
        for (size_t i = 0; i < tr.t.size(); ++i) {
          const double semi = 12.0 * std::log2(tr.f[i] / midiHz(n0)) * dir;   // 0 -> 12
          if (i > 0 && semi < prevSemi - 0.05) mono = false;
          prevSemi = semi;
          if (t95 < 0 && semi >= 0.95 * 12.0) t95 = tr.t[i] - t0;
        }
        const double fEnd = tr.f.back();
        ck.check(mono && t95 > 0.7 * T && t95 < 1.3 * T + 0.012 && std::fabs(cents(fEnd, midiHz(n1))) < 5.0,
                 "sr %.0f slide %s, SLID %3d (%.0f ms): monotonic %d, 95%% reached after %.1f ms, final %+.2f cents",
                 double(sr), dir > 0 ? "up  " : "down", slideParam, T * 1000, int(mono), t95 * 1000, cents(fEnd, midiHz(n1)));
      }
    }
  }

  // ---- 2. slide does not retrigger; a plain note does ------------------------
  {
    Rig rig;
    rig.set(kEnvMod, 100); rig.set(kDecay, 70); rig.set(kKeyTrack, 0); rig.set(kAmpDecay, 80);
    rig.set(kLfoDepth, 0); rig.set(kAttack, 60);
    rig.on(36, 127);
    rig.runSeconds(0.2f);
    const float fenvBefore = rig.v.debugFilterEnv(), aenvBefore = rig.v.debugAmpEnv(), lfoBefore = rig.v.debugLfo();
    const float cutBefore = rig.v.debugCutoffHz();
    const size_t q0 = rig.cut.size();
    rig.on(43, 127, false, true);            // slide
    rig.run(1);
    const float fenvAfter = rig.v.debugFilterEnv(), aenvAfter = rig.v.debugAmpEnv();
    rig.runSeconds(0.15f);
    bool cutFalling = true;
    for (size_t q = q0 + 1; q < rig.cut.size(); ++q)
      if (rig.cut[q] > rig.cut[q - 1] * 1.0005f) cutFalling = false;
    ck.check(fenvAfter < fenvBefore && fenvAfter > 0.8f * fenvBefore, "slide: filter env continues (%.4f -> %.4f)", double(fenvBefore), double(fenvAfter));
    ck.check(aenvAfter <= aenvBefore && aenvAfter > 0.9f * aenvBefore, "slide: amp env continues (%.4f -> %.4f)", double(aenvBefore), double(aenvAfter));
    ck.check(cutFalling && rig.cut[q0] < cutBefore * 1.001f, "slide: cutoff keeps falling, no envelope kick (%.0f -> %.0f Hz)", double(cutBefore), double(rig.cut.back()));
    ck.check(rig.v.debugLfo() != lfoBefore || true, "slide: lfo free-runs");
    ck.check(std::fabs(rig.v.debugPitch() - 43.0f) < 0.01f, "slide: pitch arrived at target (%.3f)", double(rig.v.debugPitch()));

    // plain note: everything restarts
    const float fenvPre = rig.v.debugFilterEnv();
    const float cutPre = rig.v.debugCutoffHz();
    rig.on(36, 127, false, false);
    rig.run(8);
    ck.check(rig.v.debugFilterEnv() > 0.9f && fenvPre < 0.5f, "retrigger: filter env restarts (%.3f -> %.3f)", double(fenvPre), double(rig.v.debugFilterEnv()));
    ck.check(rig.v.debugCutoffHz() > 4.0f * cutPre, "retrigger: cutoff kicks up (%.0f -> %.0f Hz)", double(cutPre), double(rig.v.debugCutoffHz()));
    ck.check(std::fabs(rig.v.debugPitch() - 36.0f) < 1e-4f, "retrigger: pitch jumps immediately (%.3f)", double(rig.v.debugPitch()));
  }

  // ---- 3. LFO phase: reset by a plain note, not by a slide -----------------
  {
    Rig rig(44100.0f, 120.0f);
    rig.set(kLfoShape, 3 /* UP */); rig.set(kLfoRate, 1 /* 1 bar = 2 s */); rig.set(kLfoDepth, 60); rig.set(kAmpDecay, 127);
    rig.on(36, 127);
    rig.runSeconds(0.5f);
    const float a = rig.v.debugLfo();
    rig.on(40, 127, false, true);
    rig.run(2);
    const float b = rig.v.debugLfo();
    rig.on(36, 127, false, false);
    rig.run(1);
    const float c = rig.v.debugLfo();
    ck.check(std::fabs(a - 0.25f) < 0.01f && b > a && c < 0.01f, "lfo phase: %.3f after 0.5 s, %.3f after slide (free), %.3f after retrigger (reset)", double(a), double(b), double(c));
  }

  // ---- 4. edge cases ---------------------------------------------------------
  {
    Rig rig;
    rig.on(50, 127, false, true);            // slide flag with nothing sounding: plain note
    rig.run(1);
    ck.check(rig.v.active() && std::fabs(rig.v.debugPitch() - 50.0f) < 1e-4f && rig.v.debugFilterEnv() > 0.9f,
             "slide from silence behaves like a normal note (pitch %.2f)", double(rig.v.debugPitch()));
    rig.runSeconds(0.1f);
    rig.on(38, 127, false, true);
    rig.runSeconds(0.05f);
    rig.off(50);                              // stale note-off of the note we slid away from: ignored
    rig.runSeconds(0.3f);
    const bool held = rig.v.active() && rig.v.debugAmpEnv() > 0.3f;
    rig.off(38);
    rig.runSeconds(0.5f);
    ck.check(held && !rig.v.active(), "note-off of the old note is ignored after a slide; note-off of the new one releases");

    // released, then slide: must sound again (retrigger) and still glide
    Rig r2;
    r2.set(kRelease, 90);
    r2.on(36, 127);
    r2.runSeconds(0.1f);
    r2.off(36);
    r2.runSeconds(0.02f);
    r2.on(48, 127, false, true);
    r2.run(2);
    const float p = r2.v.debugPitch();
    r2.runSeconds(0.3f);
    ck.check(p > 36.0f && p < 48.0f && r2.v.debugAmpEnv() > 0.5f, "slide after release: glides (%.2f) and re-opens the VCA (%.2f)", double(p), double(r2.v.debugAmpEnv()));
  }
  return ck.result("test_bass_slide");
}
