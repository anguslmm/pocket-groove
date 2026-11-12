// Pocket Groove hardware spike (env:hw-spike) -- tilt maths. Portable: no
// Arduino / ESP headers; unit-tested on the host (host/hwspike).
//
// Conventions. All vectors are in the IMU chip's own axes (the BMI270's x, y,
// z as the registers deliver them; how they sit in the Cardputer-Adv is not
// known yet, the spike's report shows the raw vector at rest so the owner can
// tell). "g" is the accelerometer vector, which at rest points opposite to
// gravity; only its direction is used. The gyroscope rate w is in rad/s about
// the same axes, right-handed. A vector fixed in the world then changes in the
// body frame as dv/dt = -w x v, which is what the fusion integrates.
//
// Two angles are derived, called A and B:
//   A = rotation that moves g along the body x axis (tilting "left/right" if x
//       points to the right of the keyboard),
//   B = rotation that moves g along the body y axis.
// The playable mapping uses them RELATIVE to the pose at key-down (RelFrame),
// which has no singularity near the reference pose whatever angle the device
// is held at.
#pragma once
#include <cmath>
#include <cstdint>

namespace hws {

struct Vec3 {
  float x, y, z;
};

constexpr float kPi = 3.14159265f;
constexpr float kRadToDeg = 57.2957795f;
constexpr float kDegToRad = 0.0174532925f;

inline Vec3 vadd(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 vsub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 vscale(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float vdot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 vcross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float vnorm(Vec3 a) { return sqrtf(vdot(a, a)); }
// Unit vector; a zero vector becomes +z (never NaN).
inline Vec3 vnormalize(Vec3 a) {
  const float n = vnorm(a);
  if (!(n > 1e-9f)) return {0.0f, 0.0f, 1.0f};
  return vscale(a, 1.0f / n);
}
// Angle between two vectors, degrees, 0..180.
float angleBetweenDeg(Vec3 a, Vec3 b);

// Absolute tilt of a gravity vector: A = atan2(x, z), B = atan2(y, sqrt(x^2 + z^2)), degrees.
void absTilt(Vec3 g, float* aDeg, float* bDeg);

// One-pole low-pass coefficient for cutoff fc at sample interval dt.
float onePoleK(float fcHz, float dt);
// Soft dead zone: 0 inside +-dz, shifted linear outside (continuous).
float deadZone(float x, float dz);
float clampf(float x, float lo, float hi);

// Complementary filter on the gravity DIRECTION: gyro prediction blended with
// the accelerometer with time constant tau (tau <= 0: accelerometer only;
// tau = infinity, i.e. useAccel = false: gyro integration only, for the drift
// test). gyroSign = -1 flips the gyro (diagnostic for a sign convention error).
class GravityFusion {
 public:
  void reset(Vec3 acc) { g_ = vnormalize(acc); valid_ = true; }
  bool valid() const { return valid_; }
  Vec3 update(Vec3 acc, Vec3 gyroRad, float dt, float tau, float gyroSign = 1.0f, bool useAccel = true);
  Vec3 g() const { return g_; }

 private:
  Vec3 g_{0.0f, 0.0f, 1.0f};
  bool valid_ = false;
};

// Reference frame built from the gravity direction at key-down: ez = g0,
// ex = body x (or body y if x is nearly parallel to g0) made perpendicular,
// ey = ez x ex. angles() then gives A = atan2(g.ex, g.ez), B = atan2(g.ey, g.ez).
class RelFrame {
 public:
  void set(Vec3 g0);
  void angles(Vec3 g, float* aDeg, float* bDeg) const;
  Vec3 ex() const { return ex_; }
  Vec3 ey() const { return ey_; }
  Vec3 ez() const { return ez_; }

 private:
  Vec3 ex_{1, 0, 0}, ey_{0, 1, 0}, ez_{0, 0, 1};
};

struct TiltPreset {
  const char* name;
  bool  fused;      // gyro-complementary gravity instead of the raw accelerometer
  float tau;        // fusion time constant, s (fused only)
  float lpHz;       // one-pole smoothing of the relative angles
  float deadDeg;    // dead zone around the key-down pose
  float rangeDeg;   // angle that reaches the end of the control range
};
constexpr int kNumTiltPresets = 4;
const TiltPreset& tiltPreset(int i);   // i = 0..3

// The playable mapping: hold -> relative angles -> smoothing -> dead zone ->
// range -> normalised controls a(), b() in [-1, 1]. Released, the targets are
// 0 and the smoothing glides back. update() is called once per IMU sample.
class TiltMapper {
 public:
  void setPreset(int i);
  int preset() const { return preset_; }
  void press();
  void release() { held_ = false; }
  bool held() const { return held_; }
  void update(Vec3 acc, Vec3 gyroRad, float dt, float gyroSign = 1.0f);
  float a() const { return outA_; }
  float b() const { return outB_; }
  float relADeg() const { return relA_; }    // unsmoothed relative angles (0 when not held)
  float relBDeg() const { return relB_; }
  float smADeg() const { return smA_; }      // smoothed relative angles
  float smBDeg() const { return smB_; }
  // Group delay of the smoothing, ms (one-pole: 1 / (2 pi fc)).
  float smoothingDelayMs() const;

 private:
  int preset_ = 0;
  bool held_ = false, pendingPress_ = false;
  GravityFusion fusion_;
  RelFrame frame_;
  float relA_ = 0, relB_ = 0, smA_ = 0, smB_ = 0, outA_ = 0, outB_ = 0;
};

// Shake: the dynamic part of the acceleration (minus a 1 Hz gravity estimate)
// exceeds thrG, reverses direction against the previous peak within maxGapS,
// kNeed times in a row. Then a refractory period. Inputs in g.
class ShakeDetector {
 public:
  void reset();
  // Returns true on the sample a shake is recognised.
  bool update(Vec3 accG, float dt);
  uint32_t count() const { return count_; }
  float lastPeakG() const { return lastPeakMag_; }

  float thrG = 1.0f, maxGapS = 0.35f, refractoryS = 0.6f;
  int kNeed = 4;

 private:
  Vec3 grav_{0, 0, 1};
  bool init_ = false, armed_ = true;
  Vec3 peakDir_{0, 0, 0};
  float peakMag_ = 0, lastPeakMag_ = 0, sinceChain_ = 1e9f, refractory_ = 0;
  int chain_ = 0;
  uint32_t count_ = 0;
};

// Standard deviation over consecutive windows of n samples, with the minimum
// over all windows (the quietest window = noise at rest if the device was put
// down at least once).
class WindowStd {
 public:
  explicit WindowStd(uint32_t n = 400) : n_(n) {}
  void setWindow(uint32_t n) { n_ = n ? n : 1; clear(); }
  void clear() { k_ = 0; mean_ = 0; m2_ = 0; last_ = -1; min_ = -1; windows_ = 0; }
  void add(float x);
  float last() const { return last_; }        // -1 until the first window completes
  float minimum() const { return min_; }
  uint32_t windows() const { return windows_; }

 private:
  uint32_t n_, k_ = 0, windows_ = 0;
  float mean_ = 0, m2_ = 0, last_ = -1, min_ = -1;
};

}  // namespace hws
