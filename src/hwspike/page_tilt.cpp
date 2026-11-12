// TILT page: BMI270 measurements and a playable tilt-controlled filter.
//
// The maths runs on the IMU task (one call per poll, fresh samples only);
// the UI reads a snapshot under a spinlock; the audio task reads the two
// filter targets as atomics. Keys (UI) post requests as atomics.
#include <Arduino.h>

#include <esp_attr.h>
#include <esp_rom_sys.h>
#include <freertos/FreeRTOS.h>
#include <atomic>
#include <cmath>
#include <cstring>

#include "app.h"
#include "dsp.h"
#include "duplex_audio.h"
#include "imu.h"
#include "tilt_math.h"
#include "../platform/keys.h"

namespace hws {
namespace tilt {

using app::key;

namespace {

// ---- keys -------------------------------------------------------------------
constexpr uint8_t kSpace = key(3, 13);
constexpr uint8_t k1 = key(0, 1), k2 = key(0, 2), k3 = key(0, 3), k4 = key(0, 4);
constexpr uint8_t k5 = key(0, 5), k6 = key(0, 6), k7 = key(0, 7), k8 = key(0, 8);
constexpr uint8_t kP = key(1, 10), kN = key(3, 8), kI = key(1, 8), kZ = key(3, 3), kC = key(3, 5), kG = key(2, 6);
constexpr uint8_t kY = key(1, 6);

// ---- requests UI -> IMU task ------------------------------------------------
std::atomic<int>  g_reqPreset{2};
std::atomic<bool> g_reqPress{false}, g_reqRelease{false}, g_reqCalib{false}, g_reqDrift{false};
std::atomic<int>  g_gyroSign{1};
std::atomic<bool> g_swapAxes{false};
std::atomic<bool> g_resetStats{false};

// ---- IMU task -> audio ------------------------------------------------------
std::atomic<float> g_cutTarget{700.0f}, g_resTarget{0.45f};
std::atomic<bool>  g_soundOn{true};
std::atomic<int>   g_source{0};   // 0 saw chord, 1 noise, 2 sine
const char* const kSourceName[3] = {"saw chord", "noise", "sine 220"};

// ---- IMU task -> UI snapshot ------------------------------------------------
struct Live {
  float acc[3], gyr[3];
  float absA, absB, fusA, fusB;
  float relA, relB, outA, outB;
  float noiseAccA, noiseAccB, noiseFusA, noiseFusB, noiseGyro;
  float accMean[3];
  float bias[3];
  bool  biasCal;
  float calProgress;   // 0..1 while calibrating, -1 otherwise
  bool  calFailed;
  bool  driftActive;
  float driftSecs, driftGyroDeg, driftFusedDeg;
  float signCheck;
  uint32_t shakes;
  float lastShakeG;
  uint32_t fresh, dups, gaps;
  float ticksPerSample;
  float cutoff, res;
  bool  held;
  int   preset;
};
Live g_live = {};
portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;

// IMU-task-only state
struct ImuState {
  bool     init = false;
  float    dt = 1.0f / 200.0f;
  float    accScale = 1.0f, gyrScale = 1.0f;   // raw -> g, raw -> dps
  GravityFusion fusion;      // display fusion, tau 0.5 s
  GravityFusion gyroOnly;    // drift test
  TiltMapper mapper;
  ShakeDetector shake;
  WindowStd nAccA{400}, nAccB{400}, nFusA{400}, nFusB{400}, nGx{400}, nGy{400}, nGz{400};
  Vec3     accLp2{0, 0, 1}, accLp10{0, 0, 1}, prevG10{0, 0, 1};
  bool     lpInit = false;
  float    bias[3] = {0, 0, 0};
  bool     biasCal = false, calFailed = false;
  int      calN = 0, calTarget = 0;
  float    calSum[3] = {0, 0, 0}, calSq[3] = {0, 0, 0};
  bool     drift = false;
  float    driftSecs = 0, driftGyro = -1, driftFused = -1;
  double   sPO = 0, sPP = 0, sOO = 0;
  uint32_t fresh = 0, dups = 0, gaps = 0;
  uint32_t prevSt = 0;
  bool     havePrevSt = false;
  float    tps = 0;    // sensortime ticks per sample (EMA)
};
ImuState g_s;

void resetWindows(ImuState& s, uint32_t n) {
  for (WindowStd* w : {&s.nAccA, &s.nAccB, &s.nFusA, &s.nFusB, &s.nGx, &s.nGy, &s.nGz}) w->setWindow(n);
}

void onSample(const imu::Sample& smp, void*) {
  ImuState& s = g_s;
  if (!smp.freshAcc) {
    ++s.dups;
    return;
  }
  ++s.fresh;
  if (s.havePrevSt) {
    const uint32_t d = (smp.sensortime - s.prevSt) & 0xFFFFFFu;
    if (s.tps > 0.0f && static_cast<float>(d) > 1.5f * s.tps) ++s.gaps;
    else s.tps = (s.tps == 0.0f) ? static_cast<float>(d) : s.tps + (static_cast<float>(d) - s.tps) * 0.02f;
  }
  s.prevSt = smp.sensortime;
  s.havePrevSt = true;

  if (g_resetStats.exchange(false)) {
    resetWindows(s, static_cast<uint32_t>(2.0f / s.dt));
    s.gaps = 0;
    s.sPO = s.sPP = s.sOO = 0;
  }
  const Vec3 acc = {smp.acc[0] * s.accScale, smp.acc[1] * s.accScale, smp.acc[2] * s.accScale};
  const float gdps[3] = {smp.gyr[0] * s.gyrScale - s.bias[0], smp.gyr[1] * s.gyrScale - s.bias[1],
                         smp.gyr[2] * s.gyrScale - s.bias[2]};
  const Vec3 w = {gdps[0] * kDegToRad, gdps[1] * kDegToRad, gdps[2] * kDegToRad};
  const float dt = s.dt;
  const float sign = static_cast<float>(g_gyroSign.load(std::memory_order_relaxed));

  // requests
  const int pr = g_reqPreset.load(std::memory_order_relaxed);
  if (pr != s.mapper.preset()) s.mapper.setPreset(pr);
  if (g_reqPress.exchange(false)) s.mapper.press();
  if (g_reqRelease.exchange(false)) s.mapper.release();
  if (g_reqCalib.exchange(false)) {
    s.calN = 0;
    s.calTarget = static_cast<int>(2.0f / dt);
    for (int i = 0; i < 3; ++i) s.calSum[i] = s.calSq[i] = 0;
    s.calFailed = false;
  }
  // gyro bias calibration (raw dps, before bias removal)
  if (s.calTarget > 0) {
    for (int i = 0; i < 3; ++i) {
      const float g = smp.gyr[i] * s.gyrScale;
      s.calSum[i] += g;
      s.calSq[i] += g * g;
    }
    if (++s.calN >= s.calTarget) {
      bool still = true;
      float b[3];
      for (int i = 0; i < 3; ++i) {
        b[i] = s.calSum[i] / static_cast<float>(s.calN);
        const float var = s.calSq[i] / static_cast<float>(s.calN) - b[i] * b[i];
        if (var > 4.0f) still = false;   // std > 2 dps: it moved
      }
      if (still) {
        for (int i = 0; i < 3; ++i) s.bias[i] = b[i];
        s.biasCal = true;
      } else {
        s.calFailed = true;
      }
      s.calTarget = 0;
    }
  }

  // low-passed accelerometer (drift reference, sign check)
  if (!s.lpInit) {
    s.accLp2 = s.accLp10 = s.prevG10 = acc;
    s.lpInit = true;
  }
  s.accLp2 = vadd(s.accLp2, vscale(vsub(acc, s.accLp2), onePoleK(2.0f, dt)));
  s.accLp10 = vadd(s.accLp10, vscale(vsub(acc, s.accLp10), onePoleK(10.0f, dt)));

  // display fusion + absolute angles
  const Vec3 gf = s.fusion.update(acc, w, dt, 0.5f, sign);
  float absA, absB, fusA, fusB;
  absTilt(acc, &absA, &absB);
  absTilt(gf, &fusA, &fusB);
  s.nAccA.add(absA);
  s.nAccB.add(absB);
  s.nFusA.add(fusA);
  s.nFusB.add(fusB);
  s.nGx.add(gdps[0]);
  s.nGy.add(gdps[1]);
  s.nGz.add(gdps[2]);

  // gyro / accelerometer sign agreement during motion
  {
    const Vec3 gNow = vnormalize(s.accLp10);
    const Vec3 gPrev = vnormalize(s.prevG10);
    if (vnorm(w) > 0.5f) {
      const Vec3 pred = vscale(vcross(w, gPrev), -dt);
      const Vec3 obs = vsub(gNow, gPrev);
      s.sPO += static_cast<double>(vdot(pred, obs));
      s.sPP += static_cast<double>(vdot(pred, pred));
      s.sOO += static_cast<double>(vdot(obs, obs));
    }
    s.prevG10 = s.accLp10;
  }

  // drift test
  if (g_reqDrift.exchange(false)) {
    s.gyroOnly.reset(s.accLp2);
    s.drift = true;
    s.driftSecs = 0;
  }
  if (s.drift) {
    s.gyroOnly.update(acc, w, dt, 0.0f, sign, false);
    s.driftSecs += dt;
    s.driftGyro = angleBetweenDeg(s.gyroOnly.g(), s.accLp2);
    s.driftFused = angleBetweenDeg(gf, s.accLp2);
    if (s.driftSecs >= 60.0f) s.drift = false;
  }

  // playable mapping
  s.mapper.update(acc, w, dt, sign);
  const bool swap = g_swapAxes.load(std::memory_order_relaxed);
  const float ca = swap ? s.mapper.b() : s.mapper.a();
  const float cb = swap ? s.mapper.a() : s.mapper.b();
  const float cutoff = 700.0f * exp2f(3.5f * ca);              // 62 Hz .. 7.9 kHz, 700 Hz at the key-down pose
  const float res = clampf(0.45f + 0.5f * cb, 0.0f, 0.95f);
  g_cutTarget.store(cutoff, std::memory_order_relaxed);
  g_resTarget.store(res, std::memory_order_relaxed);

  const bool shook = s.shake.update(acc, dt);
  (void)shook;

  // snapshot
  Live l;
  l.acc[0] = acc.x; l.acc[1] = acc.y; l.acc[2] = acc.z;
  for (int i = 0; i < 3; ++i) l.gyr[i] = gdps[i];
  l.absA = absA; l.absB = absB; l.fusA = fusA; l.fusB = fusB;
  l.relA = s.mapper.relADeg(); l.relB = s.mapper.relBDeg();
  l.outA = ca; l.outB = cb;
  l.noiseAccA = s.nAccA.minimum(); l.noiseAccB = s.nAccB.minimum();
  l.noiseFusA = s.nFusA.minimum(); l.noiseFusB = s.nFusB.minimum();
  {
    const float gx = s.nGx.minimum(), gy = s.nGy.minimum(), gz = s.nGz.minimum();
    l.noiseGyro = (gx < 0 || gy < 0 || gz < 0) ? -1.0f : (gx + gy + gz) / 3.0f;
  }
  l.accMean[0] = s.accLp2.x; l.accMean[1] = s.accLp2.y; l.accMean[2] = s.accLp2.z;
  for (int i = 0; i < 3; ++i) l.bias[i] = s.bias[i];
  l.biasCal = s.biasCal;
  l.calProgress = s.calTarget > 0 ? static_cast<float>(s.calN) / static_cast<float>(s.calTarget) : -1.0f;
  l.calFailed = s.calFailed;
  l.driftActive = s.drift;
  l.driftSecs = s.driftSecs;
  l.driftGyroDeg = s.driftGyro;
  l.driftFusedDeg = s.driftFused;
  l.signCheck = (s.sPP > 0 && s.sOO > 0) ? static_cast<float>(s.sPO / sqrt(s.sPP * s.sOO)) : 0.0f;
  l.shakes = s.shake.count();
  l.lastShakeG = s.shake.lastPeakG();
  l.fresh = s.fresh;
  l.dups = s.dups;
  l.gaps = s.gaps;
  l.ticksPerSample = s.tps;
  l.cutoff = cutoff;
  l.res = res;
  l.held = s.mapper.held();
  l.preset = s.mapper.preset();
  portENTER_CRITICAL(&g_mux);
  g_live = l;
  portEXIT_CRITICAL(&g_mux);
}

Live snapshot() {
  Live l;
  portENTER_CRITICAL(&g_mux);
  l = g_live;
  portEXIT_CRITICAL(&g_mux);
  return l;
}

// ---- UI-side state -----------------------------------------------------------
struct KeyBin {
  uint32_t n = 0;
  double sum = 0;
  uint32_t max = 0;
};
KeyBin g_keysOff, g_keysOn;
uint32_t g_downs = 0, g_ups = 0;
bool g_imuWanted = true;
bool g_poll2x = false;
float g_pollHz = 0, g_newHz = 0, g_dupPct = 0;
uint32_t g_rateT0 = 0, g_rateFresh0 = 0, g_rateDup0 = 0, g_ratePolls0 = 0;
float g_read1Us = 0;
bool g_imuOk = false;

void applyRate(uint32_t hz) {
  imu::configure(hz, g_poll2x);
  g_s.dt = 1.0f / static_cast<float>(imu::rateHz());
  g_resetStats.store(true);
  imu::resetStats();
  g_rateT0 = 0;
}

// ---- audio --------------------------------------------------------------------
struct Synth {
  Svf svf;
  float logCut = 9.45f, res = 0.45f;   // log2(700)
  float ph[3] = {0, 0, 0};
  float gain = 0;                      // fade
  uint32_t rng = 1;
};
Synth g_syn;

}  // namespace

void begin() {
  g_imuOk = imu::begin();
  if (!g_imuOk) {
    app::sp("[tilt] BMI270 not found at 0x69 / 0x68\n");
    return;
  }
  g_s.accScale = 1.0f / imu::accLsbPerG();
  g_s.gyrScale = 1.0f / imu::gyrLsbPerDps();
  imu::setListener(onSample, nullptr);
  applyRate(200);
  g_read1Us = imu::readOneByteUs();
  const imu::Regs r = imu::regs();
  app::sp("[tilt] BMI270 @0x%02X chip %02X err %02X acc_conf %02X acc_range %02X gyr_conf %02X gyr_range %02X "
          "pwr %02X/%02X\n",
          r.addr, r.chipId, r.err, r.accConf, r.accRange, r.gyrConf, r.gyrRange, r.pwrConf, r.pwrCtrl);
  imu::setRunning(true);
  g_reqCalib.store(true);   // calibrate the gyro bias in the first 2 s (rejected if it moves)
}

void enter() {
  if (g_imuOk && g_imuWanted) imu::setRunning(true);
}

void leave() {
  // Keep the IMU running (its timing is part of the key-impact data); just
  // let go of a held key so the filter returns to centre.
  g_reqRelease.store(true);
}

void render(int16_t* out, int frames) {
  Synth& y = g_syn;
  const float fs = static_cast<float>(app::g_sampleRate);
  const float target = g_soundOn.load(std::memory_order_relaxed) ? 0.2f : 0.0f;   // -14 dBFS peak-ish
  const int src = g_source.load(std::memory_order_relaxed);
  const float tc = log2f(g_cutTarget.load(std::memory_order_relaxed));
  const float tr = g_resTarget.load(std::memory_order_relaxed);
  const float inc[3] = {110.0f / fs, 164.81f / fs, 220.0f / fs};
  for (int q = 0; q < frames; q += 16) {
    y.logCut += 0.35f * (tc - y.logCut);
    y.res += 0.35f * (tr - y.res);
    y.svf.set(exp2f(y.logCut), y.res, fs);
    for (int i = q; i < q + 16 && i < frames; ++i) {
      float x;
      if (src == 1) {
        y.rng ^= y.rng << 13; y.rng ^= y.rng >> 17; y.rng ^= y.rng << 5;
        x = static_cast<float>(static_cast<int32_t>(y.rng)) * (1.0f / 2147483648.0f);
      } else if (src == 2) {
        x = sinf(6.2831853f * y.ph[2]);
        y.ph[2] += inc[2];
        if (y.ph[2] >= 1.0f) y.ph[2] -= 1.0f;
      } else {
        x = 0;
        for (int k = 0; k < 3; ++k) {
          x += 2.0f * y.ph[k] - 1.0f;
          y.ph[k] += inc[k];
          if (y.ph[k] >= 1.0f) y.ph[k] -= 1.0f;
        }
        x *= (1.0f / 3.0f);
      }
      y.gain += (target - y.gain) * 0.002f;
      const float o = y.svf.lp(x) * y.gain;
      const int16_t s = sat16(static_cast<int32_t>(o * 32767.0f));
      out[2 * i] = s;
      out[2 * i + 1] = s;
    }
  }
  if (!(y.svf.ic1 == y.svf.ic1) || !(y.svf.ic2 == y.svf.ic2)) y.svf.reset();   // NaN guard
}

void key(uint8_t k, bool down) {
  if (k == kSpace) {
    if (down) g_reqPress.store(true); else g_reqRelease.store(true);
    return;
  }
  if (!down) return;
  if (k >= k1 && k <= k4) {
    g_reqPreset.store(k - k1);
    app::msg("preset %d: %s", k - k1 + 1, tiltPreset(k - k1).name);
  } else if (k == k5 || k == k6 || k == k7) {
    applyRate(k == k5 ? 100 : (k == k6 ? 200 : 400));
    app::msg("IMU %lu Hz, poll %lu Hz", static_cast<unsigned long>(imu::rateHz()),
             static_cast<unsigned long>(imu::pollHz()));
  } else if (k == k8) {
    g_poll2x = !g_poll2x;
    applyRate(imu::rateHz());
    app::msg("poll %s ODR", g_poll2x ? "2x" : "1x");
  } else if (k == kP) {
    g_soundOn.store(!g_soundOn.load());
    app::msg("sound %s", g_soundOn.load() ? "on" : "off");
  } else if (k == kN) {
    g_source.store((g_source.load() + 1) % 3);
    app::msg("source: %s", kSourceName[g_source.load()]);
  } else if (k == kI) {
    g_imuWanted = !g_imuWanted;
    imu::setRunning(g_imuWanted);
    app::msg("IMU polling %s (key test)", g_imuWanted ? "ON" : "OFF");
  } else if (k == kZ) {
    g_reqDrift.store(true);
    app::msg("drift test: keep still 60 s");
  } else if (k == kC) {
    g_reqCalib.store(true);
    app::msg("gyro calibration: keep still 2 s");
  } else if (k == kG) {
    g_gyroSign.store(-g_gyroSign.load());
    app::msg("gyro sign %+d (diagnostic)", g_gyroSign.load());
  } else if (k == kY) {
    g_swapAxes.store(!g_swapAxes.load());
    app::msg("axes %s: A->%s", g_swapAxes.load() ? "swapped" : "normal", g_swapAxes.load() ? "res" : "cutoff");
  }
}

void tick() {
  const uint32_t now = millis();
  if (g_rateT0 == 0) {
    g_rateT0 = now;
    const Live l = snapshot();
    g_rateFresh0 = l.fresh;
    g_rateDup0 = l.dups;
    g_ratePolls0 = imu::stats().polls;
    return;
  }
  if (now - g_rateT0 >= 1000) {
    const Live l = snapshot();
    const uint32_t polls = imu::stats().polls;
    const float secs = static_cast<float>(now - g_rateT0) * 0.001f;
    const uint32_t df = l.fresh - g_rateFresh0, dd = l.dups - g_rateDup0, dp = polls - g_ratePolls0;
    if (imu::isRunning()) {
      g_newHz = static_cast<float>(df) / secs;
      g_pollHz = static_cast<float>(dp) / secs;
      g_dupPct = (df + dd) ? 100.0f * static_cast<float>(dd) / static_cast<float>(df + dd) : 0.0f;
    } else {
      g_newHz = g_pollHz = 0;
    }
    g_rateT0 = now;
    g_rateFresh0 = l.fresh;
    g_rateDup0 = l.dups;
    g_ratePolls0 = polls;
  }
}

void noteKeyLatency(uint32_t us) {
  KeyBin& b = imu::isRunning() ? g_keysOn : g_keysOff;
  ++b.n;
  b.sum += us;
  if (us > b.max) b.max = us;
}

void noteKeyEvent(bool down) {
  if (down) ++g_downs; else ++g_ups;
}

namespace {

void bar(int y, float v, uint8_t color) {   // v in [-1, 1], centred bar at the right
  const int x0 = 170, w = 66, mid = x0 + w / 2;
  app::g_cv.rect(x0, y, w, 6, smoke::kDark);
  int len = static_cast<int>(v * static_cast<float>(w / 2));
  if (len > 0) app::g_cv.fillRect(mid, y + 1, len, 4, color);
  else if (len < 0) app::g_cv.fillRect(mid + len, y + 1, -len, 4, color);
  app::g_cv.fillRect(mid, y, 1, 6, smoke::kWhite);
}

float latencyBaseMs() {
  const float fs = static_cast<float>(app::g_sampleRate);
  const float age = imu::isRunning() ? 500.0f / static_cast<float>(imu::pollHz()) : 0.0f;
  return age + 1000.0f * duplex::kBlockFrames / fs + 1000.0f * duplex::kBlockFrames * duplex::config().dmaDesc / fs;
}

}  // namespace

void draw() {
  using namespace smoke;
  Canvas& cv = app::g_cv;
  const Live l = snapshot();
  const imu::Stats is = imu::stats();
  cv.clear(kBlack);
  cv.title("TILT  [tab] sampler", kReportVersion);
  if (!g_imuOk) {
    cv.line(2, kRed, "BMI270 not found (0x69/0x68)");
    cv.line(15, kGrey, "%s", app::currentMsg());
    return;
  }
  const TiltPreset& p = tiltPreset(l.preset);
  cv.line(1, imu::isRunning() ? kGreen : kRed, "imu %luHz poll %.0f new %.1f dup %.0f%% i2c %.0fus",
          static_cast<unsigned long>(imu::rateHz()), static_cast<double>(g_pollHz), static_cast<double>(g_newHz),
          static_cast<double>(g_dupPct), static_cast<double>(is.i2cAvgUs));
  cv.line(2, kWhite, "acc %+.3f %+.3f %+.3f g", static_cast<double>(l.acc[0]), static_cast<double>(l.acc[1]),
          static_cast<double>(l.acc[2]));
  cv.line(3, kWhite, "gyr %+7.2f %+7.2f %+7.2f dps", static_cast<double>(l.gyr[0]), static_cast<double>(l.gyr[1]),
          static_cast<double>(l.gyr[2]));
  cv.line(4, kCyan, "acc A%+6.1f B%+6.1f  fus A%+6.1f B%+6.1f", static_cast<double>(l.absA),
          static_cast<double>(l.absB), static_cast<double>(l.fusA), static_cast<double>(l.fusB));
  cv.line(5, kWhite, "noise acc %.3f/%.3f fus %.3f/%.3f deg", static_cast<double>(l.noiseAccA),
          static_cast<double>(l.noiseAccB), static_cast<double>(l.noiseFusA), static_cast<double>(l.noiseFusB));
  if (l.calProgress >= 0.0f) {
    cv.line(6, kYellow, "[c] calibrating gyro %.0f%% - keep still", static_cast<double>(l.calProgress * 100.0f));
  } else {
    cv.line(6, l.driftActive ? kYellow : kWhite, "[z]drift %.0fs gyro %.2f fus %.2f%s", static_cast<double>(l.driftSecs),
            static_cast<double>(l.driftGyroDeg), static_cast<double>(l.driftFusedDeg),
            l.calFailed ? " CAL?" : (l.biasCal ? "" : " nocal"));
  }
  cv.line(7, l.held ? kGreen : kGrey, "%s A%+6.1f B%+6.1f", l.held ? "HELD" : "spc ", static_cast<double>(l.relA),
          static_cast<double>(l.relB));
  bar(7 * kCellH + 1, l.outA, kGreen);
  cv.line(8, kWhite, "cut %5.0fHz res %.2f %s", static_cast<double>(l.cutoff), static_cast<double>(l.res),
          g_soundOn.load() ? kSourceName[g_source.load()] : "(off)");
  bar(8 * kCellH + 1, l.outB, kOrange);
  cv.line(9, kCyan, "[1-4] %d %s lp%.0f dz%.1f r%.0f", l.preset + 1, p.name, static_cast<double>(p.lpHz),
          static_cast<double>(p.deadDeg), static_cast<double>(p.rangeDeg));
  const float sm = p.lpHz > 0 ? 1000.0f / (2.0f * kPi * p.lpHz) : 0.0f;
  cv.line(10, kWhite, "latency est %.1f ms (smooth %.1f)", static_cast<double>(latencyBaseMs() + sm),
          static_cast<double>(sm));
  cv.line(11, kWhite, "shake %lu (last %.1f g) gaps %lu", static_cast<unsigned long>(l.shakes),
          static_cast<double>(l.lastShakeG), static_cast<unsigned long>(l.gaps));
  cv.line(12, kWhite, "keys imu off n%lu avg %.0f max %luus", static_cast<unsigned long>(g_keysOff.n),
          g_keysOff.n ? g_keysOff.sum / g_keysOff.n : 0.0, static_cast<unsigned long>(g_keysOff.max));
  cv.line(13, kWhite, "keys imu on  n%lu avg %.0f max %luus", static_cast<unsigned long>(g_keysOn.n),
          g_keysOn.n ? g_keysOn.sum / g_keysOn.n : 0.0, static_cast<unsigned long>(g_keysOn.max));
  cv.line(14, kYellow, "%s", app::currentMsg());
  cv.line(15, kGrey, "p snd n src i imu 5-7 Hz 8 2x z c g y");
}

void report() {
  TiltReport r;
  const Live l = snapshot();
  const imu::Regs g = imu::regs();
  const imu::Stats is = imu::stats();
  r.imuOk = g_imuOk;
  r.addr = g.addr; r.chipId = g.chipId; r.errReg = g.err; r.accConf = g.accConf; r.accRange = g.accRange;
  r.gyrConf = g.gyrConf; r.gyrRange = g.gyrRange; r.pwrConf = g.pwrConf; r.pwrCtrl = g.pwrCtrl;
  r.accLsbPerG = imu::accLsbPerG();
  r.gyrLsbPerDps = imu::gyrLsbPerDps();
  r.setHz = imu::rateHz();
  r.pollHz = g_pollHz;
  r.newHz = g_newHz;
  r.dupPct = g_dupPct;
  r.gaps = l.gaps;
  r.ticksPerSample = l.ticksPerSample;
  r.odrSensortimeHz = l.ticksPerSample > 0 ? 1.0e6f / (l.ticksPerSample * 39.0625f) : 0.0f;
  r.i2cAvgUs = is.i2cAvgUs;
  r.i2cMaxUs = is.i2cMaxUs;
  r.i2c1ByteUs = g_read1Us;
  r.i2cErrors = is.i2cErrors;
  r.taskUsPerPoll = is.taskAvgUs;
  r.mathCyclesPerSample = is.listenerAvgCyc;
  r.imuCorePct = is.taskAvgUs * g_pollHz * 1e-4f;
  const duplex::Stats as = duplex::stats();
  r.audioAvgUs = as.avgUs;
  r.audioMaxUs = as.maxUs;
  r.audioCpuPct = as.cpuPercent;
  r.noiseAccA = l.noiseAccA; r.noiseAccB = l.noiseAccB; r.noiseFusedA = l.noiseFusA; r.noiseFusedB = l.noiseFusB;
  r.gyroNoiseDps = l.noiseGyro;
  for (int i = 0; i < 3; ++i) { r.biasDps[i] = l.bias[i]; r.accMeanG[i] = l.accMean[i]; }
  r.biasCalibrated = l.biasCal;
  r.accMeanNormG = sqrtf(l.accMean[0] * l.accMean[0] + l.accMean[1] * l.accMean[1] + l.accMean[2] * l.accMean[2]);
  r.driftSecs = l.driftSecs;
  r.driftGyroDeg = l.driftGyroDeg;
  r.driftFusedDeg = l.driftFusedDeg;
  r.signCheck = l.signCheck;
  r.gyroSign = static_cast<float>(g_gyroSign.load());
  const float fs = static_cast<float>(app::g_sampleRate);
  r.latReadAgeMs = 500.0f / static_cast<float>(imu::pollHz());
  r.latBlockMs = 1000.0f * duplex::kBlockFrames / fs;
  r.latDmaMs = 1000.0f * duplex::kBlockFrames * duplex::config().dmaDesc / fs;
  for (int i = 0; i < 4; ++i) {
    const TiltPreset& p = tiltPreset(i);
    r.latSmoothMs[i] = p.lpHz > 0 ? 1000.0f / (2.0f * kPi * p.lpHz) : 0.0f;
  }
  r.keysImuOff.n = g_keysOff.n;
  r.keysImuOff.avgUs = g_keysOff.n ? static_cast<float>(g_keysOff.sum / g_keysOff.n) : 0.0f;
  r.keysImuOff.maxUs = static_cast<float>(g_keysOff.max);
  r.keysImuOn.n = g_keysOn.n;
  r.keysImuOn.avgUs = g_keysOn.n ? static_cast<float>(g_keysOn.sum / g_keysOn.n) : 0.0f;
  r.keysImuOn.maxUs = static_cast<float>(g_keysOn.max);
  const plat::keys::Stats ks = plat::keys::stats();
  r.keyDrops = ks.queueDrops;
  r.keyFifoOvf = ks.fifoOverflows;
  r.keyI2cErr = ks.i2cErrors;
  r.keyUnknown = ks.unknownCodes;
  r.keyMaxIntToReadUs = ks.maxIntToReadUs;
  r.keyDowns = g_downs;
  r.keyUps = g_ups;
  r.preset = l.preset;
  r.shakes = l.shakes;
  r.lastShakeG = l.lastShakeG;
  app::fillMemory(&r.heapFree, &r.heapMin, nullptr, nullptr);
  r.uptimeS = millis() / 1000;
  formatTiltReport(r, app::reportSink, nullptr);
}

}  // namespace tilt
}  // namespace hws
