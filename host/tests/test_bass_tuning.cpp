// Tuning across the note range and both sample rates.
#include "bass_testutil.h"

using namespace bt;
using namespace pg::bass;

int main() {
  Checker ck;
  for (float sr : {44100.0f, 32000.0f}) {
    double worst = 0;
    for (int wave : {0, 64}) {
      for (int note = 24; note <= 84; note += 3) {
        Rig rig(sr);
        rig.set(kReso, 10);
        rig.set(kEnvMod, 0);
        rig.set(kCutoff, 100);
        rig.set(kSub, 0);
        rig.set(kDrive, 0);
        rig.set(kAmpDecay, 127);
        rig.set(kWave, wave);
        rig.on(note, 127);
        rig.runSeconds(1.2f);
        const size_t n = 32768;
        const double f = findPeakFreq(rig.out, rig.out.size() - n, n, midiHz(note), sr, 1.0);
        const double c = cents(f, midiHz(note));
        worst = std::fmax(worst, std::fabs(c));
        if (std::fabs(c) > 2.0) ck.check(false, "sr %.0f wave %d note %d: %.3f Hz, %+.2f cents", double(sr), wave, note, f, c);
      }
    }
    ck.check(worst < 2.0, "sr %.0f: notes 24..84, saw + square: worst tuning error %.3f cents", double(sr), worst);

    // Sub oscillator (unison and -1 octave) and the FM carrier track the same pitch.
    for (int subOct : {0, 1}) {
      Rig rig(sr);
      rig.set(kSub, 127);
      rig.set(kCutoff, 0); rig.set(kEnvMod, 0); rig.set(kReso, 0); rig.set(kKeyTrack, 0);
      rig.set(kSubOct, subOct);
      rig.set(kAmpDecay, 127);
      rig.on(45, 127);
      rig.runSeconds(1.2f);
      const double ref = midiHz(45 - 12 * subOct);
      const double f = findPeakFreq(rig.out, rig.out.size() - 32768, 32768, ref, sr, 1.0);
      ck.check(std::fabs(cents(f, ref)) < 2.0, "sr %.0f sub oct -%d: %.3f Hz (%+.2f cents)", double(sr), subOct, f, cents(f, ref));
    }
    {
      Rig rig(sr);
      rig.set(kFmMix, 127); rig.set(kFmDepth, 0); rig.set(kSub, 0); rig.set(kCutoff, 110); rig.set(kAmpDecay, 127);
      rig.set(kTune, 24 + 7);
      rig.on(40, 127);
      rig.runSeconds(1.2f);
      const double ref = midiHz(47);
      const double f = findPeakFreq(rig.out, rig.out.size() - 32768, 32768, ref, sr, 1.0);
      ck.check(std::fabs(cents(f, ref)) < 2.0, "sr %.0f FM carrier, tune +7: %.3f Hz (%+.2f cents)", double(sr), f, cents(f, ref));
    }
  }
  return ck.result("test_bass_tuning");
}
