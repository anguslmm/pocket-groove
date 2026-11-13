// Tilt maths (see tilt.h). From the hardware spike's tested code
// (src/hwspike/tilt_math.*), cut to what the instrument needs, with the costs
// docs/hardware-notes.md 11.4 found taken out: one gravity estimate per response
// in use, one normalisation per estimate (one sqrtf, one divide), coefficients
// computed when a response is chosen (never per sample), atan2 by a polynomial,
// no doubles, no diagnostics.
#include "tilt.h"

#include <cmath>

namespace pg {
namespace tilt {

namespace {

constexpr float kPi = 3.14159265f;
constexpr float kRadToDeg = 57.2957795f;

// name, fused, tau, lpHz, dead, range: the hardware spike's four presets
// (acc-fast, acc-smooth, fused, fused-wide), unchanged.
const ResponseSpec kSpecs[kNumResponses] = {
    {"fast",   false, 0.0f, 20.0f, 0.5f, 30.0f},
    {"smooth", false, 0.0f, 6.0f,  1.5f, 35.0f},
    {"fine",   true,  0.4f, 25.0f, 1.0f, 30.0f},
    {"wide",   true,  0.8f, 12.0f, 3.0f, 60.0f},
};

inline float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline Vec3 scale(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline Vec3 sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
// Unit vector; a (near) zero vector gives `fallback`.
inline Vec3 unit(Vec3 a, Vec3 fallback) {
  const float n2 = dot(a, a);
  if (!(n2 > 1e-12f)) return fallback;
  return scale(a, 1.0f / sqrtf(n2));
}
inline float clamp1(float x) { return x < -1.0f ? -1.0f : (x > 1.0f ? 1.0f : (x == x ? x : 0.0f)); }

bool ieq(const char* a, const char* b) {
  for (;; ++a, ++b) {
    char ca = *a, cb = *b;
    if (ca >= 'A' && ca <= 'Z') ca = char(ca - 'A' + 'a');
    if (cb >= 'A' && cb <= 'Z') cb = char(cb - 'A' + 'a');
    if (ca != cb) return false;
    if (!ca) return true;
  }
}

}  // namespace

const ResponseSpec& responseSpec(int r) {
  return kSpecs[r < 0 ? 0 : (r >= kNumResponses ? kNumResponses - 1 : r)];
}

int responseFromName(const char* name) {
  if (!name) return -1;
  for (int r = 0; r < kNumResponses; ++r)
    if (ieq(name, kSpecs[r].name)) return r;
  return -1;
}

float atan2Fast(float y, float x) {
  const float ax = fabsf(x), ay = fabsf(y);
  const float mx = ax > ay ? ax : ay, mn = ax > ay ? ay : ax;
  if (!(mx > 0.0f)) return 0.0f;
  const float a = mn / mx;   // 0..1
  const float s = a * a;
  // atan(a) on 0..1, minimax odd polynomial (max error ~1e-5 rad)
  float r = ((((0.0208351f * s - 0.0851330f) * s + 0.1801410f) * s - 0.3302995f) * s + 0.9998660f) * a;
  if (ay > ax) r = 0.5f * kPi - r;
  if (x < 0.0f) r = kPi - r;
  return y < 0.0f ? -r : r;
}

void Axis::setup(int r, float dt) {
  const ResponseSpec& p = responseSpec(r);
  k_ = (p.lpHz > 0.0f && dt > 0.0f) ? 1.0f - expf(-2.0f * kPi * p.lpHz * dt) : 1.0f;
  if (!(k_ > 0.0f)) k_ = 1.0f;
  if (k_ > 1.0f) k_ = 1.0f;
  dead_ = p.deadDeg;
  invSpan_ = 1.0f / (p.rangeDeg > p.deadDeg ? p.rangeDeg - p.deadDeg : 1.0f);
  reset();
}

float Axis::update(float relDeg) {
  if (!(relDeg == relDeg)) relDeg = 0.0f;   // NaN: as the press pose
  sm_ += k_ * (relDeg - sm_);
  const float d = sm_ > dead_ ? sm_ - dead_ : (sm_ < -dead_ ? sm_ + dead_ : 0.0f);
  out_ = clamp1(d * invSpan_);
  return out_;
}

void Tracker::setRate(float hz) { dt_ = hz > 0.0f ? 1.0f / hz : 0.01f; }

void Tracker::setOrientation(bool swapXY, float signX, float signY) {
  swap_ = swapXY;
  sign_[0] = signX < 0.0f ? -1.0f : 1.0f;
  sign_[1] = signY < 0.0f ? -1.0f : 1.0f;
}

void Tracker::press(int responseX, int responseY) {
  resp_[0] = uint8_t(responseX < 0 || responseX >= kNumResponses ? kDefaultResponse : responseX);
  resp_[1] = uint8_t(responseY < 0 || responseY >= kNumResponses ? kDefaultResponse : responseY);
  nFus_ = 0;
  for (int a = 0; a < 2; ++a) {
    ax_[a].setup(resp_[a], dt_);
    rel_[a] = 0.0f;
    const ResponseSpec& p = responseSpec(resp_[a]);
    src_[a] = -1;
    if (!p.fused) continue;
    if (a == 1 && src_[0] >= 0 && responseSpec(resp_[0]).tau == p.tau) {
      src_[1] = src_[0];   // both axes on the same fusion: compute it once
      continue;
    }
    src_[a] = int8_t(nFus_);
    fus_[nFus_].alpha = p.tau / (p.tau + dt_);
    ++nFus_;
  }
  n_ = 0;
  held_ = true;
  pending_ = true;
}

void Tracker::release() {
  held_ = false;
  pending_ = false;
}

void Tracker::sample(Vec3 acc, Vec3 gyro) {
  if (!held_) return;
  const Vec3 up = unit(acc, ez_);
  if (pending_) {
    // The zero pose: the accelerometer's direction now. The frame has no
    // singularity near it whatever angle the device is held at.
    pending_ = false;
    ez_ = up;
    Vec3 e = {1.0f, 0.0f, 0.0f};
    if (fabsf(dot(e, ez_)) > 0.9f) e = {0.0f, 1.0f, 0.0f};
    ex_ = unit(sub(e, scale(ez_, dot(e, ez_))), Vec3{1.0f, 0.0f, 0.0f});
    ey_ = cross(ez_, ex_);
    for (int i = 0; i < nFus_; ++i) fus_[i].g = up;
  } else {
    // Complementary filter on the gravity direction: the gyro's prediction
    // g - (w x g) dt blended toward the accelerometer, one normalisation.
    for (int i = 0; i < nFus_; ++i) {
      Fusion& f = fus_[i];
      const Vec3 pred = sub(f.g, scale(cross(gyro, f.g), dt_));
      const float al = f.alpha;
      f.g = unit(Vec3{pred.x * al + up.x * (1.0f - al), pred.y * al + up.y * (1.0f - al),
                      pred.z * al + up.z * (1.0f - al)},
                 f.g);
    }
  }
  ++n_;
  for (int a = 0; a < 2; ++a) {
    const Vec3 g = src_[a] < 0 ? up : fus_[src_[a]].g;
    // axis a reads the frame's x (roll) or y (pitch) component, or the other way round
    const Vec3& e = ((a == 0) != swap_) ? ex_ : ey_;
    rel_[a] = sign_[a] * atan2Fast(dot(g, e), dot(g, ez_)) * kRadToDeg;
    ax_[a].update(rel_[a]);
  }
}

}  // namespace tilt
}  // namespace pg
