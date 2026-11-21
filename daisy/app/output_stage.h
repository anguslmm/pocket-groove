// The audio callback's last step after the engine (App::render, and the bench, which
// times it on its own): the output level in float on the engine's full-resolution
// output, clamped to +-1.0 (libDaisy's f2s24, x 2^23 truncated, is the only rounding
// after it), and the block's largest |sample|. Interleaved stereo, 2 * frames floats.
#pragma once

namespace pgd {

inline float applyOutputLevel(float* out, int frames, float gain) {
  float pk = 0.0f;
  for (int i = 0; i < 2 * frames; ++i) {
    float y = out[i] * gain;
    y = y > 1.0f ? 1.0f : (y < -1.0f ? -1.0f : y);
    out[i] = y;
    const float a = y < 0.0f ? -y : y;
    pk = a > pk ? a : pk;
  }
  return pk;
}

}  // namespace pgd
