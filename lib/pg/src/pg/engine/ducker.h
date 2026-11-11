// Sidechain ducker: a control-rate gain envelope (one value per quantum, the
// mixer interpolates linearly inside the quantum, so it is click-free).
//
//   trigger -> ATTACK  ~2 ms smoothstep from the current gain down to floor
//           -> HOLD    kGDuckHold
//           -> RELEASE kGDuckRelease, shaped by kGDuckCurve
//   floor = 1 - depth.  A retrigger in any phase restarts ATTACK from wherever
//   the gain currently is, so fast kick rolls never click.
#pragma once
#include "dsp_util.h"

namespace pg {
namespace engine_detail {

class Ducker {
 public:
  void init(float sampleRate, int quantum) {
    quantaPerSec_ = sampleRate / float(quantum);
    attackQuanta_ = int(0.002f * quantaPerSec_ + 0.5f);
    if (attackQuanta_ < 2) attackQuanta_ = 2;
    reset();
    setParams(100, 49, 78, 64);
  }
  void reset() { phase_ = kIdle; gain_ = 1.0f; count_ = 0; }

  // Raw 0..127 params. Cheap; call on change.
  void setParams(int depth, int hold, int release, int curve) {
    floor_ = 1.0f - clampf(float(depth) * (1.0f / 127.0f), 0.0f, 1.0f);
    const float h = float(hold) * (1.0f / 127.0f);
    holdSec_ = 0.2f * h * h;                                           // 0 .. 200 ms
    releaseSec_ = 0.02f * fastPow2(float(release) * (5.643856f / 127.0f));  // 20 ms .. 1 s
    holdQuanta_ = int(holdSec_ * quantaPerSec_ + 0.5f);
    releaseQuanta_ = int(releaseSec_ * quantaPerSec_ + 0.5f);
    if (releaseQuanta_ < 1) releaseQuanta_ = 1;
    // curve: 0 = log (fast start), 64 = linear, 127 = exp (slow start).
    shapeA_ = fastPow2(clampf(float(curve - 64) * (1.0f / 63.0f), -1.0f, 1.0f) * 3.0f);  // 1/8 .. 8
  }

  void trigger() {
    if (floor_ >= 0.9999f) return;
    phase_ = kAttack;
    count_ = 0;
    start_ = gain_;
  }

  // Advance one quantum; returns the gain at the END of that quantum.
  float tick() {
    switch (phase_) {
      case kIdle:
        gain_ = 1.0f;
        break;
      case kAttack: {
        ++count_;
        const float s = float(count_) / float(attackQuanta_);
        const float w = 1.0f - s * s * (3.0f - 2.0f * s);
        gain_ = floor_ + (start_ - floor_) * w;
        if (count_ >= attackQuanta_) {
          gain_ = floor_;
          count_ = 0;
          phase_ = holdQuanta_ > 0 ? kHold : kRelease;
        }
        break;
      }
      case kHold:
        gain_ = floor_;
        if (++count_ >= holdQuanta_) { count_ = 0; phase_ = kRelease; }
        break;
      case kRelease: {
        ++count_;
        if (count_ >= releaseQuanta_) {
          gain_ = 1.0f;
          phase_ = kIdle;
        } else {
          const float x = float(count_) / float(releaseQuanta_);
          const float sh = x / (x + shapeA_ * (1.0f - x));
          gain_ = floor_ + (1.0f - floor_) * sh;
        }
        break;
      }
    }
    return gain_;
  }

  float gain() const { return gain_; }
  float floorGain() const { return floor_; }
  float holdSeconds() const { return holdSec_; }
  float releaseSeconds() const { return releaseSec_; }
  float attackSeconds() const { return float(attackQuanta_) / quantaPerSec_; }

 private:
  enum Phase : uint8_t { kIdle, kAttack, kHold, kRelease };
  // No non-zero default initialisers: the engine state must live in .bss.
  Phase phase_;
  float gain_, start_, floor_, shapeA_;
  float holdSec_, releaseSec_, quantaPerSec_;
  int count_, attackQuanta_, holdQuanta_, releaseQuanta_;
};

}  // namespace engine_detail
}  // namespace pg
