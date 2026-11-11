// Levels and dynamics: default level, velocity curve, accent (level + filter
// excursion), click-free note on/off/choke/retrigger, DC.
#include "bass_testutil.h"

using namespace bt;
using namespace pg::bass;

namespace {
struct NoteStats { double pk, r, cutMax, cent; };
NoteStats oneNote(Rig& rig, int note, int vel, bool accent, float gateS = 0.12f, float tailS = 0.3f) {
  const size_t a = rig.pos();
  const size_t q0 = rig.cut.size();
  rig.on(note, vel, accent);
  rig.runSeconds(gateS);
  rig.off(note);
  rig.runSeconds(tailS);
  NoteStats s;
  s.pk = peak(rig.out, a);
  s.r = rms(rig.out, a, a + size_t(gateS * rig.sr));
  s.cutMax = 0;
  for (size_t q = q0; q < rig.cut.size(); ++q) s.cutMax = std::fmax(s.cutMax, double(rig.cut[q]));
  // Brightness over the first 60 ms: RMS of the first difference over RMS, as
  // an equivalent frequency (Hz). A power-weighted spectral centroid is
  // dominated by the fundamental at bass pitches and barely moves.
  {
    const size_t b = a + size_t(0.06f * rig.sr);
    double e = 0, d = 0;
    for (size_t i = a + 1; i < b && i < rig.out.size(); ++i) {
      const double q = double(rig.out[i]) - double(rig.out[i - 1]);
      e += double(rig.out[i]) * double(rig.out[i]);
      d += q * q;
    }
    s.cent = e > 0 ? std::sqrt(d / e) * double(rig.sr) / (2.0 * kPiD) : 0.0;
  }
  return s;
}
}  // namespace

int main() {
  Checker ck;

  // ---- default level: full velocity, no accent -> about -9 dBFS ----------------
  for (float sr : {44100.0f, 32000.0f}) {
    double lo = 1e9, hi = -1e9;
    std::printf("  default patch, vel 127, sr %.0f: ", double(sr));
    for (int note : {29, 33, 36, 41, 45, 48, 55}) {
      Rig rig(sr);
      NoteStats s = oneNote(rig, note, 127, false);
      std::printf("n%d %.1f  ", note, dB(s.pk));
      lo = std::fmin(lo, dB(s.pk)); hi = std::fmax(hi, dB(s.pk));
    }
    std::printf("dBFS peak\n");
    ck.check(lo > -11.0 && hi < -7.5, "sr %.0f: default note peaks between %.2f and %.2f dBFS (target -9)", double(sr), lo, hi);
  }

  // ---- velocity curve ---------------------------------------------------------
  {
    double prev = -1, prevR = -1;
    bool mono = true;
    std::printf("  velocity -> peak dBFS: ");
    double d64 = 0, d1 = 0;
    for (int vel : {1, 8, 16, 32, 48, 64, 80, 96, 112, 127}) {
      Rig rig;
      NoteStats s = oneNote(rig, 36, vel, false);
      std::printf("%d:%.1f ", vel, dB(s.pk));
      if (s.pk <= prev || s.r <= prevR) mono = false;
      prev = s.pk; prevR = s.r;
      if (vel == 64) d64 = dB(s.pk);
      if (vel == 1) d1 = dB(s.pk);
    }
    std::printf("\n");
    Rig rig;
    const double full = dB(oneNote(rig, 36, 127, false).pk);
    ck.check(mono, "velocity curve is strictly monotonic (peak and rms)");
    ck.check(d64 - full < -6.0 && d64 - full > -14.0 && d1 - full < -30.0,
             "velocity range: vel 64 is %.1f dB, vel 1 is %.1f dB below vel 127", d64 - full, d1 - full);
  }

  // ---- accent -------------------------------------------------------------------
  {
    std::printf("  accent (ACC param -> peak gain dB / rms gain dB / max cutoff ratio / brightness ratio):\n");
    double gainDef = 0, gainMax = 0;
    for (int acc : {0, 45, 90, 127}) {
      Rig a, b;
      a.set(kAccent, acc); b.set(kAccent, acc);
      NoteStats n = oneNote(a, 36, 127, false), x = oneNote(b, 36, 127, true);
      const double gp = dB(x.pk / n.pk), gr = dB(x.r / n.r);
      std::printf("    ACC %3d: %+5.2f dB peak, %+5.2f dB rms, cutoff max %.0f -> %.0f Hz (x%.2f), brightness %.0f -> %.0f Hz\n",
                  acc, gp, gr, n.cutMax, x.cutMax, x.cutMax / n.cutMax, n.cent, x.cent);
      if (acc == 90) {
        gainDef = gp;
        ck.check(gp > 1.5 && gp < 4.5 && gr > 1.0, "accent at default ACC: %+.2f dB peak, %+.2f dB rms", gp, gr);
        ck.check(x.cutMax > 1.3 * n.cutMax && x.cent > 1.25 * n.cent,
                 "accent opens the filter further: cutoff peak x%.2f, brightness (first 60 ms) x%.2f", x.cutMax / n.cutMax, x.cent / n.cent);
      }
      if (acc == 127) { gainMax = gp; ck.check(gp > gainDef && gp < 5.0, "accent at ACC 127: %+.2f dB peak (limit +5)", gp); }
      if (acc == 0) ck.check(std::fabs(gp) < 0.3 && std::fabs(x.cent / n.cent - 1.0) < 0.03, "ACC 0: accent has no effect (%+.2f dB, brightness x%.3f)", gp, x.cent / n.cent);
    }
    (void)gainMax;
    // 303 behaviour: consecutive accents build up the filter kick; accented decay is short.
    Rig rig;
    rig.set(kDecay, 100);
    double c1 = oneNote(rig, 36, 127, true, 0.1f, 0.019f).cutMax;
    double c2 = oneNote(rig, 36, 127, true, 0.1f, 0.019f).cutMax;
    double c3 = oneNote(rig, 36, 127, true, 0.1f, 0.019f).cutMax;
    ck.check(c2 > c1 * 1.02 && c3 >= c2, "consecutive accents stack: cutoff peaks %.0f, %.0f, %.0f Hz", c1, c2, c3);
    Rig n2, a2;
    n2.set(kDecay, 100); a2.set(kDecay, 100);
    n2.on(36, 127, false); a2.on(36, 127, true);
    n2.runSeconds(0.25f); a2.runSeconds(0.25f);
    ck.check(a2.v.debugFilterEnv() < 0.2f * n2.v.debugFilterEnv(), "accent shortens the filter decay (env after 250 ms: %.3f vs %.3f)",
             double(a2.v.debugFilterEnv()), double(n2.v.debugFilterEnv()));
  }

  // ---- clicks -------------------------------------------------------------------
  // Sine-only patch: any discontinuity shows up as a step larger than the
  // sine's own slope.
  for (float sr : {44100.0f, 32000.0f}) {
    Rig rig(sr);
    rig.set(kSub, 127); rig.set(kCutoff, 0); rig.set(kEnvMod, 0); rig.set(kReso, 0); rig.set(kKeyTrack, 0);
    rig.set(kDrive, 0); rig.set(kAmpDecay, 127);
    rig.on(45, 127);
    rig.runSeconds(0.2f);
    const double steady = maxStep(rig.out, size_t(0.1f * sr));
    const double onStep = maxStep(rig.out, 1, size_t(0.02f * sr));
    const double first = std::fabs(double(rig.out[0]));
    size_t t = rig.pos();
    rig.off(45);
    rig.runSeconds(0.1f);
    const double offStep = maxStep(rig.out, t);
    const double tailEnd = peak(rig.out, rig.pos() - 64);
    // retrigger while sounding (gate still high), different velocity
    rig.on(45, 127); rig.runSeconds(0.15f);
    t = rig.pos();
    rig.on(52, 70); rig.runSeconds(0.1f);
    const double retrigStep = maxStep(rig.out, t);
    // retrigger inside the release tail
    rig.off(52); rig.run(6);
    t = rig.pos();
    rig.on(45, 127); rig.runSeconds(0.1f);
    const double tailRetrig = maxStep(rig.out, t);
    // choke
    t = rig.pos();
    rig.v.choke();
    rig.runSeconds(0.02f);
    const double chokeStep = maxStep(rig.out, t);
    const double chokeMs = [&] { size_t i = rig.pos(); while (i > t && std::fabs(rig.out[i - 1]) < 1e-5f) --i; return double(i - t) * 1000.0 / double(sr); }();
    // A ramp a(t) on a sine has slope up to (a' + a w) A: a 1.5 ms attack on
    // 110 Hz may legitimately reach ~1.9x the steady slope and a 2 ms choke
    // ~1.7x without any discontinuity. A real click (a step) is many times
    // the steady slope, so these limits still catch one.
    ck.check(first < 0.003 && onStep <= steady * 1.5, "sr %.0f note-on: first sample %.5f, max step %.5f (steady sine %.5f)", double(sr), first, onStep, steady);
    ck.check(offStep <= steady * 1.05 && tailEnd < 1e-4, "sr %.0f note-off: max step %.5f (steady %.5f), tail ends at %.6f", double(sr), offStep, steady, tailEnd);
    ck.check(retrigStep <= steady * 1.6, "sr %.0f legato retrigger: max step %.5f (steady %.5f)", double(sr), retrigStep, steady);
    ck.check(tailRetrig <= steady * 1.3, "sr %.0f retrigger in release tail: max step %.5f (steady %.5f)", double(sr), tailRetrig, steady);
    ck.check(chokeStep <= steady * 1.4 && chokeMs < 4.0 && !rig.v.active(), "sr %.0f choke: silent after %.2f ms, max step %.5f, voice inactive", double(sr), chokeMs, chokeStep);
  }
  // Default (bright) patch: note-off / choke must not add a step beyond what the waveform already has.
  {
    Rig rig;
    rig.set(kAmpDecay, 127); rig.set(kDecay, 127);
    rig.on(36, 127);
    rig.runSeconds(0.3f);
    const double steady = maxStep(rig.out, size_t(0.1f * rig.sr));
    size_t t = rig.pos();
    rig.off(36);
    rig.runSeconds(0.2f);
    const double offStep = maxStep(rig.out, t);
    rig.on(36, 127); rig.runSeconds(0.2f);
    t = rig.pos();
    rig.v.choke(); rig.runSeconds(0.02f);
    const double chokeStep = maxStep(rig.out, t);
    ck.check(offStep <= steady * 1.02 && chokeStep <= steady * 1.02 && !rig.v.active(),
             "default patch: note-off step %.4f, choke step %.4f, steady-state step %.4f", offStep, chokeStep, steady);
    // attack from silence: first 0.5 ms is quiet
    Rig r2;
    r2.on(36, 127);
    r2.runSeconds(0.05f);
    ck.check(peak(r2.out, 0, 8) < 0.02, "default patch: attack starts from silence (first 8 samples peak %.4f)", peak(r2.out, 0, 8));
  }

  // ---- DC -----------------------------------------------------------------------
  {
    double worst = 0;
    for (int drive : {0, 64, 127}) for (int fold : {0, 127}) for (int wave : {0, 100, 127}) {
      Rig rig;
      rig.set(kDrive, drive); rig.set(kFold, fold); rig.set(kWave, wave); rig.set(kAmpDecay, 127); rig.set(kDecay, 100);
      rig.set(kFmMix, fold ? 64 : 0);
      rig.on(40, 127);
      rig.runSeconds(1.5f);
      worst = std::fmax(worst, std::fabs(mean(rig.out, size_t(0.5f * rig.sr))));
    }
    ck.check(worst < 0.002, "no DC with asymmetric drive / fold / narrow pulse: worst mean %.5f", worst);
  }
  return ck.result("test_bass_dynamics");
}
