// Robustness: every param at min and max (others default), all-min / all-max,
// and random combinations with p-lock style changes during notes, at 44100 and
// 32000 Hz, chords across the whole keyboard. Output must stay finite, bounded
// (|x| < 2.0), free of DC, and the voice must go idle after the tails.
#include "test_lead_util.h"

using namespace lt;
using namespace pg::lead;

namespace {

struct Stats {
  double peak = 0, dc = 0;   // dc: |mean| of the whole render, re full scale
  bool finite = true, idle = true;
};

// Notes and chords spanning the keyboard, overlapping, with slides, accents,
// velocity extremes. Runs until the synth is idle (or `cap` seconds).
Stats exercise(Synth& s, Rng& rng, bool randomLocks, double cap = 16.0) {
  Stats st;
  const int roots[] = {12, 24, 36, 48, 55, 60, 67, 72, 84, 96, 108, 120, 127};
  int prev = -1;
  for (int k = 0; k < 13; ++k) {
    const int root = roots[k];
    const int chord = k % pg::kChordNum;
    const int vel = (k % 3 == 0) ? 1 : (k % 3 == 1 ? 127 : 64);
    s.on(root, vel, chord, k % 4 == 1, k % 5 == 2);
    for (int q = 0; q < 160; ++q) {   // ~58 ms at 44.1k
      if (randomLocks && q % 8 == 0) s.set(rng.range(0, kNumParams - 1), rng.range(0, 127));
      s.runQuanta(1);
    }
    if (k % 2 == 0 && prev >= 0) s.off(prev);
    prev = root;
  }
  for (int n = 0; n <= 127; ++n) s.off(uint8_t(n));
  const double t0 = s.now();
  while (s.v.active() && s.now() - t0 < cap) s.runQuanta(64);
  st.idle = !s.v.active();
  st.finite = allFinite(s.L) && allFinite(s.R);
  st.peak = std::max(peak(s.L), peak(s.R));
  st.dc = std::max(std::fabs(mean(s.L)), std::fabs(mean(s.R)));
  return st;
}

void clampToInfo(Synth& s) {
  for (int i = 0; i < kNumParams; ++i) {
    const pg::ParamInfo* pi = s.v.paramInfo(i);
    if (s.p[i] < pi->min) s.p[i] = pi->min;
    if (s.p[i] > pi->max) s.p[i] = pi->max;
  }
}

// DC as a Blackman-Harris weighted mean: rejects every component above ~4 bins
// (a few Hz for a 1.5 s window), unlike a plain mean over a non-integer number
// of periods of a low note.
double windowedMean(const std::vector<float>& x, size_t a, size_t b) {
  double s = 0, ws = 0;
  for (size_t i = a; i < b; ++i) {
    const double t = 2.0 * M_PI * double(i - a) / double(b - a);
    const double w = 0.35875 - 0.48829 * std::cos(t) + 0.14128 * std::cos(2 * t) - 0.01168 * std::cos(3 * t);
    s += w * double(x[i]);
    ws += w;
  }
  return s / ws;
}

}  // namespace

int main() {
  // ---- metadata sanity
  {
    Synth s;
    for (int i = 0; i < pg::kSoundParams; ++i) {
      const pg::ParamInfo* pi = s.v.paramInfo(i);
      LT_CHECK(pi != nullptr, "param %d has no info", i);
      if (!pi) continue;
      LT_CHECK(pi->min <= pi->def && pi->def <= pi->max, "param %s default out of range", pi->name);
      LT_CHECK(std::strlen(pi->name) <= 4 && std::strlen(pi->longName) <= 12, "param %s name too long", pi->name);
      if (pi->fmt == pg::ParamFormat::Enum) LT_CHECK(pi->enumNames != nullptr, "enum %s without names", pi->name);
    }
    LT_CHECK(s.v.paramInfo(-1) == nullptr && s.v.paramInfo(pg::kSoundParams) == nullptr, "out-of-range paramInfo");
  }

  double worstPeak = 0, worstDc = 0;
  int runs = 0;
  for (float fs : {44100.0f, 32000.0f}) {
    Rng rng(1234u + uint32_t(fs));
    // ---- each param at min and max
    for (int i = 0; i < kNumParams; ++i) {
      for (int side = 0; side < 2; ++side) {
        Synth s(fs);
        const pg::ParamInfo* pi = s.v.paramInfo(i);
        s.set(i, side ? pi->max : pi->min);
        const Stats st = exercise(s, rng, false);
        ++runs;
        worstPeak = std::max(worstPeak, st.peak);
        worstDc = std::max(worstDc, st.dc);
        LT_CHECK(st.finite, "fs %.0f %s=%d: not finite", double(fs), pi->name, s.p[i]);
        LT_CHECK(st.peak < 2.0, "fs %.0f %s=%d: peak %.3f", double(fs), pi->name, s.p[i], st.peak);
        LT_CHECK(st.dc < 0.002, "fs %.0f %s=%d: DC %.4f", double(fs), pi->name, s.p[i], st.dc);
        LT_CHECK(st.idle, "fs %.0f %s=%d: still active after the tails", double(fs), pi->name, s.p[i]);
      }
    }
    // ---- all params at min, all at max, and every WAVE x FILTER type at extreme RES / DRIVE
    for (int mode = 0; mode < 2; ++mode) {
      Synth s(fs);
      for (int i = 0; i < kNumParams; ++i) s.set(i, mode ? 255 : 0);
      clampToInfo(s);
      const Stats st = exercise(s, rng, false);
      ++runs;
      worstPeak = std::max(worstPeak, st.peak);
      LT_CHECK(st.finite && st.peak < 2.0 && st.idle, "fs %.0f all-%s: finite %d peak %.3f idle %d", double(fs),
               mode ? "max" : "min", st.finite, st.peak, st.idle);
    }
    for (int w = 0; w < kNumWaves; ++w)
      for (int f = 0; f < kNumFilterTypes; ++f) {
        Synth s(fs);
        s.set(kPWave, w); s.set(kPFltType, f); s.set(kPReso, 127); s.set(kPDrive, 127); s.set(kPEnvAmt, 127);
        s.set(kPCutoff, 100); s.set(kPShape, 127); s.set(kPOscMix, 64); s.set(kPDetune, 127); s.set(kPLfoCutoff, 127);
        s.set(kPAttack, 0); s.set(kPRelease, 30);
        const Stats st = exercise(s, rng, false);
        ++runs;
        worstPeak = std::max(worstPeak, st.peak);
        worstDc = std::max(worstDc, st.dc);
        LT_CHECK(st.finite && st.peak < 2.0 && st.idle && st.dc < 0.002, "fs %.0f wave %d flt %d extreme: finite %d peak %.3f dc %.4f idle %d",
                 double(fs), w, f, st.finite, st.peak, st.dc, st.idle);
      }
    // ---- random combinations, with random p-locks while notes sound
    for (int r = 0; r < 120; ++r) {
      Synth s(fs);
      for (int i = 0; i < kNumParams; ++i) s.set(i, rng.range(0, 127));
      clampToInfo(s);
      const bool locks = r % 2 == 1;
      const Stats st = exercise(s, rng, locks);
      ++runs;
      worstPeak = std::max(worstPeak, st.peak);
      worstDc = std::max(worstDc, st.dc);
      LT_CHECK(st.finite, "fs %.0f random %d: not finite", double(fs), r);
      LT_CHECK(st.peak < 2.0, "fs %.0f random %d: peak %.3f", double(fs), r, st.peak);
      LT_CHECK(st.dc < 0.002, "fs %.0f random %d: DC %.4f", double(fs), r, st.dc);
      LT_CHECK(st.idle, "fs %.0f random %d: still active after the tails", double(fs), r);
    }
  }
  std::printf("robust: %d runs at 44100 + 32000 Hz, worst peak %.3f (%.2f dBFS), worst whole-render DC %.5f\n", runs,
              worstPeak, dB(worstPeak), worstDc);

  // ---- DC on held chords (1.5 s window, weighted mean), every wave x filter, SHAPE 0 / 64 / 127.
  // DRIVE 0 must be exactly DC-free. With DRIVE the clipper makes DC from the
  // (not odd-symmetric) organ table and slow intermodulation from detuned
  // stacks; the drive-dependent DC blocker must keep that below -54 dBFS.
  {
    double worst[2] = {0, 0};
    for (int drv = 0; drv < 2; ++drv)
      for (int w = 0; w < kNumWaves; ++w)
        for (int f = 0; f < kNumFilterTypes; ++f)
          for (int shp : {0, 64, 127})
            for (int root : {36, 60, 84}) {
              Synth s;
              s.set(kPWave, w); s.set(kPFltType, f); s.set(kPShape, shp); s.set(kPSustain, 127); s.set(kPDrive, drv ? 127 : 0);
              s.on(root, 127, pg::kChordMin7);
              s.run(2.0);
              const size_t a = size_t(0.5 * double(s.fs)), b = s.size();
              const double dc = std::max(std::fabs(windowedMean(s.L, a, b)), std::fabs(windowedMean(s.R, a, b)));
              worst[drv] = std::max(worst[drv], dc);
              const double lim = drv ? 0.002 : 1e-4;
              LT_CHECK(dc < lim, "held chord drive %d wave %d flt %d shape %d root %d: DC %.5f", drv, w, f, shp, root, dc);
            }
    std::printf("held chords (all waves x filters x shapes): worst DC %.6f at DRIVE 0, %.6f at DRIVE max\n", worst[0], worst[1]);
  }

  // ---- re-init at another sample rate on the same object, and render before init
  {
    LeadVoice v;
    float l[pg::kQuantum], r[pg::kQuantum];
    v.render(l, r, pg::kQuantum);   // not initialised: silence, no crash
    LT_CHECK(l[0] == 0.0f && r[0] == 0.0f, "uninitialised render not silent");
    v.init(32000.0f);
    v.init(44100.0f);
    pg::NoteEvent e{60, 127, false, false, pg::kChordMin7};
    v.noteOn(e);
    bool ok = true;
    for (int q = 0; q < 200; ++q) {
      v.render(l, r, pg::kQuantum);
      for (int i = 0; i < pg::kQuantum; ++i) ok = ok && std::isfinite(l[i]) && std::fabs(l[i]) < 2.0f;
    }
    LT_CHECK(ok, "re-init at another rate broke the output");
  }
  return finish("test_lead_robust");
}
