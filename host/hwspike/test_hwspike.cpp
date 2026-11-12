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
#include "spectrum.h"
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

// ---------------------------------------------------------------------------
// Tone table / generator / detector / verdict (mic matrix)
ToneResult detect(const ToneTable& t, const std::vector<int16_t>& x) {
  ToneDetector d;
  d.begin(&t);
  for (int16_t v : x) d.add(v);
  return d.result();
}

void testTone() {
  ToneTable t;
  check(t.init(1000, 44100) && t.len == 441, "tone: 1 kHz at 44.1 kHz repeats every 441 samples", t.len, 441);
  check(t.init(1000, 16000) && t.len == 16, "tone: 1 kHz at 16 kHz repeats every 16 samples", t.len, 16);
  check(t.init(1000, 32000) && t.len == 32, "tone: 1 kHz at 32 kHz repeats every 32 samples", t.len, 32);
  check(!t.init(997, 44100) && t.len == 0, "tone: a table longer than kToneTableMax is refused");
  check(!t.init(30000, 44100), "tone: above Nyquist refused");
  check(t.init(1000, 44100), "tone: re-init");

  // the generator's table is a sine at exactly 1 kHz
  {
    ToneGen g;
    g.start(&t, 16384.0f);
    double maxErr = 0;
    for (int n = 0; n < 2000; ++n) {
      const double want = 16384.0 * std::sin(2.0 * 3.14159265358979 * 1000.0 * n / 44100.0);
      const double e = std::fabs(static_cast<double>(g.next()) - want);
      if (e > maxErr) maxErr = e;
    }
    check(maxErr <= 1.0, "tone: generator = 16384 sin(2 pi 1000 n / 44100) within rounding", maxErr, 1.0);
  }
  const int N = 441 * 50;   // what the matrix captures: 0.5 s, whole periods
  // pure tone, arbitrary phase and DC: amplitude exact, residual ~ 0
  {
    std::vector<int16_t> x(N);
    for (int n = 0; n < N; ++n)
      x[n] = static_cast<int16_t>(std::lrint(-37.0 + 1000.0 * std::sin(2.0 * 3.14159265358979 * 1000.0 * n / 44100.0 + 0.7)));
    const ToneResult r = detect(t, x);
    near(r.toneAmpLsb, 1000.0, 0.5, "tone: amplitude of a pure 1 kHz sine");
    near(r.toneDb, 20.0 * std::log10(1000.0 / 32768.0), 0.01, "tone: level in dBFS");
    near(r.dcLsb, -37.0, 0.05, "tone: DC");
    check(r.snrDb > 40.0f, "tone: pure sine SNR > 40 dB (rounding only)", r.snrDb, 40);
    const MicVerdict v = judgeMic(r);
    check(v.alive && v.tone, "tone: pure sine judged alive + tone");
  }
  // tone 20 dB under white noise: found, SNR about right (bin is exact)
  {
    std::mt19937 rng(7);
    std::normal_distribution<double> nd(0.0, 300.0);
    std::vector<int16_t> x(N);
    const double a = 300.0 * std::sqrt(2.0) * 0.1;   // tone power = noise power / 100 -> -20 dB
    for (int n = 0; n < N; ++n)
      x[n] = static_cast<int16_t>(std::lrint(nd(rng) + a * std::sin(2.0 * 3.14159265358979 * 1000.0 * n / 44100.0)));
    const ToneResult r = detect(t, x);
    near(r.snrDb, -20.0, 1.5, "tone: SNR of a tone 20 dB below white noise");
    near(r.toneAmpLsb, a, a * 0.25, "tone: amplitude under noise");
    const MicVerdict v = judgeMic(r);
    check(v.alive && !v.tone, "tone: buried tone -> alive but no tone");
  }
  // tone 15 dB above noise at a low level (-60 dBFS): detected
  {
    std::mt19937 rng(11);
    std::normal_distribution<double> nd(0.0, 5.0);
    std::vector<int16_t> x(N);
    const double a = 32.8 * std::sqrt(2.0);   // ~ -60 dBFS RMS
    for (int n = 0; n < N; ++n)
      x[n] = static_cast<int16_t>(std::lrint(nd(rng) + a * std::sin(2.0 * 3.14159265358979 * 1000.0 * n / 44100.0)));
    const ToneResult r = detect(t, x);
    check(r.snrDb > 10.0f && r.snrDb < 20.0f, "tone: quiet tone SNR ~16 dB", r.snrDb, 16);
    const MicVerdict v = judgeMic(r);
    check(v.alive && v.tone, "tone: quiet tone above noise is detected");
  }
  // other frequencies do not leak into the 1 kHz bin
  for (double f : {500.0, 900.0, 1100.0, 3000.0}) {
    std::vector<int16_t> x(N);
    for (int n = 0; n < N; ++n)
      x[n] = static_cast<int16_t>(std::lrint(8000.0 * std::sin(2.0 * 3.14159265358979 * f * n / 44100.0)));
    const ToneResult r = detect(t, x);
    check(r.toneDb < -60.0f && !judgeMic(r).tone, "tone: a sine at another frequency is not the tone", r.toneDb, -60);
  }
  // the device's failure: every sample -1 -> stuck, dead
  {
    std::vector<int16_t> x(N, -1);
    const ToneResult r = detect(t, x);
    check(r.stuck && r.mn == -1 && r.mx == -1, "tone: constant -1 is stuck");
    near(r.dcLsb, -1.0, 1e-9, "tone: constant -1 DC");
    check(r.acRmsDb <= -119.0f && r.toneDb <= -119.0f, "tone: constant has no AC and no tone");
    const MicVerdict v = judgeMic(r);
    check(!v.alive && !v.tone, "tone: constant judged dead");
  }
  // a powered-down modulator behind the DC filter: toggles -1 / 0 -> not alive
  {
    std::vector<int16_t> x(N);
    for (int n = 0; n < N; ++n) x[n] = static_cast<int16_t>((n * 7919) % 3 == 0 ? 0 : -1);
    const MicVerdict v = judgeMic(detect(t, x));
    check(!v.alive, "tone: 1-LSB toggling judged dead");
  }
  // ranking order: tone > alive > dead, ties keep config order
  {
    MicMatrixReport r;
    r.n = 4;
    const float scores[4] = {judgeMic(detect(t, std::vector<int16_t>(N, -1))).score, 1000.0f, 1550.0f, 1000.0f};
    for (int i = 0; i < 4; ++i) {
      r.cfg[i].ran = true;
      r.cfg[i].score = scores[i];
      r.cfg[i].v[0].alive = i != 0;
    }
    rankMicMatrix(r);
    check(r.rank[0] == 2 && r.rank[1] == 1 && r.rank[2] == 3 && r.rank[3] == 0, "matrix: rank by score, stable");
    check(r.best == 2, "matrix: best = top working config", r.best, 2);
    char buf[48];
    micMatrixVerdict(r, buf, sizeof(buf));
    check(std::string(buf) == "MIC WORKS (config 2)", "matrix: verdict text when one works");
    for (int i = 0; i < 4; ++i) {
      r.cfg[i].v[0].alive = false;
      r.cfg[i].score = -5.0f;
    }
    r.cfg[3].ran = false;
    rankMicMatrix(r);
    check(r.best == -1, "matrix: no working config -> best -1");
    micMatrixVerdict(r, buf, sizeof(buf));
    check(std::string(buf) == "MIC DEAD IN ALL 3 CONFIGS", "matrix: dead verdict counts the configs that ran");
  }
}

void testMatrixReport() {
  MicMatrixReport r;
  Collect c0;
  const int n0 = formatMicMatrixReport(r, collect, &c0);
  checkBlock(c0, "===== PG-HWSPIKE MICMATRIX BEGIN", "===== PG-HWSPIKE MICMATRIX END =====", "matrix report (empty)");
  check(n0 == static_cast<int>(c0.lines.size()), "matrix report: returns its line count");
  // full: every config ran, worst-case numbers everywhere
  r.n = kMaxMicCfgs;
  r.pinBoot.valid = r.pinAfterI2s.valid = true;
  r.pinBoot.oe = r.pinBoot.out = r.pinBoot.ie = r.pinBoot.level = 1;
  r.pinBoot.outSel = 0xFFF;
  r.pinBoot.inSel = 0xFF;
  r.pinTestValid = true;
  for (float& o : r.pinOnes) o = 1.0f;
  r.pinVerdict = "esp_drove_din_itself+codec_drives_line";
  r.chipValid = true;
  r.regsBeforeValid = r.regsAfterValid = true;
  for (int i = 0; i < kNumCodecRegs; ++i) r.regsBefore[i] = r.regsAfter[i] = 0xFF;
  for (int i = 0; i < kMaxMicCfgs; ++i) {
    MicCfgRow& c = r.cfg[i];
    c.name = "esp_codec_dev+dacref-and-a-long-name";
    c.ran = true;
    c.m5mic = (i % 5) == 4;
    c.slotBits = 32;
    c.din = 3;
    c.rate = 44100;
    c.regsValid = true;
    c.score = -1234567.0f;
    for (auto& s : c.res) {
      s.stuck = true;
      s.mn = -32768;
      s.mx = 32767;
      s.dcLsb = -32768.0f;
      s.acRmsDb = s.peakDb = s.toneDb = s.residualDb = s.snrDb = -120.0f;
    }
  }
  rankMicMatrix(r);
  Collect c1;
  formatMicMatrixReport(r, collect, &c1);
  checkBlock(c1, "===== PG-HWSPIKE MICMATRIX BEGIN", "===== PG-HWSPIKE MICMATRIX END =====", "matrix report (full)");
  bool sawDead = false, sawRank = false, sawPin = false;
  for (auto& l : c1.lines) {
    if (l.find("result: MIC DEAD IN ALL " + std::to_string(kMaxMicCfgs) + " CONFIGS") != std::string::npos) sawDead = true;
    if (l.rfind("rank:", 0) == 0) sawRank = true;
    if (l.find("pin_test: as_found=1.00") != std::string::npos) sawPin = true;
  }
  check(sawDead && sawRank && sawPin, "matrix report: verdict, rank and pin test lines present");
  for (size_t i = 0; i < 14 && i < c1.lines.size(); ++i) std::printf("  %s\n", c1.lines[i].c_str());
}

// ---------------------------------------------------------------------------
// Second matrix run: microphone slots only, register diffs
void testMicSlots() {
  ToneTable t;
  t.init(1000, 44100);
  const int N = 441 * 50;
  std::vector<int16_t> ref(N), mic(N);
  std::mt19937 rng(3);
  std::normal_distribution<double> nd(0.0, 60.0);
  for (int n = 0; n < N; ++n) {
    const double ph = 2.0 * 3.14159265358979 * 1000.0 * n / 44100.0;
    ref[n] = static_cast<int16_t>(std::lrint(8192.0 * std::sin(ph)));                   // DAC reference: exactly -12 dBFS
    mic[n] = static_cast<int16_t>(std::lrint(1780.0 * std::sin(ph + 1.0) + nd(rng)));   // ~ -25 dBFS, ~30 dB SNR
  }
  ToneResult res[2] = {detect(t, mic), detect(t, ref)};
  const float stim = -12.041f;
  check(looksLikeDacRef(res[1], stim), "slots: the stimulus itself is recognised as the DAC reference");
  check(!looksLikeDacRef(res[0], stim), "slots: the microphone through the speaker is not");
  bool ok[2];
  micSlots(true, 0x08, false, res, true, stim, ok);
  check(ok[0] && !ok[1], "slots: 0x44 = 0x08 but R is the stimulus -> only L counts");
  micSlots(true, 0x58, false, res, false, stim, ok);
  check(ok[0] && !ok[1], "slots: 0x44 = 0x58 routes DACR to R -> only L counts (no tone needed)");
  micSlots(true, 0x48, false, res, false, stim, ok);
  check(!ok[0] && ok[1], "slots: 0x44 = 0x48 routes DACL to L -> only R counts");
  micSlots(false, 0, true, res, false, stim, ok);
  check(ok[0] && !ok[1], "slots: loopR flag excludes R");
  ToneResult two[2] = {res[0], res[0]};
  micSlots(true, 0x08, false, two, true, stim, ok);
  check(ok[0] && ok[1], "slots: both slots the ADC -> both count");

  // The device's config 8: L dead, R the DAC reference at 63.8 dB SNR. It must not win.
  MicMatrixReport r;
  r.n = 3;
  std::vector<int16_t> dead(N);
  for (int n = 0; n < N; ++n) dead[n] = static_cast<int16_t>((n * 7919) % 3 == 0 ? 0 : -1);
  // cfg0: the mic in both slots; cfg1: L dead, R = reference (r44 = 0x58); cfg2: same, r44 back to 0x08
  ToneResult in0[2] = {detect(t, mic), detect(t, mic)};
  ToneResult in1[2] = {detect(t, dead), detect(t, ref)};
  const ToneResult* ins[3] = {in0, in1, in1};
  const uint8_t r44[3] = {0x08, 0x58, 0x08};
  for (int i = 0; i < 3; ++i) {
    MicCfgRow& c = r.cfg[i];
    c.ran = true;
    c.tone = true;
    c.regsValid = true;
    c.r44 = r44[i];
    for (int s = 0; s < 2; ++s) {
      c.res[s] = ins[i][s];
      c.v[s] = judgeMic(c.res[s]);
    }
    micSlots(c.regsValid, c.r44, false, c.res, c.tone, stim, c.micOk);
    scoreMicRow(c);
  }
  check(r.cfg[1].v[1].score > r.cfg[0].v[0].score, "slots: the reference alone out-scores the mic (the bug)");
  rankMicMatrix(r);
  check(r.rank[0] == 0 && r.best == 0, "slots: ranking picks the real microphone, not the DAC reference", r.best, 0);
  check(r.cfg[1].bestSlot == 0 && !r.cfg[1].micOk[1], "slots: a dead mic slot stays the config's best slot");
  check(!r.cfg[2].micOk[1], "slots: stimulus-level R excluded even with 0x44 = 0x08");
  MicCfgRow none;
  none.ran = true;
  none.micOk[0] = none.micOk[1] = false;
  none.v[0] = none.v[1] = judgeMic(detect(t, mic));
  scoreMicRow(none);
  check(none.score < judgeMic(detect(t, std::vector<int16_t>(N, -1))).score, "slots: no mic slot scores below dead");
}

void testDiffLine() {
  MicMatrixReport r;
  r.n = 2;
  r.cfg[0].ran = r.cfg[1].ran = true;
  r.cfg[0].diffValid = true;
  r.cfg[0].nDiff = 2;
  r.cfg[0].diffReg[0] = 0x15;
  r.cfg[0].diffVal[0] = 0x00;
  r.cfg[0].diffReg[1] = 0x44;
  r.cfg[0].diffVal[1] = 0x58;
  r.cfg[1].diffValid = true;
  r.cfg[1].nDiff = 50;   // more than stored and shown
  for (int i = 0; i < kMaxRegDiffs; ++i) {
    r.cfg[1].diffReg[i] = static_cast<uint8_t>(i);
    r.cfg[1].diffVal[i] = 0xFF;
  }
  Collect c;
  formatMicMatrixReport(r, collect, &c);
  checkBlock(c, "===== PG-HWSPIKE MICMATRIX BEGIN", "===== PG-HWSPIKE MICMATRIX END =====", "matrix report (diffs)");
  bool saw0 = false, saw1 = false, sawBase = false;
  for (auto& l : c.lines) {
    if (l == "cfg00_diff: n=2 15=00 44=58") saw0 = true;
    if (l.rfind("cfg01_diff: n=50 00=FF", 0) == 0 && l.find(" +24") != std::string::npos) saw1 = true;
    if (l.rfind("baseline:", 0) == 0) sawBase = true;
  }
  check(saw0, "matrix report: diff line lists reg=value");
  check(saw1, "matrix report: long diff truncated with +N");
  check(sawBase, "matrix report: baseline line");
}

// ---------------------------------------------------------------------------
// Harmonics / THD (quality measurement)
void testHarmonics() {
  ToneTable t;
  t.init(1000, 44100);
  const int N = 44100;   // 1 s = 100 table lengths
  const double fa = 4000.0, a2 = 40.0, a3 = 20.0, a5 = 4.0;
  std::mt19937 rng(5);
  std::normal_distribution<double> nd(0.0, 4.0);
  std::vector<int16_t> x(N);
  for (int n = 0; n < N; ++n) {
    const double w = 2.0 * 3.14159265358979 * 1000.0 * n / 44100.0;
    x[n] = static_cast<int16_t>(std::lrint(12.0 + fa * std::sin(w + 0.3) + a2 * std::sin(2 * w + 1.1) +
                                           a3 * std::cos(3 * w) + a5 * std::sin(5 * w + 2.0) + nd(rng)));
  }
  HarmonicDetector d;
  d.begin(&t);
  for (int16_t v : x) d.add(v);
  const HarmonicResult r = d.result();
  near(r.fundDb, 20.0 * std::log10(fa / 32768.0), 0.02, "harm: fundamental level");
  near(r.harmDb[2], 20.0 * std::log10(a2 / 32768.0), 0.3, "harm: 2nd harmonic level");
  near(r.harmDb[3], 20.0 * std::log10(a3 / 32768.0), 0.5, "harm: 3rd harmonic level");
  near(r.harmDb[5], 20.0 * std::log10(a5 / 32768.0), 2.0, "harm: 5th harmonic level");
  check(r.harmDb[4] < -85.0f, "harm: absent 4th harmonic is at the noise", r.harmDb[4], -85);
  const double thd = std::sqrt(a2 * a2 + a3 * a3 + a5 * a5) / fa;
  near(r.thdPct, 100.0 * thd, 0.03, "harm: THD percent");
  near(r.thdDb, 20.0 * std::log10(thd), 0.2, "harm: THD dB");
  near(r.noiseDb, 20.0 * std::log10(4.0 / 32768.0), 0.5, "harm: noise without tone and harmonics");
  near(r.snrDb, 10.0 * std::log10((fa * fa / 2) / 16.0), 0.5, "harm: SNR without harmonics");
  check(r.sinadDb < r.snrDb && r.sinadDb > 0, "harm: SINAD below SNR when harmonics exist");
  near(r.dcLsb, 12.0, 0.1, "harm: DC");
  check(r.nHarm == 5, "harm: five harmonics below Nyquist at 44.1 kHz", r.nHarm, 5);
  const ToneResult tr = detect(t, x);
  near(tr.snrDb, r.sinadDb, 0.05, "harm: the matrix's tone snr == sinad");
  ToneTable t16;
  t16.init(3000, 16000);
  HarmonicDetector d16;
  d16.begin(&t16);
  for (int n = 0; n < 16000; ++n)
    d16.add(static_cast<int16_t>(std::lrint(1000.0 * std::sin(2 * 3.14159265358979 * 3000.0 * n / 16000.0))));
  const HarmonicResult r16 = d16.result();
  check(r16.nHarm == 2, "harm: only 2 harmonics of 3 kHz below 8 kHz", r16.nHarm, 2);
  near(r16.fundDb, 20.0 * std::log10(1000.0 / 32768.0), 0.05, "harm: 16 kHz fundamental");
  HarmonicDetector e;
  e.begin(&t);
  check(e.result().n == 0 && e.result().fundDb <= -119.0f, "harm: empty");
  for (int n = 0; n < 4410; ++n) e.add(-1);
  check(e.result().fundDb <= -119.0f && e.result().noiseDb <= -119.0f, "harm: constant has no tone and no noise");
}

// ---------------------------------------------------------------------------
// FFT / averaged spectrum / worst spur
void testSpectrum() {
  {
    const int n = 64;
    std::vector<float> re(n), im(n, 0.0f);
    for (int i = 0; i < n; ++i) re[i] = std::cos(2.0f * 3.14159265f * 5.0f * static_cast<float>(i) / n);
    fftRadix2(re.data(), im.data(), n);
    bool ok = true;
    for (int k = 0; k < n; ++k) {
      const float m = std::sqrt(re[k] * re[k] + im[k] * im[k]);
      const float want = (k == 5 || k == n - 5) ? n / 2.0f : 0.0f;
      if (std::fabs(m - want) > 1e-3f) ok = false;
    }
    check(ok, "fft: bin-centred cosine is one line of n/2");
  }
  const int N = 2048;
  const float rate = 44100.0f;
  std::vector<float> re(N), im(N), pw(N / 2 + 1);
  std::vector<int16_t> frame(N);
  {  // 50 Hz hum at -70 dBFS on white noise (-80 dBFS RMS) with DC
    SpectrumAcc s;
    check(s.begin(pw.data(), N), "spectrum: begin");
    std::mt19937 rng(9);
    const double sigma = 32768.0 * std::pow(10.0, -80.0 / 20.0);
    std::normal_distribution<double> nd(0.0, sigma);
    const double a = 32768.0 * std::pow(10.0, -70.0 / 20.0);
    long t = 0;
    for (int f = 0; f < 20; ++f) {
      for (int i = 0; i < N; ++i, ++t)
        frame[i] = static_cast<int16_t>(
            std::lrint(-300.0 + a * std::sin(2 * 3.14159265358979 * 50.0 * t / 44100.0) + nd(rng)));
      s.addFrame(frame.data(), re.data(), im.data());
    }
    const SpurResult sp = s.worstSpur(rate, 40.0f, re.data());
    check(sp.valid && std::fabs(sp.hz - 50.0f) < 22.0f, "spectrum: hum found at 50 Hz", sp.hz, 50);
    near(sp.db, -70.0, 0.5, "spectrum: hum 0.3 bin off-centre still reads its sine level (main-lobe energy)");
    check(sp.overFloorDb > 15.0f, "spectrum: hum stands well above the median bin", sp.overFloorDb, 15);
    // white noise: mean power per bin = sigma^2 * sum(w^2) = sigma^2 * 3N/8, median of an exponential = 0.693 x mean
    // (averaged over 20 frames the distribution narrows, so the median sits between 0.69 and 1 x mean)
    const double floorMean = 20.0 * std::log10(4.0 * std::sqrt(sigma * sigma * 3.0 * N / 8.0) / N / 32768.0);
    const double fl = sp.floorDb;
    check(fl > floorMean - 1.7 && fl < floorMean + 0.3, "spectrum: median bin of white noise",
          sp.floorDb, floorMean);
    check(s.frames() == 20, "spectrum: frame count");
  }
  {  // DC only: not a spur
    SpectrumAcc s;
    s.begin(pw.data(), N);
    std::mt19937 rng(1);
    std::normal_distribution<double> nd(0.0, 3.0);
    for (int f = 0; f < 8; ++f) {
      for (int i = 0; i < N; ++i) frame[i] = static_cast<int16_t>(std::lrint(5000.0 + nd(rng)));
      s.addFrame(frame.data(), re.data(), im.data());
    }
    const SpurResult sp = s.worstSpur(rate, 40.0f, re.data());
    check(sp.valid && sp.overFloorDb < 12.0f, "spectrum: DC is not a spur", sp.overFloorDb, 12);
  }
  {  // full-scale bin-centred sine reads ~0 dBFS
    SpectrumAcc s;
    s.begin(pw.data(), N);
    for (int i = 0; i < N; ++i)
      frame[i] = static_cast<int16_t>(std::lrint(32000.0 * std::sin(2 * 3.14159265358979 * 100.0 * i / N)));
    s.addFrame(frame.data(), re.data(), im.data());
    const SpurResult sp = s.worstSpur(rate, 40.0f, re.data());
    check(sp.bin == 100, "spectrum: bin of a centred sine", sp.bin, 100);
    near(sp.db, 20.0 * std::log10(32000.0 / 32768.0), 0.05, "spectrum: centred sine level");
  }
  {
    SpectrumAcc s;
    check(!s.begin(pw.data(), 1000), "spectrum: n not a power of two refused");
    check(!s.worstSpur(rate, 40.0f, re.data()).valid, "spectrum: no frames -> no spur");
  }
}

void testQualityReport() {
  SamplerReport r;
  Collect c0;
  formatSamplerReport(r, collect, &c0);
  bool sawNa = false;
  for (auto& l : c0.lines)
    if (l.rfind("tone: valid=0", 0) == 0 && l.find("thd_db=na") != std::string::npos) sawNa = true;
  check(sawNa, "quality report: unmeasured tone prints na");
  MicQuality& q = r.quality;
  q.valid = q.toneValid = true;
  q.slot = 1;
  q.rate = 4294967295u;
  q.fftN = 2048;
  q.silSecs = q.secs = 12345.6f;
  q.toneHz = q.toneN = 4294967295u;
  q.vol = 0xAF;
  q.stimDb = -120.0f;
  for (auto& n : q.noise) {
    n.valid = n.spurValid = n.stuck = true;
    n.pgaDb = 30;
    n.rmsDb = n.peakDb = n.spurDb = n.floorDb = -120.0f;
    n.spurOverFloorDb = -120.0f;
    n.dcLsb = -32768.0f;
    n.spurHz = 22050.0f;
    n.clips = n.frames = 4294967295u;
  }
  q.tone.fundDb = q.tone.thdDb = q.tone.noiseDb = q.tone.snrDb = q.tone.sinadDb = -120.0f;
  for (float& h : q.tone.harmDb) h = -120.0f;
  q.tone.thdPct = 100.0f;
  q.tone.dcLsb = -32768.0f;
  q.tone.n = 4294967295u;
  Collect c1;
  formatSamplerReport(r, collect, &c1);
  check(c1.lines.size() == c0.lines.size(), "quality report: fixed line count");
  checkBlock(c1, "===== PG-HWSPIKE SAMPLER BEGIN", "===== PG-HWSPIKE SAMPLER END =====", "sampler report (quality)");
  int noiseLines = 0;
  bool sawTone = false;
  for (auto& l : c1.lines) {
    if (l.rfind("noise_gain", 0) == 0) ++noiseLines;
    if (l.rfind("tone_noise:", 0) == 0 && l.find("sinad_db=-120.0") != std::string::npos) sawTone = true;
  }
  check(noiseLines == kNumGainRows, "quality report: one noise line per gain", noiseLines, kNumGainRows);
  check(sawTone, "quality report: tone noise line");
  for (auto& l : c1.lines)
    if (l.rfind("quality:", 0) == 0 || l.rfind("noise_gain1", 0) == 0 || l.rfind("tone", 0) == 0)
      std::printf("  %s\n", l.c_str());
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
  testTone();
  testMatrixReport();
  testMicSlots();
  testDiffLine();
  testHarmonics();
  testSpectrum();
  testQualityReport();
  std::printf("test_hwspike: %d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
