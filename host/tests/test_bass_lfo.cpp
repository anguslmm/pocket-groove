// The LFO follows the tempo: measured on the audio (brightness wobble) and on
// the cutoff trace, for several divisions and tempi.
#include "bass_testutil.h"

using namespace bt;
using namespace pg::bass;

namespace {
// Dominant modulation frequency of a control-rate series (mean removed), by DTFT scan.
double dominantHz(const std::vector<float>& s, double rate, double expect) {
  std::vector<float> x(s);
  double m = 0;
  for (float v : x) m += double(v);
  m /= double(x.size());
  for (float& v : x) v = float(double(v) - m);
  return findPeakFreq(x, 0, x.size(), expect, rate, 7.0);   // search +-7 semitones (covers a wrong division)
}
}  // namespace

int main() {
  Checker ck;
  const float beats[] = {8.0f, 4.0f, 2.0f, 1.5f, 1.0f, 0.75f, 2.0f / 3.0f, 0.5f, 1.0f / 3.0f, 0.25f, 1.0f / 6.0f, 0.125f};
  for (float sr : {44100.0f, 32000.0f}) {
    for (float bpm : {100.0f, 126.0f, 174.0f}) {
      for (int rate : {1, 3, 4, 5, 7, 8, 9, 11}) {
        for (int shape : {0, 1, 2, 4}) {
          if (shape != 0 && !(rate == 7 && bpm == 126.0f)) continue;   // all shapes only for one rate
          Rig rig(sr, bpm);
          rig.set(kLfoDepth, 70); rig.set(kLfoRate, rate); rig.set(kLfoShape, shape);
          rig.set(kCutoff, 30); rig.set(kEnvMod, 0); rig.set(kReso, 60); rig.set(kSub, 0); rig.set(kAmpDecay, 127);
          rig.set(kKeyTrack, 0);
          rig.on(33, 127);
          const double expect = double(bpm) / 60.0 / double(beats[rate]);
          const double secs = std::fmax(6.0, 6.0 / expect);
          rig.runSeconds(float(secs));
          // brightness per quantum = energy of the first difference
          std::vector<float> bright;
          for (size_t q = 0; q + 1 < rig.out.size() / pg::kQuantum; ++q) {
            double e = 0;
            for (int i = 1; i < pg::kQuantum; ++i) {
              const double d = double(rig.out[q * pg::kQuantum + size_t(i)]) - double(rig.out[q * pg::kQuantum + size_t(i) - 1]);
              e += d * d;
            }
            bright.push_back(float(std::sqrt(e)));
          }
          const double ctl = double(sr) / pg::kQuantum;
          const double fAudio = dominantHz(bright, ctl, expect);
          const double fCut = dominantHz(rig.cut, ctl, expect);
          const bool ok = std::fabs(fAudio / expect - 1.0) < 0.02 && std::fabs(fCut / expect - 1.0) < 0.02;
          if (!ok || shape != 0 || (bpm == 126.0f) )
            ck.check(ok, "sr %.0f bpm %3.0f rate %-5s shape %d: expect %.4f Hz, cutoff wobble %.4f Hz, audio brightness %.4f Hz",
                     double(sr), double(bpm), rig.v.paramInfo(kLfoRate)->enumNames[rate], shape, expect, fCut, fAudio);
          else if (!ok) ck.check(false, "lfo mismatch");
        }
      }
    }
  }
  // setTempo while running changes the rate
  {
    Rig rig(44100.0f, 120.0f);
    rig.set(kLfoDepth, 70); rig.set(kLfoRate, 4); rig.set(kAmpDecay, 127); rig.set(kEnvMod, 0);
    rig.on(33, 127);
    rig.runSeconds(4.0f);
    std::vector<float> a(rig.cut);
    rig.cut.clear();
    rig.v.setTempo(150.0f);
    rig.runSeconds(4.0f);
    const double ctl = 44100.0 / pg::kQuantum;
    const double f1 = dominantHz(a, ctl, 2.0), f2 = dominantHz(rig.cut, ctl, 2.5);
    ck.check(std::fabs(f1 - 2.0) < 0.03 && std::fabs(f2 - 2.5) < 0.03, "setTempo 120 -> 150: 1/4 LFO %.3f Hz -> %.3f Hz", f1, f2);
  }
  // depth 0 = no movement
  {
    Rig rig;
    rig.set(kLfoDepth, 0); rig.set(kEnvMod, 0); rig.set(kAmpDecay, 127);
    rig.on(33, 127);
    rig.runSeconds(1.0f);
    float lo = 1e9f, hi = 0;
    for (size_t q = 200; q < rig.cut.size(); ++q) { lo = std::fmin(lo, rig.cut[q]); hi = std::fmax(hi, rig.cut[q]); }
    ck.check(hi / lo < 1.001f, "LFO depth 0: cutoff static (%.1f..%.1f Hz)", double(lo), double(hi));
  }
  return ck.result("test_bass_lfo");
}
