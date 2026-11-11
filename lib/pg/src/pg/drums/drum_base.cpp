// Shared plumbing for the drum voices: parameter cache, retrigger de-click,
// choke fade.
#include "drums.h"

namespace pg {
namespace drums {

DrumVoice::DrumVoice(const ParamInfo* table, int numParams)
    : table_(table), numParams_(numParams > kMaxParams ? kMaxParams : numParams) {
  for (int i = 0; i < kMaxParams; ++i) raw_[i] = p_[i] = 0;
  for (int i = 0; i < numParams_; ++i) raw_[i] = p_[i] = table_[i].def;
}

void DrumVoice::init(float sampleRate) {
  fs_ = clampf(sampleRate, 8000.0f, 192000.0f);
  chokeStep_ = 1.0f / (0.002f * fs_);  // 2 ms linear fade
  residStep_ = 1.0f / (0.0015f * fs_); // 1.5 ms retrigger de-click
  active_ = false;
  choking_ = false;
  chokeGain_ = 1.0f;
  resid_ = residRamp_ = lastOut_ = 0.0f;
  resetState();
  updateCoefs(0xFFFFFFFFu);
}

void DrumVoice::setParams(const uint8_t* soundParams) {
  uint32_t changed = 0;
  for (int i = 0; i < numParams_; ++i) {
    const uint8_t v = soundParams[i];
    if (v != raw_[i]) {
      raw_[i] = v;
      const ParamInfo& pi = table_[i];
      const uint8_t c = v < pi.min ? pi.min : (v > pi.max ? pi.max : v);
      if (c != p_[i]) {
        p_[i] = c;
        changed |= 1u << i;
      }
    }
  }
  if (changed) updateCoefs(changed);
}

void DrumVoice::choke() {
  if (active_ && !choking_) {
    choking_ = true;
    chokeGain_ = 1.0f;
  }
}

void DrumVoice::beginHit(bool declick, float ceiling) {
  ceiling_ = ceiling > 1.0e-6f ? ceiling : 1.0e-6f;
  if (active_ && declick) {
    resid_ = lastOut_;
    residRamp_ = 1.0f;
  } else if (!active_) {
    resid_ = 0.0f;
  }
  choking_ = false;
  chokeGain_ = 1.0f;
  active_ = true;
}

void DrumVoice::endBlock(float* outL, float* outR, int frames, bool stillSounding) {
  if (resid_ != 0.0f) {
    float r = residRamp_;
    const float a = resid_, step = residStep_, c = ceiling_, ic = 1.0f / ceiling_;
    for (int i = 0; i < frames; ++i) {
      r -= step;
      if (r < 0.0f) r = 0.0f;
      // The old tail plus the new attack should not stack above the voice's
      // ceiling. Soft-limited (this used to be a hard clamp that flat-topped
      // fast retriggers).
      outL[i] = softLimit(outL[i] + a * r * r, c, ic);
    }
    residRamp_ = r;
    if (r <= 0.0f) resid_ = 0.0f;
  }
  if (choking_) {
    float c = chokeGain_;
    const float step = chokeStep_;
    for (int i = 0; i < frames; ++i) {
      c -= step;
      if (c < 0.0f) c = 0.0f;
      outL[i] *= c;
    }
    chokeGain_ = c;
    if (c <= 0.0f) {
      choking_ = false;
      stillSounding = false;
      resid_ = 0.0f;
    }
  }
  for (int i = 0; i < frames; ++i) outR[i] = outL[i];
  lastOut_ = outL[frames - 1];
  if (!stillSounding && resid_ == 0.0f) {
    active_ = false;
    lastOut_ = 0.0f;
  }
}

}  // namespace drums
}  // namespace pg
