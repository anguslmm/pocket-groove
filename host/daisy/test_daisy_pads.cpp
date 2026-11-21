// Host test of the force-sensor pad scanner (daisy/core/pads.h) with synthetic force
// curves, scanned at the Daisy's 2 ms (500 Hz): soft and hard hits (velocity rises
// with the force's rise, a note decided one scan after the crossing), the crossing's
// phase between two scans (the velocity hardly moves), a slow press (one quiet note,
// then the pressure), hysteresis on release (a hover near the threshold and a
// one-scan bounce do not re-trigger), noise and one-scan spikes (nothing), flams on
// two pads and on one (both play), crosstalk (a hard hit's leak into the next pad is
// dropped, a deliberate soft hit later is not), calibration changes and a pad turned
// off; the default curve (a medium hit at 0.79 = note velocity 100, a hard one at 1 =
// 127, pads.h THE VELOCITY CURVE); pressure never takes the ring's last places (a
// stalled main loop loses pressure values, not a note's start or end); and the
// settings file (settings.h): round trip with the pads' job, a version 1 file still
// read, a corrupt file refused.
#include <cmath>
#include <cstring>
#include <vector>

#include "check.h"
#include "core/pads.h"
#include "core/settings.h"
#include "pg/sampler/sample_format.h"
#include "pg/ui/pads.h"

using namespace pgd;

namespace {

constexpr float kScanMs = 2.0f;

// A hit's conductance (uS) at time t ms after its start: a linear rise to peak over
// riseMs, held for holdMs, a linear fall over fallMs.
float hit(float peak, float riseMs, float holdMs, float fallMs, float t) {
  if (t < 0.0f) return 0.0f;
  if (t < riseMs) return peak * t / riseMs;
  t -= riseMs;
  if (t < holdMs) return peak;
  t -= holdMs;
  if (t < fallMs) return peak * (1.0f - t / fallMs);
  return 0.0f;
}

struct Run {
  std::vector<PadEvent> ev;
  int downs(int pad) const {
    int n = 0;
    for (const PadEvent& e : ev) n += e.type == PadEvent::Down && e.pad == pad;
    return n;
  }
  int ups(int pad) const {
    int n = 0;
    for (const PadEvent& e : ev) n += e.type == PadEvent::Up && e.pad == pad;
    return n;
  }
  const PadEvent* first(int pad, uint8_t type) const {
    for (const PadEvent& e : ev)
      if (e.pad == pad && e.type == type) return &e;
    return nullptr;
  }
  float velocity(int pad) const {
    const PadEvent* e = first(pad, PadEvent::Down);
    return e ? e->value : -1.0f;
  }
};

// Scans `ms` of the four curves f(pad, t) (uS), t from t0.
template <class F>
Run scanFor(PadScanner& s, float ms, F f, float t0 = 0.0f, std::vector<uint32_t>* crossScan = nullptr,
            float threshold = 10.0f) {
  Run r;
  bool crossed[PadScanner::kPads] = {};
  for (float t = t0; t < t0 + ms; t += kScanMs) {
    float x[PadScanner::kPads];
    for (int p = 0; p < PadScanner::kPads; ++p) {
      const float g = f(p, t);
      x[p] = PadScanner::readingFor(g);
      if (crossScan && !crossed[p] && g >= threshold) {
        crossed[p] = true;
        (*crossScan)[size_t(p)] = s.scans() + 1;   // the scan that is about to run
      }
    }
    s.scan(x);
    PadEvent e;
    while (s.pop(&e)) r.ev.push_back(e);
  }
  return r;
}

// Back to idle with the default calibration.
void defaults(PadScanner& s) {
  s.reset();
  for (int i = 0; i < PadScanner::kPads; ++i) s.setCal(i, PadCal{});
  s.setGlobal(PadGlobal{});
}

void testConductance() {
  CHECK(std::fabs(PadScanner::conductance(0.5f) - 100.0f) < 0.01f);   // R_fsr = 10 k
  CHECK(PadScanner::conductance(0.0f) == 0.0f);
  CHECK(PadScanner::conductance(1.0f) < 300000.0f);                   // clamped, finite
  for (float g : {1.0f, 10.0f, 100.0f, 1000.0f}) CHECK(std::fabs(PadScanner::conductance(PadScanner::readingFor(g)) - g) < g * 1e-4f);
}

void testSoftHardAndLatency() {
  PadScanner s;
  defaults(s);
  std::vector<uint32_t> cross(4, 0);
  // soft: 120 uS over 8 ms; hard: 900 uS over 2 ms (pad 0, then pad 0 again later)
  Run soft = scanFor(s, 200, [](int p, float t) { return p == 0 ? hit(120, 8, 40, 10, t) : 0.0f; }, 0, &cross);
  CHECK_EQ(soft.downs(0), 1);
  CHECK_EQ(soft.ups(0), 1);
  const PadEvent* d = soft.first(0, PadEvent::Down);
  CHECK(d != nullptr);
  if (d) CHECK(d->scan - cross[0] <= 1u);   // decided at most one scan (2 ms) after the crossing scan
  Run hard = scanFor(s, 200, [](int p, float t) { return p == 0 ? hit(900, 2, 40, 10, t) : 0.0f; }, 0, &cross);
  CHECK_EQ(hard.downs(0), 1);
  d = hard.first(0, PadEvent::Down);
  if (d) CHECK(d->scan - cross[0] <= 1u);
  const float vs = soft.velocity(0), vh = hard.velocity(0);
  std::printf("  soft hit velocity %.3f, hard hit %.3f\n", double(vs), double(vh));
  CHECK(vs > 0.0f && vs < 0.4f);
  CHECK(vh > 0.95f);
  // velocity rises with the force (same rise time, peaks 40..1000 uS)
  float prev = -1.0f;
  bool mono = true;
  for (float peak : {40.0f, 80.0f, 160.0f, 320.0f, 640.0f, 1000.0f}) {
    Run r = scanFor(s, 200, [peak](int p, float t) { return p == 1 ? hit(peak, 6, 30, 10, t) : 0.0f; });
    const float v = r.velocity(1);
    mono = mono && v > prev && r.downs(1) == 1;
    prev = v;
  }
  CHECK(mono);
}

void testPhase() {
  // The same hit starting 0, 0.5, 1.0, 1.5 ms into a scan: the velocity barely moves.
  float lo = 2.0f, hi = -1.0f;
  for (float off : {0.0f, 0.5f, 1.0f, 1.5f}) {
    PadScanner s;
    defaults(s);
    Run r = scanFor(s, 200, [off](int p, float t) { return p == 2 ? hit(400, 8, 30, 10, t - off) : 0.0f; });
    CHECK_EQ(r.downs(2), 1);
    const float v = r.velocity(2);
    lo = v < lo ? v : lo;
    hi = v > hi ? v : hi;
  }
  std::printf("  phase: velocity %.3f .. %.3f\n", double(lo), double(hi));
  CHECK(hi - lo < 0.1f * hi);
}

void testSlowPress() {
  PadScanner s;
  defaults(s);
  // 0 -> 600 uS over 300 ms, held 200 ms, released over 100 ms
  Run r = scanFor(s, 800, [](int p, float t) { return p == 3 ? hit(600, 300, 200, 100, t) : 0.0f; });
  CHECK_EQ(r.downs(3), 1);
  CHECK_EQ(r.ups(3), 1);
  CHECK(r.velocity(3) < 0.15f);
  float maxP = 0.0f, lastP = 0.0f;
  int pressures = 0;
  bool upLast = false;
  for (const PadEvent& e : r.ev) {
    if (e.type == PadEvent::Pressure) {
      ++pressures;
      maxP = e.value > maxP ? e.value : maxP;
      lastP = e.value;
    }
    upLast = e.type == PadEvent::Up;
  }
  std::printf("  slow press: velocity %.3f, %d pressure events, max %.3f\n", double(r.velocity(3)), pressures, double(maxP));
  CHECK(pressures > 20);
  CHECK(std::fabs(maxP - (600.0f - 10.0f) / (PadCal{}.ceiling - 10.0f)) < 0.02f);
  CHECK(lastP < 0.1f);
  CHECK(upLast);
}

void testHysteresisAndBounce() {
  PadScanner s;
  defaults(s);
  // a press to 80 uS that decays to a hover between 7 and 13 uS (around the threshold,
  // above the release level of 6) for 300 ms
  Run r = scanFor(s, 500, [](int p, float t) {
    if (p != 0) return 0.0f;
    if (t < 40.0f) return hit(80, 5, 20, 15, t) + (t > 25.0f ? 10.0f : 0.0f);
    if (t < 340.0f) return 10.0f + 3.0f * std::sin(t * 0.7f);
    return 0.0f;
  });
  CHECK_EQ(r.downs(0), 1);
  CHECK_EQ(r.ups(0), 1);
  // a bounce: one scan below the release level in the middle of a hit
  defaults(s);
  Run b = scanFor(s, 200, [](int p, float t) {
    if (p != 1) return 0.0f;
    if (t >= 20.0f && t < 22.0f) return 2.0f;   // one scan
    return hit(300, 4, 50, 10, t);
  });
  CHECK_EQ(b.downs(1), 1);
  CHECK_EQ(b.ups(1), 1);
}

void testNoise() {
  PadScanner s;
  defaults(s);
  uint32_t seed = 7;
  auto rnd = [&seed]() {
    seed = seed * 1664525u + 1013904223u;
    return float(int32_t(seed)) / 2147483648.0f;
  };
  // the idle sensor: 0.5 uS +- 3 uS of noise for 2 s, on every pad
  Run r = scanFor(s, 2000, [&](int, float) { return 0.5f + 3.0f * rnd(); });
  CHECK(r.ev.empty());
  // one-scan spikes to 40 uS now and then: blips, no notes
  Run sp = scanFor(s, 2000, [&](int p, float t) {
    const int k = int(t / kScanMs);
    return (k % 97 == p * 13) ? 40.0f : 0.5f + 2.0f * rnd();
  });
  CHECK(sp.ev.empty());
  CHECK(s.blips(0) > 0);
}

void testFlams() {
  PadScanner s;
  defaults(s);
  // two pads about as hard, 20 ms apart: both play
  Run r = scanFor(s, 300, [](int p, float t) {
    if (p == 0) return hit(500, 3, 40, 10, t);
    if (p == 1) return hit(420, 3, 40, 10, t - 20.0f);
    return 0.0f;
  });
  CHECK_EQ(r.downs(0), 1);
  CHECK_EQ(r.downs(1), 1);
  // one pad, two strokes 30 ms apart (a release in between): two notes
  defaults(s);
  Run one = scanFor(s, 300, [](int p, float t) {
    if (p != 2) return 0.0f;
    return hit(300, 3, 10, 4, t) + hit(500, 3, 10, 4, t - 30.0f);
  });
  CHECK_EQ(one.downs(2), 2);
  CHECK_EQ(one.ups(2), 2);
}

void testCrosstalk() {
  PadScanner s;
  defaults(s);
  // a hard hit on pad 0 leaks 25 % of its force into pad 1, a scan later: pad 1 is a ghost
  Run r = scanFor(s, 300, [](int p, float t) {
    if (p == 0) return hit(900, 2, 40, 10, t);
    if (p == 1) return 0.25f * hit(900, 2, 40, 10, t - 1.0f);
    return 0.0f;
  });
  CHECK_EQ(r.downs(0), 1);
  CHECK_EQ(r.downs(1), 0);
  CHECK_EQ(s.ghosts(1), 1u);
  CHECK_EQ(r.ups(1), 0);   // and no stray release for it
  // the same leak with the cancellation off: pad 1 plays (so the test means something)
  PadGlobal g;
  g.xtalkScans = 0;
  defaults(s);
  s.setGlobal(g);
  Run off = scanFor(s, 300, [](int p, float t) {
    if (p == 0) return hit(900, 2, 40, 10, t);
    if (p == 1) return 0.25f * hit(900, 2, 40, 10, t - 1.0f);
    return 0.0f;
  });
  CHECK_EQ(off.downs(1), 1);
  // a deliberate soft hit on pad 1, 40 ms after a hard one on pad 0 (outside the 10 ms): plays
  defaults(s);
  Run late = scanFor(s, 300, [](int p, float t) {
    if (p == 0) return hit(900, 2, 60, 10, t);
    if (p == 1) return hit(120, 6, 40, 10, t - 40.0f);
    return 0.0f;
  });
  CHECK_EQ(late.downs(0), 1);
  CHECK_EQ(late.downs(1), 1);
}

void testCalibration() {
  PadScanner s;
  defaults(s);
  PadCal c;
  c.on = false;
  s.setCal(3, c);
  Run r = scanFor(s, 100, [](int p, float t) { return p == 3 ? hit(500, 3, 20, 5, t) : 0.0f; });
  CHECK(r.ev.empty());
  // a higher threshold: a 40 uS hit no longer plays, an 80 uS one does
  c = PadCal{};
  c.threshold = 50.0f;
  s.setCal(3, c);
  Run lo = scanFor(s, 100, [](int p, float t) { return p == 3 ? hit(40, 3, 20, 5, t) : 0.0f; });
  CHECK_EQ(lo.downs(3), 0);
  Run hi = scanFor(s, 100, [](int p, float t) { return p == 3 ? hit(300, 3, 20, 5, t) : 0.0f; });
  CHECK_EQ(hi.downs(3), 1);
  // the curve: 0.5 (the default) lifts a soft hit over a linear 1
  PadScanner a, b;
  defaults(a);
  defaults(b);
  PadCal linear;
  linear.curve = 1.0f;
  a.setCal(0, linear);
  PadCal lift;
  lift.curve = 0.5f;
  b.setCal(0, lift);
  auto soft = [](int p, float t) { return p == 0 ? hit(150, 6, 30, 10, t) : 0.0f; };
  const float va = scanFor(a, 100, soft).velocity(0), vb = scanFor(b, 100, soft).velocity(0);
  CHECK(vb > va);
  CHECK(std::fabs(vb - std::sqrt(va)) < 0.01f);
  // sanitize: nonsense in, something usable out
  PadCal bad;
  bad.threshold = -5.0f;
  bad.ceiling = 0.0f;
  bad.curve = 99.0f;
  const PadCal ok = PadScanner::sanitize(bad);
  CHECK(ok.threshold > 0.0f && ok.ceiling > ok.threshold && ok.curve <= 4.0f);
}

// The default curve (pads.h): medium = the force's steepest rise about 100 uS per ms
// (400 uS in 4 ms) at 0.79, note velocity 100 within 5; hard = 160 uS per ms or more
// across the window (640 uS in 4 ms), or the ceiling inside it, at 1 (127); a soft
// tap well below. At four phases
// of the crossing between two scans.
void testDefaultCurve() {
  struct Case {
    const char* what;
    float peak, riseMs;
    int lo, hi;   // the note velocity (pg::ui::padVelocity)
  };
  const Case cases[] = {{"medium", 400, 4, 95, 105},   {"hard 640/4ms", 640, 4, 125, 127},
                        {"hard 1000/2ms", 1000, 2, 127, 127}, {"hard 900/3ms", 900, 3, 125, 127},
                        {"soft", 120, 8, 30, 50}};
  for (const Case& c : cases) {
    int lo = 200, hi = -1;
    for (float off : {0.0f, 0.5f, 1.0f, 1.5f}) {
      PadScanner s;
      defaults(s);
      Run r = scanFor(s, 150, [&](int p, float t) { return p == 0 ? hit(c.peak, c.riseMs, 30, 10, t - off) : 0.0f; });
      CHECK_EQ(r.downs(0), 1);
      const int v = pg::ui::padVelocity(r.velocity(0));
      lo = v < lo ? v : lo;
      hi = v > hi ? v : hi;
    }
    std::printf("  default curve, %-13s (%4.0f uS in %.0f ms): note velocity %d..%d\n", c.what, double(c.peak),
                double(c.riseMs), lo, hi);
    CHECK(lo >= c.lo && hi <= c.hi);
  }
}

// Pressure fills the ring while nobody pops (a stalled main loop): a release and a
// new hit still get in.
void testReserve() {
  PadScanner s;
  defaults(s);
  float x[PadScanner::kPads] = {};
  auto scanG = [&](float g0, float g1) {
    x[0] = PadScanner::readingFor(g0);
    x[1] = PadScanner::readingFor(g1);
    s.scan(x);
  };
  scanG(600, 0);
  scanG(600, 0);   // pad 0 down
  for (int i = 0; i < 300; ++i) scanG(100.0f + float(i % 50) * 10.0f, 0);   // pressure, every scan moving
  CHECK(s.dropped() > 0);
  scanG(0, 0);
  scanG(0, 0);        // pad 0 released (2 scans below)
  scanG(0, 600);
  scanG(0, 600);      // pad 1 hit
  int downs = 0, ups = 0, press = 0;
  PadEvent e;
  while (s.pop(&e)) {
    downs += e.type == PadEvent::Down;
    ups += e.type == PadEvent::Up;
    press += e.type == PadEvent::Pressure;
  }
  std::printf("  ring full of pressure: %d pressure events kept, %u dropped; downs %d, ups %d\n", press,
              unsigned(s.dropped()), downs, ups);
  CHECK_EQ(downs, 2);
  CHECK_EQ(ups, 1);
  CHECK(press <= PadScanner::kRing - PadScanner::kReserve);
}

void testSettings() {
  DaisySettings s;
  s.pads[1].threshold = 22.5f;
  s.pads[2].ceiling = 700.0f;
  s.pads[3].curve = 0.6f;
  s.pads[0].on = false;
  s.padGlobal.windowScans = 3;
  s.padGlobal.xtalkRatio = 0.3f;
  s.displayTurned = true;
  s.touchOrientation = 5;
  s.spiDivider = 4;
  s.padJob = 1;
  CHECK(!DaisySettings{}.potsOn);   // the pots are off unless switched on
  s.potsOn = true;
  uint8_t b[DaisySettings::kBytes];
  CHECK_EQ(s.encode(b, sizeof(b)), DaisySettings::kBytes);
  DaisySettings t;
  CHECK(t.decode(b, sizeof(b)));
  CHECK(t.pads[1].threshold == 22.5f && t.pads[2].ceiling == 700.0f && t.pads[3].curve == 0.6f && !t.pads[0].on);
  CHECK(t.padGlobal.windowScans == 3 && t.padGlobal.xtalkRatio == 0.3f);
  CHECK(t.displayTurned && t.touchOrientation == 5 && t.spiDivider == 4 && t.padJob == 1 && t.potsOn);
  // a version 2 file (the job, no pots byte): read, the pots off
  {
    uint8_t v2[DaisySettings::kBytesV2];
    std::memcpy(v2, b, DaisySettings::kBytesV2 - 4);
    v2[4] = 2;
    v2[5] = uint8_t(DaisySettings::kBytesV2);
    const uint32_t crc = pg::sampler::crc32(v2, DaisySettings::kBytesV2 - 4);
    std::memcpy(v2 + DaisySettings::kBytesV2 - 4, &crc, 4);
    DaisySettings o;
    o.potsOn = true;
    CHECK(o.decode(v2, sizeof(v2)));
    CHECK(o.padJob == 1 && !o.potsOn && o.spiDivider == 4);
  }
  // a version 1 file (no job): read, the job drums
  {
    uint8_t v1[DaisySettings::kBytesV1];
    std::memcpy(v1, b, DaisySettings::kBytesV1 - 4);
    v1[4] = 1;
    v1[5] = uint8_t(DaisySettings::kBytesV1);
    const uint32_t crc = pg::sampler::crc32(v1, DaisySettings::kBytesV1 - 4);
    std::memcpy(v1 + DaisySettings::kBytesV1 - 4, &crc, 4);
    DaisySettings o;
    o.padJob = 1;
    CHECK(o.decode(v1, sizeof(v1)));
    CHECK(o.padJob == 0 && o.pads[1].threshold == 22.5f && o.spiDivider == 4);
  }
  b[20] ^= 1;   // one flipped bit: refused, nothing changed
  DaisySettings u;
  CHECK(!u.decode(b, sizeof(b)));
  CHECK(!u.displayTurned && u.pads[1].threshold == 10.0f);
  CHECK(!u.decode(b, sizeof(b) - 1));
}

}  // namespace

int main() {
  testConductance();
  testSoftHardAndLatency();
  testPhase();
  testSlowPress();
  testHysteresisAndBounce();
  testNoise();
  testFlams();
  testCrosstalk();
  testCalibration();
  testDefaultCurve();
  testReserve();
  testSettings();
  return pgdt::finish("test_daisy_pads");
}
