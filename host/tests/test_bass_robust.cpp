// Robustness: every param at min/max, random patches with random playing,
// resonance sweeps, both sample rates. Output must stay finite, bounded, DC-free.
#include "bass_testutil.h"

using namespace bt;
using namespace pg::bass;

namespace {
uint32_t gSeed = 0xC0FFEEu;
uint32_t rnd() { gSeed = gSeed * 1664525u + 1013904223u; return gSeed >> 8; }
int rndInt(int lo, int hi) { return lo + int(rnd() % uint32_t(hi - lo + 1)); }

struct Stat { double pk = 0, dc = 0; bool finite = true; };
void accumulate(Stat& s, const Rig& rig) {
  s.finite = s.finite && allFinite(rig.out);
  s.pk = std::fmax(s.pk, peak(rig.out));
  s.dc = std::fmax(s.dc, std::fabs(mean(rig.out)));
}
// A short phrase that exercises accents, slides, retriggers, releases.
void phrase(Rig& rig, int base) {
  rig.on(base, 127, true); rig.runSeconds(0.12f);
  rig.on(base + 12, 110, false, true); rig.runSeconds(0.12f);
  rig.off(base + 12); rig.runSeconds(0.03f);
  rig.on(base + 7, 127, true); rig.runSeconds(0.25f);
  rig.on(base, 90, false); rig.runSeconds(0.1f);
  rig.off(base); rig.runSeconds(0.15f);
}
}  // namespace

int main() {
  Checker ck;
  const int notes[] = {24, 36, 48, 60, 72, 96};

  // ---- 1. each param to min and max, others default ---------------------------
  for (float sr : {44100.0f, 32000.0f}) {
    Stat st;
    int worstParam = -1; double worstPk = 0;
    Rig probe(sr);
    for (int p = 0; p < kNumParams; ++p) {
      const pg::ParamInfo* pi = probe.v.paramInfo(p);
      for (int v : {int(pi->min), int(pi->max)}) {
        for (int note : notes) {
          Rig rig(sr);
          rig.set(p, v);
          phrase(rig, note);
          accumulate(st, rig);
          if (peak(rig.out) > worstPk) { worstPk = peak(rig.out); worstParam = p; }
        }
      }
    }
    ck.check(st.finite && st.pk < 2.0, "sr %.0f: every param at min/max x 6 notes: finite %d, max |x| %.3f (%.1f dBFS, param %s)",
             double(sr), int(st.finite), st.pk, dB(st.pk), probe.v.paramInfo(worstParam)->name);
  }

  // ---- 2. all-min, all-max, and pairs of extremes ----------------------------------
  for (float sr : {44100.0f, 32000.0f}) {
    Stat st;
    Rig probe(sr);
    for (int mode = 0; mode < 2; ++mode) for (int note : notes) {
      Rig rig(sr);
      for (int p = 0; p < kNumParams; ++p) rig.set(p, mode ? probe.v.paramInfo(p)->max : probe.v.paramInfo(p)->min);
      phrase(rig, note);
      accumulate(st, rig);
    }
    // every pair of the level-critical params at max together
    const int hot[] = {kCutoff, kReso, kEnvMod, kAccent, kDrive, kLfoDepth, kSub, kFmMix, kFmDepth, kFold, kWave};
    for (int a : hot) for (int b : hot) {
      if (a >= b) continue;
      Rig rig(sr);
      rig.set(a, 127); rig.set(b, 127);
      phrase(rig, 36);
      accumulate(st, rig);
    }
    ck.check(st.finite && st.pk < 2.0, "sr %.0f: all-min / all-max / hot pairs: finite %d, max |x| %.3f (%.1f dBFS)", double(sr), int(st.finite), st.pk, dB(st.pk));
  }

  // ---- 3. random patches, random playing, params changing under held notes ---------------
  for (float sr : {44100.0f, 32000.0f}) {
    Stat st;
    double worstDc = 0;
    Rig probe(sr);
    for (int trial = 0; trial < 400; ++trial) {
      Rig rig(sr, float(rndInt(60, 200)));
      auto randomize = [&](int count) {
        for (int i = 0; i < count; ++i) {
          const int p = rndInt(0, kNumParams - 1);
          const pg::ParamInfo* pi = probe.v.paramInfo(p);
          const int r = rndInt(0, 9);
          rig.set(p, r == 0 ? pi->min : (r == 1 ? pi->max : rndInt(pi->min, pi->max)));
        }
      };
      randomize(kNumParams);
      for (int ev = 0; ev < 12; ++ev) {
        const int note = rndInt(0, 5) == 0 ? rndInt(0, 127) : rndInt(24, 72);
        switch (rndInt(0, 5)) {
          case 0: rig.off(note); break;
          case 1: rig.v.choke(); break;
          default: rig.on(note, rndInt(1, 127), rndInt(0, 2) == 0, rndInt(0, 2) == 0); break;
        }
        if (rndInt(0, 1)) randomize(rndInt(1, 4));
        rig.run(rndInt(1, 600));
        if (rndInt(0, 3) == 0) rig.off(note);
      }
      rig.runSeconds(0.2f);
      accumulate(st, rig);
      worstDc = std::fmax(worstDc, std::fabs(mean(rig.out)));
    }
    ck.check(st.finite && st.pk < 2.0, "sr %.0f: 400 random patches x 12 random events: finite %d, max |x| %.3f (%.1f dBFS)", double(sr), int(st.finite), st.pk, dB(st.pk));
    ck.check(worstDc < 0.01, "sr %.0f: random patches: worst mean (DC) %.5f", double(sr), worstDc);
  }

  // ---- 4. resonance swept to max while the cutoff sweeps the whole range ----------------
  for (float sr : {44100.0f, 32000.0f}) {
    for (int drive : {0, 127}) {
      Rig rig(sr);
      rig.set(kAmpDecay, 127); rig.set(kDrive, drive); rig.set(kSub, 0);
      rig.on(33, 127, true);
      const int total = rig.quantaFor(8.0f);
      double pkSelf = 0;
      for (int q = 0; q < total; ++q) {
        const float f = float(q) / float(total);
        rig.set(kReso, int(127.0f * (f < 0.5f ? f * 2.0f : 1.0f)));
        const float ph = f * 6.0f - float(int(f * 6.0f));
        rig.set(kCutoff, int(127.0f * (ph < 0.5f ? ph * 2.0f : 2.0f - ph * 2.0f)));
        rig.set(kEnvMod, (q / 500) % 2 ? 127 : 0);
        rig.run(1);
        if (f > 0.5f) pkSelf = std::fmax(pkSelf, peak(rig.out, rig.pos() - 16));
      }
      const double pk = peak(rig.out);
      ck.check(allFinite(rig.out) && pk < 1.0 && pkSelf > 0.02,
               "sr %.0f drive %3d: resonance sweep to max with cutoff sweeps: finite, peak %.3f (%.1f dBFS), still sounding at RES max (%.3f)",
               double(sr), drive, pk, dB(pk), pkSelf);
    }
  }

  // ---- 5. API edge cases --------------------------------------------------------------------
  {
    Rig rig;
    uint8_t wild[pg::kSoundParams];
    for (int i = 0; i < pg::kSoundParams; ++i) wild[i] = 255;     // out-of-range values must clamp
    rig.v.setParams(wild);
    pg::NoteEvent e{127, 127, true, false, 0};
    rig.v.noteOn(e);
    float l[16], r[16];
    bool ok = true;
    double pk = 0;
    for (int q = 0; q < 4000; ++q) {
      rig.v.render(l, r, 16);
      for (int i = 0; i < 16; ++i) { ok = ok && std::isfinite(l[i]) && l[i] == r[i]; pk = std::fmax(pk, std::fabs(double(l[i]))); }
    }
    ck.check(ok && pk < 2.0, "out-of-range param bytes (255) and note 127: clamped, finite, L == R, peak %.3f", pk);

    Rig idle;
    ck.check(!idle.v.active(), "voice is inactive after init");
    idle.v.render(l, r, 16);
    bool zero = true;
    for (int i = 0; i < 16; ++i) zero = zero && l[i] == 0.0f && r[i] == 0.0f;
    ck.check(zero, "render while idle writes silence");
    idle.on(40, 100); idle.runSeconds(0.05f); idle.off(40); idle.runSeconds(1.0f);
    ck.check(!idle.v.active(), "voice goes inactive after the release tail (engine can skip it)");
    idle.on(40, 100); idle.runSeconds(0.05f); idle.v.choke(); idle.runSeconds(0.01f);
    ck.check(!idle.v.active(), "voice goes inactive after choke");
    int n = 0;
    for (int i = 0; i < pg::kSoundParams; ++i) {
      const pg::ParamInfo* pi = idle.v.paramInfo(i);
      if (pi) { ++n; ok = ok && pi->def >= pi->min && pi->def <= pi->max && std::strlen(pi->name) <= 4 && std::strlen(pi->longName) <= 12; }
      else ok = ok && i >= kNumParams;
    }
    ck.check(ok && n == kNumParams, "param table: %d contiguous entries, defaults in range, names fit", n);
  }
  return ck.result("test_bass_robust");
}
