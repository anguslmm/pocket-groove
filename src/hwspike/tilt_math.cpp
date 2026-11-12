// Tilt maths (see tilt_math.h). Portable, host-tested.
#include "tilt_math.h"

namespace hws {

float angleBetweenDeg(Vec3 a, Vec3 b) {
  const Vec3 ua = vnormalize(a), ub = vnormalize(b);
  // atan2 of |cross| and dot is accurate for small angles (acos is not).
  return atan2f(vnorm(vcross(ua, ub)), vdot(ua, ub)) * kRadToDeg;
}

void absTilt(Vec3 g, float* aDeg, float* bDeg) {
  if (aDeg) *aDeg = atan2f(g.x, g.z) * kRadToDeg;
  if (bDeg) *bDeg = atan2f(g.y, sqrtf(g.x * g.x + g.z * g.z)) * kRadToDeg;
}

float onePoleK(float fcHz, float dt) {
  if (!(fcHz > 0.0f) || !(dt > 0.0f)) return 1.0f;
  const float k = 1.0f - expf(-2.0f * kPi * fcHz * dt);
  return clampf(k, 0.0f, 1.0f);
}

float deadZone(float x, float dz) {
  if (x > dz) return x - dz;
  if (x < -dz) return x + dz;
  return 0.0f;
}

float clampf(float x, float lo, float hi) {
  if (!(x >= lo)) return lo;   // also catches NaN
  if (x > hi) return hi;
  return x;
}

Vec3 GravityFusion::update(Vec3 acc, Vec3 gyroRad, float dt, float tau, float gyroSign, bool useAccel) {
  if (!valid_) {
    reset(acc);
    return g_;
  }
  const Vec3 w = vscale(gyroRad, gyroSign);
  const Vec3 pred = vnormalize(vsub(g_, vscale(vcross(w, g_), dt)));
  if (!useAccel) {
    g_ = pred;
    return g_;
  }
  const float an = vnorm(acc);
  if (!(tau > 0.0f) || !(an > 1e-6f)) {
    g_ = (an > 1e-6f) ? vscale(acc, 1.0f / an) : pred;
    return g_;
  }
  const float alpha = tau / (tau + dt);
  g_ = vnormalize(vadd(vscale(pred, alpha), vscale(acc, (1.0f - alpha) / an)));
  return g_;
}

void RelFrame::set(Vec3 g0) {
  ez_ = vnormalize(g0);
  Vec3 e = {1.0f, 0.0f, 0.0f};
  if (fabsf(vdot(e, ez_)) > 0.9f) e = {0.0f, 1.0f, 0.0f};
  ex_ = vnormalize(vsub(e, vscale(ez_, vdot(e, ez_))));
  ey_ = vcross(ez_, ex_);
}

void RelFrame::angles(Vec3 g, float* aDeg, float* bDeg) const {
  const float cz = vdot(g, ez_);
  if (aDeg) *aDeg = atan2f(vdot(g, ex_), cz) * kRadToDeg;
  if (bDeg) *bDeg = atan2f(vdot(g, ey_), cz) * kRadToDeg;
}

namespace {
const TiltPreset kPresets[kNumTiltPresets] = {
    // name           fused  tau    lpHz   dead  range
    {"acc-fast",      false, 0.0f,  20.0f, 0.5f, 30.0f},
    {"acc-smooth",    false, 0.0f,  6.0f,  1.5f, 35.0f},
    {"fused",         true,  0.4f,  25.0f, 1.0f, 30.0f},
    {"fused-wide",    true,  0.8f,  12.0f, 3.0f, 60.0f},
};
}  // namespace

const TiltPreset& tiltPreset(int i) {
  if (i < 0) i = 0;
  if (i >= kNumTiltPresets) i = kNumTiltPresets - 1;
  return kPresets[i];
}

void TiltMapper::setPreset(int i) {
  preset_ = (i < 0) ? 0 : (i >= kNumTiltPresets ? kNumTiltPresets - 1 : i);
}

void TiltMapper::press() {
  held_ = true;
  pendingPress_ = true;   // the frame is taken from the next sample's gravity
}

float TiltMapper::smoothingDelayMs() const {
  const TiltPreset& p = tiltPreset(preset_);
  return p.lpHz > 0.0f ? 1000.0f / (2.0f * kPi * p.lpHz) : 0.0f;
}

void TiltMapper::update(Vec3 acc, Vec3 gyroRad, float dt, float gyroSign) {
  const TiltPreset& p = tiltPreset(preset_);
  const Vec3 g = fusion_.update(acc, gyroRad, dt, p.fused ? p.tau : 0.0f, gyroSign);
  if (pendingPress_) {
    pendingPress_ = false;
    frame_.set(g);
  }
  if (held_) {
    frame_.angles(g, &relA_, &relB_);
  } else {
    relA_ = 0.0f;
    relB_ = 0.0f;
  }
  const float k = onePoleK(p.lpHz, dt);
  smA_ += k * (relA_ - smA_);
  smB_ += k * (relB_ - smB_);
  const float span = p.rangeDeg > p.deadDeg ? p.rangeDeg - p.deadDeg : 1.0f;
  outA_ = clampf(deadZone(smA_, p.deadDeg) / span, -1.0f, 1.0f);
  outB_ = clampf(deadZone(smB_, p.deadDeg) / span, -1.0f, 1.0f);
}

void ShakeDetector::reset() {
  init_ = false;
  armed_ = true;
  chain_ = 0;
  peakMag_ = 0;
  sinceChain_ = 1e9f;
  refractory_ = 0;
  count_ = 0;
}

bool ShakeDetector::update(Vec3 a, float dt) {
  if (!init_) {
    grav_ = a;
    init_ = true;
    return false;
  }
  const float k = onePoleK(1.0f, dt);
  grav_ = vadd(grav_, vscale(vsub(a, grav_), k));
  const Vec3 d = vsub(a, grav_);
  const float m = vnorm(d);
  sinceChain_ += dt;
  if (refractory_ > 0.0f) {
    refractory_ -= dt;
    return false;
  }
  if (sinceChain_ > maxGapS) chain_ = 0;
  if (armed_ && m > thrG) {
    // A new peak: does it point against the previous one?
    if (chain_ > 0 && vdot(d, peakDir_) < 0.0f) {
      ++chain_;
    } else {
      chain_ = 1;
    }
    peakDir_ = d;
    peakMag_ = m;
    lastPeakMag_ = m;
    sinceChain_ = 0.0f;
    armed_ = false;
    if (chain_ >= kNeed) {
      ++count_;
      chain_ = 0;
      refractory_ = refractoryS;
      armed_ = true;
      return true;
    }
  } else if (!armed_ && m < 0.5f * thrG) {
    armed_ = true;
  }
  return false;
}

void WindowStd::add(float x) {
  ++k_;
  const float d = x - mean_;
  mean_ += d / static_cast<float>(k_);
  m2_ += d * (x - mean_);
  if (k_ >= n_) {
    const float var = m2_ / static_cast<float>(k_);
    last_ = var > 0.0f ? sqrtf(var) : 0.0f;
    if (min_ < 0.0f || last_ < min_) min_ = last_;
    ++windows_;
    k_ = 0;
    mean_ = 0;
    m2_ = 0;
  }
}

}  // namespace hws
