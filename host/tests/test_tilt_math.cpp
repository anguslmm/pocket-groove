// pg::tilt (lib/pg/src/pg/tilt, docs/UX.md 10.3 "Tilt"): the maths between the
// IMU and the UI, links the tilt module alone.
//
//   atan2Fast   every quadrant against atan2, max error
//   responses   the four presets by name, unknown names
//   Axis        dead zone (soft, continuous), range (+-1 at the range, clamped),
//               smoothing (one-pole: 63 % after 1 / (2 pi fc)), reset, NaN input
//   Tracker     the pose at the press is the zero at any holding angle (flat, 50
//               degrees toward the player, upside down, on its side); roll and pitch
//               give the right axis with the right sign and the angle within 0.01
//               degrees; orientation (swap, signs); not held = no output change;
//               fused responses: follow a real rotation (gyro and accelerometer
//               agree), reject a linear jolt better than the accelerometer alone, a
//               gyro bias stays within bias * tau; two axes with two responses;
//               cost of one sample on this host.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string_view>

#include "pg/tilt/tilt.h"

using namespace pg::tilt;

namespace {

int gFail = 0, gChecks = 0;
#define CHECK(c, ...)                                         \
  do {                                                        \
    ++gChecks;                                                \
    if (!(c)) {                                               \
      ++gFail;                                                \
      std::printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #c); \
      std::printf(__VA_ARGS__);                               \
      std::printf("\n");                                      \
    }                                                         \
  } while (0)

constexpr float kPi = 3.14159265f;
constexpr float kD2R = kPi / 180.0f;

// Rotations in the body frame (chip axes: x to the right, y to the far edge, z out
// of the keys; the accelerometer reads "up").
Vec3 rotY(Vec3 v, float a) {   // about the body y axis
  const float c = cosf(a), s = sinf(a);
  return {c * v.x + s * v.z, v.y, -s * v.x + c * v.z};
}
Vec3 rotX(Vec3 v, float a) {
  const float c = cosf(a), s = sinf(a);
  return {v.x, c * v.y - s * v.z, s * v.y + c * v.z};
}
Vec3 cross3(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
// "Up" with the device held at b radians toward the player (far edge up; 0 = flat on
// the table), then rolled right by r (right side down) and tipped away by p (far edge
// down), both relative to that pose: up leans toward -x, then toward -y of the pose.
Vec3 upAfter(float b, float r, float p) {
  const Vec3 ez = rotX(Vec3{0.0f, 0.0f, 1.0f}, -b);   // (0, sin b, cos b)
  const Vec3 ex = {1.0f, 0.0f, 0.0f};
  const Vec3 ey = cross3(ez, ex);
  const float cr = cosf(r), sr = sinf(r), cp = cosf(p), sp = sinf(p);
  return {ez.x * cr * cp - ex.x * sr * cp - ey.x * sp, ez.y * cr * cp - ex.y * sr * cp - ey.y * sp,
          ez.z * cr * cp - ex.z * sr * cp - ey.z * sp};
}

Tracker makeTracker(int rx, int ry, float hz = 50.0f) {
  Tracker t;
  t.setRate(hz);
  t.setOrientation(false, -1.0f, -1.0f);   // the firmware's assumption (src/app/tilt_input.cpp)
  t.press(rx, ry);
  return t;
}

void testAtan2() {
  std::printf("atan2Fast\n");
  float worst = 0.0f;
  for (int i = 0; i < 3600; ++i) {
    const float a = (float(i) - 1800.0f) * 0.1f * kD2R;
    for (float r : {0.001f, 1.0f, 1000.0f}) {
      const float y = r * sinf(a), x = r * cosf(a);
      float e = std::fabs(atan2Fast(y, x) - atan2f(y, x));
      if (e > kPi) e = 2.0f * kPi - e;   // +-pi are the same angle
      if (e > worst) worst = e;
    }
  }
  CHECK(worst < 2e-5f, "max error %.2e rad", double(worst));
  CHECK(atan2Fast(0.0f, 0.0f) == 0.0f, "(0,0)");
  CHECK(std::fabs(atan2Fast(1.0f, 0.0f) - kPi / 2) < 1e-5f, "+y");
  CHECK(std::fabs(atan2Fast(0.0f, -1.0f) - kPi) < 1e-5f, "-x");
  std::printf("  max error %.2e rad (%.5f degrees)\n", double(worst), double(worst / kD2R));
}

void testResponses() {
  std::printf("responses\n");
  const char* names[] = {"fast", "smooth", "fine", "wide"};
  for (int r = 0; r < kNumResponses; ++r) {
    CHECK(responseFromName(names[r]) == r, "%s", names[r]);
    CHECK(std::string_view(responseSpec(r).name) == names[r], "%s", names[r]);
  }
  CHECK(responseFromName("SMOOTH") == kSmooth, "case");
  CHECK(responseFromName("slow") == -1 && responseFromName("") == -1 && responseFromName(nullptr) == -1, "unknown");
  CHECK(!responseSpec(kFast).fused && !responseSpec(kSmooth).fused && responseSpec(kFine).fused &&
            responseSpec(kWide).fused, "sources");
  CHECK(responseSpec(kSmooth).rangeDeg == 35.0f && responseSpec(kWide).rangeDeg == 60.0f, "ranges");
}

void testAxis() {
  std::printf("Axis\n");
  Axis a;
  // no smoothing to speak of: dt 1 s
  a.setup(kSmooth, 1.0f);
  CHECK(a.update(1.5f) == 0.0f, "inside the dead zone: 0");
  CHECK(a.update(-1.4f) == 0.0f, "inside the dead zone: 0 (%f)", double(a.out()));
  CHECK(std::fabs(a.update(1.6f) - 0.1f / 33.5f) < 1e-5f, "just outside: continuous (%f)", double(a.out()));
  CHECK(std::fabs(a.update(35.0f) - 1.0f) < 1e-5f, "at the range: 1 (%f)", double(a.out()));
  CHECK(a.update(80.0f) == 1.0f && a.update(-80.0f) == -1.0f, "clamped");
  CHECK(std::fabs(a.update(-18.25f) + 0.5f) < 1e-5f, "half way, negative (%f)", double(a.out()));
  a.reset();
  CHECK(a.out() == 0.0f && a.smoothedDeg() == 0.0f, "reset");
  CHECK(a.update(NAN) == 0.0f, "NaN is the press pose");
  // smoothing: a step of 30 degrees at 50 Hz, smooth = 6 Hz: 63 % after 1 / (2 pi 6) = 26.5 ms
  a.setup(kSmooth, 0.02f);
  a.update(30.0f);
  const float after1 = a.smoothedDeg();
  CHECK(after1 > 30.0f * 0.5f && after1 < 30.0f * 0.6f, "one 20 ms step of a 6 Hz one-pole: 53 %% (%f)", double(after1));
  for (int i = 0; i < 20; ++i) a.update(30.0f);
  CHECK(std::fabs(a.smoothedDeg() - 30.0f) < 0.01f, "settled (%f)", double(a.smoothedDeg()));
  // fast: 20 Hz: nearly the input after one 20 ms step
  a.setup(kFast, 0.02f);
  a.update(30.0f);
  CHECK(a.smoothedDeg() > 30.0f * 0.9f, "fast follows (%f)", double(a.smoothedDeg()));
}

void testTrackerAngles() {
  std::printf("Tracker: angles\n");
  // Base poses: flat, held at 50 degrees toward the player, nearly vertical, upside down.
  for (float baseDeg : {0.0f, 50.0f, 85.0f, 180.0f}) {
    for (float rollDeg : {-40.0f, -10.0f, 0.0f, 12.5f, 30.0f}) {
      Tracker t = makeTracker(kFast, kFast);
      const float b = baseDeg * kD2R;
      t.sample(upAfter(b, 0.0f, 0.0f), Vec3{0, 0, 0});   // the press pose
      CHECK(t.x() == 0.0f && t.y() == 0.0f, "zero at the press (base %.0f)", double(baseDeg));
      t.sample(upAfter(b, rollDeg * kD2R, 0.0f), Vec3{0, 0, 0});
      CHECK(std::fabs(t.relXDeg() - rollDeg) < 0.01f, "roll %.1f at base %.0f: X %.4f", double(rollDeg), double(baseDeg),
            double(t.relXDeg()));
      CHECK(std::fabs(t.relYDeg()) < 0.01f, "roll %.1f at base %.0f: Y stays 0 (%.4f)", double(rollDeg), double(baseDeg),
            double(t.relYDeg()));
    }
    for (float pitchDeg : {-30.0f, -5.0f, 7.0f, 25.0f}) {
      Tracker t = makeTracker(kSmooth, kSmooth);
      const float b = baseDeg * kD2R;
      t.sample(upAfter(b, 0.0f, 0.0f), Vec3{0, 0, 0});
      t.sample(upAfter(b, 0.0f, pitchDeg * kD2R), Vec3{0, 0, 0});
      CHECK(std::fabs(t.relYDeg() - pitchDeg) < 0.01f, "pitch %.1f at base %.0f: Y %.4f", double(pitchDeg),
            double(baseDeg), double(t.relYDeg()));
      CHECK(std::fabs(t.relXDeg()) < 0.01f, "pitch at base %.0f: X stays 0 (%.4f)", double(baseDeg), double(t.relXDeg()));
    }
  }
  // the device on its side (chip x vertical): the frame falls back to chip y
  {
    Tracker t = makeTracker(kFast, kFast);
    const Vec3 side = {1.0f, 0.0f, 0.0f};
    t.sample(side, Vec3{0, 0, 0});
    t.sample(rotY(side, 0.3f), Vec3{0, 0, 0});
    CHECK(std::isfinite(t.relXDeg()) && std::isfinite(t.relYDeg()) && std::fabs(t.relXDeg()) + std::fabs(t.relYDeg()) > 10.0f,
          "on its side: still a finite angle (%f %f)", double(t.relXDeg()), double(t.relYDeg()));
  }
  // orientation: swap and signs
  {
    Tracker t;
    t.setRate(50.0f);
    t.setOrientation(true, 1.0f, 1.0f);
    t.press(kFast, kFast);
    t.sample(upAfter(0.0f, 0.0f, 0.0f), Vec3{0, 0, 0});
    t.sample(upAfter(0.0f, 20.0f * kD2R, 0.0f), Vec3{0, 0, 0});
    CHECK(std::fabs(t.relYDeg() + 20.0f) < 0.01f && std::fabs(t.relXDeg()) < 0.01f,
          "swapped, signs +: a roll shows on Y with the chip's sign (%f %f)", double(t.relXDeg()), double(t.relYDeg()));
  }
  // not held: nothing changes; a new press starts from its own pose
  {
    Tracker t = makeTracker(kFast, kFast);
    t.sample(upAfter(0.0f, 0.0f, 0.0f), Vec3{0, 0, 0});
    for (int i = 0; i < 10; ++i) t.sample(upAfter(0.0f, 0.5f, 0.0f), Vec3{0, 0, 0});
    const float x = t.x();
    CHECK(x > 0.9f, "rolled 29 degrees right: X near 1 (%f)", double(x));
    t.release();
    t.sample(upAfter(0.0f, -0.5f, 0.0f), Vec3{0, 0, 0});
    CHECK(t.x() == x && !t.held(), "released: ignored");
    t.press(kFast, kFast);
    t.sample(upAfter(0.0f, -0.5f, 0.0f), Vec3{0, 0, 0});
    t.sample(upAfter(0.0f, -0.5f, 0.0f), Vec3{0, 0, 0});
    CHECK(std::fabs(t.relXDeg()) < 0.01f && t.x() == 0.0f, "a new press: its pose is the zero (%f)", double(t.relXDeg()));
  }
}

void testTrackerFused() {
  std::printf("Tracker: fused responses\n");
  const float hz = 50.0f, dt = 1.0f / hz;
  // A real rotation: roll right at 60 deg/s for 0.5 s; gyro and accelerometer agree.
  // The chip rotates about its y axis; a world vector changes in the body frame as
  // dv/dt = -w x v: up = (-sin r, 0, cos r) needs w = (0, +rate, 0).
  for (int r : {kFine, kWide}) {
    Tracker t = makeTracker(r, r, hz);
    t.sample(upAfter(0.0f, 0.0f, 0.0f), Vec3{0, 0, 0});
    const float rate = 60.0f * kD2R;
    for (int i = 1; i <= 25; ++i) t.sample(upAfter(0.0f, rate * dt * float(i), 0.0f), Vec3{0.0f, rate, 0.0f});
    CHECK(std::fabs(t.relXDeg() - 30.0f) < 0.5f, "%s: follows a real rotation to 30 degrees (%f)",
          responseSpec(r).name, double(t.relXDeg()));
    for (int i = 0; i < 100; ++i) t.sample(upAfter(0.0f, 0.5236f, 0.0f), Vec3{0, 0, 0});
    CHECK(std::fabs(t.relXDeg() - 30.0f) < 0.05f, "%s: settles on the accelerometer (%f)", responseSpec(r).name,
          double(t.relXDeg()));
  }
  // A jolt: 0.3 g sideways for 3 samples, no rotation. The fused estimate barely moves.
  {
    Tracker acc = makeTracker(kFast, kFast, hz), fus = makeTracker(kFine, kFine, hz);
    float worstA = 0.0f, worstF = 0.0f;
    for (int i = 0; i < 20; ++i) {
      Vec3 a = upAfter(0.0f, 0.0f, 0.0f);
      if (i >= 5 && i < 8) a.x += 0.3f;
      acc.sample(a, Vec3{0, 0, 0});
      fus.sample(a, Vec3{0, 0, 0});
      worstA = std::fmax(worstA, std::fabs(acc.relXDeg()));
      worstF = std::fmax(worstF, std::fabs(fus.relXDeg()));
    }
    CHECK(worstF < 0.25f * worstA, "a jolt: fused %.2f degrees vs accelerometer %.2f", double(worstF), double(worstA));
  }
  // A gyro bias of 1 deg/s, device still: the error stays within bias * tau.
  for (int r : {kFine, kWide}) {
    Tracker t = makeTracker(r, r, hz);
    float worst = 0.0f;
    for (int i = 0; i < 500; ++i) {
      t.sample(upAfter(0.4f, 0.0f, 0.0f), Vec3{0.0f, 1.0f * kD2R, 0.0f});
      worst = std::fmax(worst, std::fabs(t.relXDeg()));
    }
    const float bound = responseSpec(r).tau * 1.0f * 1.05f;
    CHECK(worst <= bound && worst < responseSpec(r).deadDeg, "%s: bias 1 dps -> %.3f degrees (bound %.3f), inside the dead zone",
          responseSpec(r).name, double(worst), double(bound));
    CHECK(t.x() == 0.0f, "%s: no output from the bias", responseSpec(r).name);
  }
  // Two axes, two responses (X fast from the accelerometer, Y wide fused), one pose.
  {
    Tracker t = makeTracker(kFast, kWide, hz);
    t.sample(upAfter(0.2f, 0.0f, 0.0f), Vec3{0, 0, 0});
    for (int i = 0; i < 600; ++i) t.sample(upAfter(0.2f, 0.3f, 0.2f), Vec3{0, 0, 0});   // wide: tau 0.8 s
    // rolled 17.2 degrees and tipped 11.5: X = r exactly, Y = atan(tan p / cos r)
    const float wantY = atan2f(sinf(0.2f), cosf(0.3f) * cosf(0.2f)) / kD2R;
    CHECK(std::fabs(t.relXDeg() - 17.1887f) < 0.01f && std::fabs(t.relYDeg() - wantY) < 0.01f,
          "both axes (%f %f, want 17.19 %f)", double(t.relXDeg()), double(t.relYDeg()), double(wantY));
    CHECK(t.x() > 0.5f && t.y() > 0.1f && t.y() < 0.2f, "outputs per response (%f %f)", double(t.x()), double(t.y()));
  }
}

void testCost() {
  std::printf("cost\n");
  Tracker t = makeTracker(kFine, kWide, 50.0f);
  Vec3 up = upAfter(0.3f, 0.0f, 0.0f);
  t.sample(up, Vec3{0, 0, 0});
  float sink = 0.0f;
  const int n = 200000;
  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < n; ++i) {
    up = upAfter(0.3f, 0.001f * float(i & 255), 0.0f);
    t.sample(up, Vec3{0.01f, -0.02f, 0.0f});
    sink += t.x();
  }
  const auto t1 = std::chrono::steady_clock::now();
  const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / n;
  std::printf("  one sample (two fusions, two axes, incl. the test's rotation): %.0f ns on this host (sink %g)\n", ns,
              double(sink));
}

}  // namespace

int main() {
  testAtan2();
  testResponses();
  testAxis();
  testTrackerAngles();
  testTrackerFused();
  testCost();
  std::printf("test_tilt_math: %d checks, %d failed\n", gChecks, gFail);
  return gFail ? 1 : 0;
}
