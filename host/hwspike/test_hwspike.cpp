// Host tests for src/hwspike's portable code: tilt maths, the sample
// resampler, the SVF, the level statistics and the report formatting.
// Exit code 0 = all passed.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "dsp.h"
#include "report.h"
#include "tilt_math.h"

using namespace hws;

namespace {

int g_fail = 0, g_pass = 0;

void check(bool ok, const char* what, double got = 0.0, double want = 0.0) {
  if (ok) {
    ++g_pass;
  } else {
    ++g_fail;
    std::printf("FAIL: %s (got %.6g, want %.6g)\n", what, got, want);
  }
}
void near(double got, double want, double tol, const char* what) { check(std::fabs(got - want) <= tol, what, got, want); }

// Rodrigues rotation of v about unit axis k by angle (radians).
Vec3 rotate(Vec3 v, Vec3 k, float angle) {
  k = vnormalize(k);
  const float c = cosf(angle), s = sinf(angle);
  return vadd(vadd(vscale(v, c), vscale(vcross(k, v), s)), vscale(k, vdot(k, v) * (1.0f - c)));
}

// ---------------------------------------------------------------------------
void testRelFrame() {
  // Reference pose: device held at an arbitrary angle (not flat).
  const Vec3 g0 = vnormalize({0.3f, -0.6f, 0.74f});
  RelFrame f;
  f.set(g0);
  float a, b;
  f.angles(g0, &a, &b);
  near(a, 0, 1e-4, "relframe: reference pose -> A = 0");
  near(b, 0, 1e-4, "relframe: reference pose -> B = 0");
  near(vdot(f.ex(), f.ez()), 0, 1e-5, "relframe: ex perpendicular to ez");
  near(vnorm(f.ey()), 1, 1e-5, "relframe: ey unit");
  // Rotating g about ey by +x degrees moves it towards ex: A = +x, B = 0.
  for (float deg : {-40.0f, -10.0f, 5.0f, 25.0f, 60.0f}) {
    const Vec3 g = rotate(g0, f.ey(), deg * kDegToRad);
    f.angles(g, &a, &b);
    near(a, deg, 1e-3, "relframe: rotation about ey -> A");
    near(b, 0, 1e-3, "relframe: rotation about ey -> B stays 0");
    // about ex by -deg moves it towards +ey
    const Vec3 g2 = rotate(g0, f.ex(), -deg * kDegToRad);
    f.angles(g2, &a, &b);
    near(b, deg, 1e-3, "relframe: rotation about ex -> B");
    near(a, 0, 1e-3, "relframe: rotation about ex -> A stays 0");
  }
  // Reference along body x (the fallback axis is used): still well defined.
  f.set({1.0f, 0.0f, 0.0f});
  f.angles({1.0f, 0.0f, 0.0f}, &a, &b);
  near(a, 0, 1e-4, "relframe: g0 along x -> A = 0");
  near(b, 0, 1e-4, "relframe: g0 along x -> B = 0");
  check(std::isfinite(a) && std::isfinite(b), "relframe: finite for g0 along x");
}

void testAbsTilt() {
  float a, b;
  absTilt({0, 0, 1}, &a, &b);
  near(a, 0, 1e-4, "abstilt: flat A");
  near(b, 0, 1e-4, "abstilt: flat B");
  absTilt({sinf(30 * kDegToRad), 0, cosf(30 * kDegToRad)}, &a, &b);
  near(a, 30, 1e-3, "abstilt: 30 deg about y -> A");
  absTilt({0, sinf(-20 * kDegToRad), cosf(-20 * kDegToRad)}, &a, &b);
  near(b, -20, 1e-3, "abstilt: -20 deg about x -> B");
  near(angleBetweenDeg({0, 0, 1}, {0, sinf(0.01f), cosf(0.01f)}), 0.01 * 57.2957795, 1e-4, "angleBetween small");
}

void testFusion() {
  // Truth: the body turns at w (body frame) for 2 s; a world-fixed vector seen
  // from the body is v(t) = Rot(w_hat, -|w| t) v0. The gyro sees w, the
  // accelerometer sees v(t) plus noise.
  const float fs = 200.0f, dt = 1.0f / fs;
  const Vec3 w = {0.0f, 1.0f, 0.3f};   // rad/s
  const Vec3 v0 = vnormalize({0.1f, 0.2f, 1.0f});
  std::mt19937 rng(1);
  std::normal_distribution<float> noise(0.0f, 0.02f);
  GravityFusion gyroOnly, fused, flipped, accOnly;
  float maxErrGyro = 0, maxErrFused = 0, endErrFlipped = 0;
  float sumAccNoise = 0, sumFusedNoise = 0;
  int n = 0;
  for (int i = 0; i <= static_cast<int>(2.0f * fs); ++i) {
    const float t = static_cast<float>(i) * dt;
    const Vec3 truth = rotate(v0, w, -vnorm(w) * t);
    const Vec3 acc = {truth.x + noise(rng), truth.y + noise(rng), truth.z + noise(rng)};
    if (i == 0) {
      gyroOnly.reset(truth);
      fused.reset(truth);
      flipped.reset(truth);
      accOnly.reset(acc);
      continue;
    }
    gyroOnly.update(acc, w, dt, 0.0f, 1.0f, false);
    fused.update(acc, w, dt, 0.5f, 1.0f, true);
    flipped.update(acc, w, dt, 0.0f, -1.0f, false);
    accOnly.update(acc, w, dt, 0.0f);
    const float eg = angleBetweenDeg(gyroOnly.g(), truth);
    const float ef = angleBetweenDeg(fused.g(), truth);
    if (eg > maxErrGyro) maxErrGyro = eg;
    if (i > 50 && ef > maxErrFused) maxErrFused = ef;
    endErrFlipped = angleBetweenDeg(flipped.g(), truth);
    sumAccNoise += angleBetweenDeg(accOnly.g(), truth);
    sumFusedNoise += ef;
    ++n;
  }
  check(maxErrGyro < 0.5f, "fusion: gyro-only integration tracks a 1 rad/s turn for 2 s (< 0.5 deg)", maxErrGyro, 0.5);
  check(maxErrFused < 1.0f, "fusion: complementary tracks the turn (< 1 deg)", maxErrFused, 1.0);
  check(endErrFlipped > 20.0f, "fusion: a flipped gyro sign diverges (sign convention is tested)", endErrFlipped, 20.0);
  check(sumFusedNoise < 0.5f * sumAccNoise, "fusion: complementary halves accelerometer noise", sumFusedNoise / n,
        sumAccNoise / n);
  // Rest with a gyro bias: accel correction bounds the error.
  GravityFusion f2;
  f2.reset({0, 0, 1});
  for (int i = 0; i < 2000; ++i) f2.update({0, 0, 1}, {0.01f, 0, 0}, dt, 0.5f);
  check(angleBetweenDeg(f2.g(), {0, 0, 1}) < 0.5f, "fusion: bias error bounded at rest");
  // Degenerate input stays finite.
  GravityFusion f3;
  f3.update({0, 0, 0}, {1e6f, 0, 0}, dt, 0.5f);
  f3.update({0, 0, 0}, {1e6f, 0, 0}, dt, 0.5f);
  check(std::isfinite(f3.g().x) && std::isfinite(f3.g().z), "fusion: zero accel / huge gyro finite");
}

void testMapper() {
  const float dt = 1.0f / 200.0f;
  for (int p = 0; p < kNumTiltPresets; ++p) {
    const TiltPreset& pr = tiltPreset(p);
    TiltMapper m;
    m.setPreset(p);
    const Vec3 g0 = vnormalize({0.5f, 0.2f, 0.84f});   // held at an angle
    for (int i = 0; i < 50; ++i) m.update(g0, {0, 0, 0}, dt);
    m.press();
    m.update(g0, {0, 0, 0}, dt);   // frame taken here
    check(std::fabs(m.a()) < 1e-4f && std::fabs(m.b()) < 1e-4f, "mapper: zero at the key-down pose");
    // Tilt by 15 deg along ex of the reference frame (static: gyro 0, so the
    // fused presets converge with their tau).
    RelFrame f;
    f.set(g0);
    const Vec3 g = rotate(g0, f.ey(), 15.0f * kDegToRad);
    for (int i = 0; i < 2000; ++i) m.update(g, {0, 0, 0}, dt);
    const float want = (15.0f - pr.deadDeg) / (pr.rangeDeg - pr.deadDeg);
    near(m.a(), want, 0.01, "mapper: 15 deg -> (15 - dead) / (range - dead)");
    near(m.b(), 0.0, 0.01, "mapper: other axis 0");
    // Beyond the range: clamped.
    const Vec3 gBig = rotate(g0, f.ey(), (pr.rangeDeg + 20.0f) * kDegToRad);
    for (int i = 0; i < 2000; ++i) m.update(gBig, {0, 0, 0}, dt);
    near(m.a(), 1.0, 1e-4, "mapper: clamped at +1");
    m.release();
    for (int i = 0; i < 2000; ++i) m.update(gBig, {0, 0, 0}, dt);
    near(m.a(), 0.0, 1e-3, "mapper: release glides back to 0");
    // Dead zone: a small wobble produces nothing. (Settle at the pose first:
    // without gyro motion the fused presets need their tau to follow a step.)
    for (int i = 0; i < 2000; ++i) m.update(g0, {0, 0, 0}, dt);
    m.press();
    m.update(g0, {0, 0, 0}, dt);
    const Vec3 gSmall = rotate(g0, f.ey(), 0.8f * pr.deadDeg * kDegToRad);
    for (int i = 0; i < 2000; ++i) m.update(gSmall, {0, 0, 0}, dt);
    near(m.a(), 0.0, 1e-6, "mapper: inside the dead zone -> 0");
    check(m.smoothingDelayMs() > 0.0f, "mapper: smoothing delay reported");
  }
}

void testShake() {
  const float fs = 200.0f, dt = 1.0f / fs;
  {
    ShakeDetector s;
    for (int i = 0; i < 200; ++i) s.update({0, 0, 1}, dt);   // settle
    for (int i = 0; i < 200; ++i) {
      const float t = static_cast<float>(i) * dt;
      s.update({2.5f * sinf(2 * kPi * 6.0f * t), 0, 1}, dt);
    }
    check(s.count() >= 1, "shake: 6 Hz +-2.5 g for 1 s is a shake", s.count(), 1);
    check(s.count() <= 3, "shake: ... but only a few", s.count(), 3);
  }
  {
    ShakeDetector s;
    for (int i = 0; i < 400; ++i) {
      const float ang = 60.0f * kDegToRad * static_cast<float>(i) / 400.0f;
      s.update({sinf(ang), 0, cosf(ang)}, dt);
    }
    check(s.count() == 0, "shake: a slow 60 deg tilt is not a shake", s.count(), 0);
  }
  {
    ShakeDetector s;
    for (int i = 0; i < 200; ++i) s.update({0, 0, 1}, dt);
    for (int i = 0; i < 10; ++i) s.update({2.0f * sinf(kPi * static_cast<float>(i) / 10.0f), 0, 1}, dt);
    for (int i = 0; i < 200; ++i) s.update({0, 0, 1}, dt);
    check(s.count() == 0, "shake: a single bump is not a shake", s.count(), 0);
  }
}

void testWindowStd() {
  WindowStd w(100);
  for (int i = 0; i < 100; ++i) w.add(3.0f);
  near(w.last(), 0, 1e-6, "windowstd: constant -> 0");
  for (int i = 0; i < 100; ++i) w.add((i & 1) ? 1.0f : -1.0f);
  near(w.last(), 1, 1e-5, "windowstd: +-1 -> 1");
  near(w.minimum(), 0, 1e-6, "windowstd: minimum over windows");
  check(w.windows() == 2, "windowstd: window count");
}

// ---------------------------------------------------------------------------
void testResampler() {
  uint32_t si, sf;
  ratioToStep(1.0f, &si, &sf);
  check(si == 1 && sf == 0, "ratioToStep 1.0");
  ratioToStep(1.5f, &si, &sf);
  check(si == 1 && sf == 0x80000000u, "ratioToStep 1.5");
  ratioToStep(0.25f, &si, &sf);
  check(si == 0 && sf == 0x40000000u, "ratioToStep 0.25");
  ratioToStep(16.0f, &si, &sf);
  check(si == 16 && sf == 0, "ratioToStep clamps at 16");

  // ratio 1: exact copy, one-shot produces len - 1 samples then stops.
  std::vector<int16_t> src(1000);
  for (size_t i = 0; i < src.size(); ++i) src[i] = static_cast<int16_t>((i * 7919) % 65536 - 32768);
  {
    Voice v;
    voiceStart(v, src.data(), static_cast<uint32_t>(src.size()), 0, 1.0f, 1.0f, false);
    std::vector<int32_t> mix(2048, 0);
    const int got = voiceRender(v, mix.data(), 2048);
    check(got == static_cast<int>(src.size()) - 1, "voice: one-shot at 1.0 produces len - 1 samples", got,
          static_cast<double>(src.size() - 1));
    check(!v.active, "voice: inactive after the end");
    bool same = true;
    for (int i = 0; i < got; ++i) {
      // gain 32767/32768: (y * 32767) >> 15 differs from y by at most 1 LSB
      if (std::abs(mix[i] - src[i]) > 1) same = false;
    }
    check(same, "voice: ratio 1.0 reproduces the sample (within 1 LSB of the Q15 gain)");
  }
  // ratio 0.5 on a ramp: midpoints interpolated.
  {
    std::vector<int16_t> ramp = {0, 100, 200, 300, 400};
    Voice v;
    voiceStart(v, ramp.data(), 5, 0, 0.5f, 1.0f, false);
    int32_t mix[16] = {0};
    const int got = voiceRender(v, mix, 16);
    check(got == 8, "voice: ratio 0.5 on 5 samples -> 8 outputs", got, 8);
    bool ok = true;
    for (int i = 0; i < got; ++i) {
      const int want = 50 * i;
      if (std::abs(mix[i] - want) > 1) ok = false;
    }
    check(ok, "voice: ratio 0.5 interpolates midpoints");
  }
  // Pitch: a 1 kHz sine played at +7 semitones has 1 kHz * 2^(7/12).
  {
    const float fs = 44100.0f;
    std::vector<int16_t> sine(44100);
    for (size_t i = 0; i < sine.size(); ++i)
      sine[i] = static_cast<int16_t>(16000.0f * sinf(2 * kPi * 1000.0f * static_cast<float>(i) / fs));
    Voice v;
    const float r = semitoneRatio(7.0f);
    voiceStart(v, sine.data(), static_cast<uint32_t>(sine.size()), 0, r, 1.0f, false);
    std::vector<int32_t> out(22050, 0);
    voiceRender(v, out.data(), 22050);
    int cross = 0;
    for (size_t i = 1; i < out.size(); ++i)
      if ((out[i - 1] < 0) != (out[i] < 0)) ++cross;
    const double hz = cross / 2.0 / (22050.0 / 44100.0);
    near(hz, 1000.0 * std::pow(2.0, 7.0 / 12.0), 5.0, "voice: +7 semitones -> 1498 Hz");
    // Position accuracy against a double reference.
    Voice p;
    voiceStart(p, sine.data(), static_cast<uint32_t>(sine.size()), 0, r, 1.0f, false);
    int32_t scratch[128];
    for (int b = 0; b < 100; ++b) voiceRender(p, scratch, 128);
    const double pos = p.idx + p.frac / 4294967296.0;
    near(pos, 12800.0 * static_cast<double>(r), 0.01, "voice: 32.32 position matches the ratio after 12800 steps");
  }
  // Loop: wraps and stays active.
  {
    std::vector<int16_t> s(100, 1000);
    Voice v;
    voiceStart(v, s.data(), 100, 50, 1.7f, 0.5f, true);
    std::vector<int32_t> mix(1000, 0);
    const int got = voiceRender(v, mix.data(), 1000);
    check(got == 1000 && v.active, "voice: loop keeps playing");
    check(std::abs(mix[999] - 500) <= 1, "voice: gain 0.5", mix[999], 500);
    check(v.idx < 99, "voice: loop index stays inside the sample", v.idx, 99);
  }
  // Benchmark (informational): 8 voices x 128 frames.
  {
    std::vector<int16_t> big(1 << 19);
    for (size_t i = 0; i < big.size(); ++i) big[i] = static_cast<int16_t>(i * 31);
    Voice vs[8];
    for (int k = 0; k < 8; ++k)
      voiceStart(vs[k], big.data(), static_cast<uint32_t>(big.size()), static_cast<uint32_t>(k * 65536), 1.4983f, 0.25f,
                 true);
    int32_t mix[128];
    const auto t0 = std::chrono::steady_clock::now();
    const int blocks = 20000;
    for (int b = 0; b < blocks; ++b) {
      std::memset(mix, 0, sizeof(mix));
      for (auto& v : vs) voiceRender(v, mix, 128);
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / (blocks * 128.0 * 8.0);
    std::printf("bench: voiceRender %.2f ns per voice-sample (host)\n", ns);
  }
}

void testSvf() {
  Svf f;
  f.set(1000.0f, 0.5f, 44100.0f);
  float y = 0;
  for (int i = 0; i < 20000; ++i) y = f.lp(1.0f);
  near(y, 1.0, 1e-3, "svf: DC gain 1");
  std::mt19937 rng(2);
  std::uniform_real_distribution<float> u(-1.0f, 1.0f);
  bool ok = true;
  for (float fc : {5.0f, 100.0f, 5000.0f, 20000.0f, 1e6f}) {
    for (float r : {-1.0f, 0.0f, 0.9f, 0.98f, 2.0f}) {
      Svf s;
      s.set(fc, r, 44100.0f);
      for (int i = 0; i < 20000; ++i) {
        const float o = s.lp(u(rng));
        if (!std::isfinite(o) || std::fabs(o) > 100.0f) ok = false;
      }
    }
  }
  check(ok, "svf: finite and bounded at every cutoff / resonance extreme");
}

void testLevel() {
  LevelAcc a;
  for (int i = 0; i < 44100; ++i) a.add(static_cast<int16_t>(16384.0f * sinf(2 * kPi * 440.0f * static_cast<float>(i) / 44100.0f)));
  LevelResult r = levelResult(a);
  near(r.peakDb, -6.02, 0.05, "level: half-scale sine peak -6 dBFS");
  near(r.rmsDb, -9.03, 0.05, "level: half-scale sine RMS -9 dBFS");
  check(!r.stuck && r.clips == 0, "level: not stuck, no clips");
  a.clear();
  std::mt19937 rng(3);
  std::normal_distribution<float> nd(0.0f, 3.0f);
  for (int i = 0; i < 44100; ++i) a.add(static_cast<int16_t>(lrintf(-120.0f + nd(rng))));
  r = levelResult(a);
  near(r.dcLsb, -120.0, 0.2, "level: DC offset");
  near(r.acRmsDb, 20.0 * std::log10(3.0 / 32768.0), 0.3, "level: noise floor with the DC removed");
  a.clear();
  for (int i = 0; i < 100; ++i) a.add(-1);
  r = levelResult(a);
  check(r.stuck, "level: constant input is flagged stuck");
  a.clear();
  a.add(32767);
  a.add(-32768);
  a.add(0);
  r = levelResult(a);
  check(r.clips == 2, "level: clips counted", r.clips, 2);
  near(dbfs(0.0f), -120.0, 1e-6, "dbfs floor");
  LevelAcc e;
  r = levelResult(e);
  check(r.n == 0 && r.peakDb <= -119.0f, "level: empty");
}

// ---------------------------------------------------------------------------
struct Collect {
  std::vector<std::string> lines;
};
void collect(const char* line, void* user) { static_cast<Collect*>(user)->lines.emplace_back(line); }

void checkBlock(const Collect& c, const char* begin, const char* end, const char* what) {
  std::string w = what;
  check(!c.lines.empty() && c.lines.front().rfind(begin, 0) == 0, (w + ": BEGIN marker first").c_str());
  check(!c.lines.empty() && c.lines.back() == end, (w + ": END marker last").c_str());
  bool lenOk = true, keyOk = true;
  for (size_t i = 1; i + 1 < c.lines.size(); ++i) {
    const std::string& l = c.lines[i];
    if (l.size() >= static_cast<size_t>(kReportLineMax)) lenOk = false;   // would have been truncated
    const size_t colon = l.find(": ");
    if (colon == std::string::npos || colon == 0 || l.find(' ') < colon) keyOk = false;
  }
  check(lenOk, (w + ": no line truncated").c_str());
  check(keyOk, (w + ": every line is 'key: ...'").c_str());
}

void testReports() {
  {
    TiltReport r;
    Collect c;
    const int n = formatTiltReport(r, collect, &c);
    check(n == static_cast<int>(c.lines.size()), "tilt report: returned line count");
    checkBlock(c, "===== PG-HWSPIKE TILT BEGIN", "===== PG-HWSPIKE TILT END =====", "tilt report (empty)");
    bool sawNa = false;
    for (auto& l : c.lines)
      if (l.rfind("noise:", 0) == 0 && l.find("acc_a_deg=na") != std::string::npos) sawNa = true;
    check(sawNa, "tilt report: unmeasured noise prints na");
    const size_t nEmpty = c.lines.size();
    // Filled with large values: same line count, nothing truncated.
    r.imuOk = true;
    r.addr = 0x69;
    r.chipId = 0x24;
    r.setHz = 400;
    r.pollHz = r.newHz = 399.9f;
    r.noiseAccA = 0.1234f;
    r.driftSecs = 60;
    r.driftGyroDeg = 1.5f;
    r.keysImuOn.n = 123456;
    r.keysImuOn.avgUs = 123456;
    r.keysImuOn.maxUs = 9999999;
    r.keyDowns = r.keyUps = 4000000000u;
    r.heapFree = r.heapMin = 4000000000u;
    for (float& x : r.latSmoothMs) x = 12345.678f;
    Collect c2;
    formatTiltReport(r, collect, &c2);
    check(c2.lines.size() == nEmpty, "tilt report: fixed line count");
    checkBlock(c2, "===== PG-HWSPIKE TILT BEGIN", "===== PG-HWSPIKE TILT END =====", "tilt report (filled)");
    bool sawNoise = false, sawDrift = false;
    for (auto& l : c2.lines) {
      if (l.find("acc_a_deg=0.123 ") != std::string::npos) sawNoise = true;
      if (l.find("gyro_deg_per_min=1.500") != std::string::npos) sawDrift = true;
    }
    check(sawNoise, "tilt report: noise value formatted");
    check(sawDrift, "tilt report: drift per minute computed");
    for (auto& l : c2.lines) std::printf("  %s\n", l.c_str());
  }
  {
    SamplerReport r;
    Collect c;
    formatSamplerReport(r, collect, &c);
    checkBlock(c, "===== PG-HWSPIKE SAMPLER BEGIN", "===== PG-HWSPIKE SAMPLER END =====", "sampler report (empty)");
    const size_t base = c.lines.size();
    r.storeOk = true;
    r.partAddr = 0x340000;
    r.partSize = 0x330000;
    r.regsValid = true;
    for (auto& g : r.gains) {
      g.silValid = g.recValid = true;
      g.silAcRmsDb = g.silPeakDb = g.recPeakDb = g.recRmsDb = -119.9f;
      g.silDcLsb = g.recDcLsb = -32768.0f;
      g.recClips = 4000000000u;
      g.recSecs = 123.45f;
      g.pgaDb = 30;
    }
    static const char* srcs[3] = {"ram48k", "flash48k", "flash1m"};
    r.benchRows = 36;
    for (int i = 0; i < r.benchRows; ++i) {
      r.bench[i].src = srcs[i % 3];
      r.bench[i].voices = 8;
      r.bench[i].pollKb = 64;
      r.bench[i].cycPerSample = 12345.6f;
      r.bench[i].cycPerVoiceSample = 1543.2f;
      r.bench[i].maxBlockCyc = 4000000000u;
      r.bench[i].corePct = 100.0f;
    }
    Collect c2;
    formatSamplerReport(r, collect, &c2);
    check(c2.lines.size() == base + 36, "sampler report: one line per bench row", static_cast<double>(c2.lines.size()),
          static_cast<double>(base + 36));
    checkBlock(c2, "===== PG-HWSPIKE SAMPLER BEGIN", "===== PG-HWSPIKE SAMPLER END =====", "sampler report (filled)");
    bool sawPart = false;
    for (auto& l : c2.lines)
      if (l.find("part=0x340000+0x330000") != std::string::npos) sawPart = true;
    check(sawPart, "sampler report: partition printed");
    for (size_t i = 0; i < 12 && i < c2.lines.size(); ++i) std::printf("  %s\n", c2.lines[i].c_str());
  }
}

}  // namespace

int main() {
  testRelFrame();
  testAbsTilt();
  testFusion();
  testMapper();
  testShake();
  testWindowStd();
  testResampler();
  testSvf();
  testLevel();
  testReports();
  std::printf("test_hwspike: %d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
