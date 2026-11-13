// Tilt as a control (docs/UX.md 10.3 "Tilt"): the portable maths between the
// IMU (or the desktop's mouse) and the UI. Heap-free, float only, no libm on the
// per-sample path (one expf per axis when a response is chosen). Host-tested
// (host/tests/test_tilt_*.cpp).
//
// The gesture: the player holds G0 and tilts the device. The pose at the moment
// of the press is the zero, so it works at any holding angle; two axes come out,
// X (roll, left/right) and Y (pitch, away/toward), each in [-1, 1]. Each axis has
// its own RESPONSE, chosen by the control it moves (the instrument's `tilt` lines,
// docs/MACROLAB.md): the four presets the hardware spike's owner found useful
// (docs/hardware-notes.md 11.4):
//   fast    accelerometer only, light smoothing, small dead zone: twitchy
//   smooth  accelerometer only, 6 Hz smoothing: the general-purpose one (default)
//   fine    gyro-complementary gravity (tau 0.4 s): steady, for subtle moves
//   wide    gyro-complementary (tau 0.8 s), a big range: slow, broad moves
//
// Conventions: vectors in the IMU chip's own axes. The accelerometer reads the
// reaction to gravity (it points up at rest); only its direction is used. The
// gyroscope rate is in rad/s about the same axes, right-handed; a vector fixed in
// the world changes in the body frame as dv/dt = -w x v. Which chip axis is the
// device's roll axis, and the signs, are the caller's (Tracker::setOrientation).
#pragma once
#include <cstdint>

namespace pg {
namespace tilt {

enum Response : uint8_t { kFast = 0, kSmooth = 1, kFine = 2, kWide = 3 };
constexpr int kNumResponses = 4;
constexpr int kDefaultResponse = kSmooth;

struct ResponseSpec {
  const char* name;   // as the .pgm writes it
  bool  fused;        // gyro-complementary gravity instead of the accelerometer alone
  float tau;          // fusion time constant, s (fused only)
  float lpHz;         // one-pole smoothing of the relative angle
  float deadDeg;      // dead zone around the pose at the press
  float rangeDeg;     // angle (from the press pose) at which the axis reaches +-1
};
const ResponseSpec& responseSpec(int r);    // clamped to 0..3
int responseFromName(const char* name);    // case-insensitive; -1 if none

struct Vec3 {
  float x, y, z;
};

// atan2 without libm: max error about 1e-5 rad (0.0006 degrees), any quadrant;
// (0, 0) gives 0.
float atan2Fast(float y, float x);

// One control axis: relative angle (degrees from the press pose) -> one-pole
// smoothing -> dead zone (soft: continuous at its edge) -> range -> [-1, 1].
class Axis {
 public:
  // Response r at a fixed update interval dt (s). Computes the smoothing
  // coefficient (the only expf) and resets the axis to 0.
  void setup(int r, float dt);
  void reset() { sm_ = out_ = 0.0f; }
  float update(float relDeg);
  float out() const { return out_; }
  float smoothedDeg() const { return sm_; }

 private:
  float k_ = 1.0f, dead_ = 0.0f, invSpan_ = 1.0f, sm_ = 0.0f, out_ = 0.0f;
};

// The IMU side of the gesture: per sample, gravity (from the accelerometer, or
// fused with the gyroscope where an axis's response asks for it) relative to the
// pose at the press, as two angles, through each axis's response. While not held
// it does nothing: the reader can stop polling, the fused responses start from
// the accelerometer pose of the first sample after the press (no state from
// earlier holds, no gyro calibration needed: a gyro bias b shows as at most
// b * tau, 0.4 degrees at 1 dps and tau 0.4 s, inside every dead zone).
class Tracker {
 public:
  // Fixed sample rate of the reader (Hz). Call before press().
  void setRate(float hz);
  // Which chip axis carries X (the device's roll) and the signs that make X
  // positive for "right side down" and Y positive for "far edge down (away)".
  void setOrientation(bool swapXY, float signX, float signY);
  // The gesture starts: the next sample is the zero pose. Responses per axis.
  void press(int responseX, int responseY);
  void release();
  bool held() const { return held_; }
  // One reader sample: acc in g, gyro in rad/s, chip axes. Ignored while not held.
  void sample(Vec3 acc, Vec3 gyro);
  float x() const { return ax_[0].out(); }   // [-1, 1]
  float y() const { return ax_[1].out(); }
  float relXDeg() const { return rel_[0]; }  // the unsmoothed relative angles (tests)
  float relYDeg() const { return rel_[1]; }
  uint32_t samples() const { return n_; }    // since the press

 private:
  struct Fusion {
    Vec3 g;
    float alpha;   // tau / (tau + dt)
  };
  float dt_ = 0.01f;
  bool swap_ = false;
  float sign_[2] = {1.0f, 1.0f};
  bool held_ = false, pending_ = false;
  uint8_t resp_[2] = {kSmooth, kSmooth};
  int8_t src_[2] = {-1, -1};   // per axis: -1 the accelerometer, 0/1 fusion slot
  uint8_t nFus_ = 0;
  Fusion fus_[2] = {};
  Vec3 ex_{1, 0, 0}, ey_{0, 1, 0}, ez_{0, 0, 1};   // the press pose's frame
  Axis ax_[2];
  float rel_[2] = {0.0f, 0.0f};
  uint32_t n_ = 0;
};

}  // namespace tilt
}  // namespace pg
