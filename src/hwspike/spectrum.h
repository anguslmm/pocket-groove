// Pocket Groove hardware spike -- averaged spectrum of the microphone input
// for the SAMPLER quality measurement (mic_quality.cpp). Portable,
// header-only, unit-tested on the host (host/hwspike).
//
//  * fftRadix2(): in-place iterative complex FFT, n a power of two.
//  * SpectrumAcc: Welch average of Hann-windowed frames (each frame's mean
//    removed first, so DC never shows as a spur), as power per bin.
//  * worstSpur(): the largest bin at or above minHz, its level as the
//    amplitude of a sine in dBFS (from the main-lobe energy, so a full-scale
//    sine reads 0 dBFS wherever it falls between bins), the median bin as a
//    one-bin sine amplitude (the broadband floor per bin) and how far the
//    peak bin stands above the median bin. A spur well above the median is
//    hum / a whine / an interference line; one near it is just the noise's
//    largest bin.
//
// Buffers are the caller's (the firmware allocates them for one measurement
// only): re / im n floats each, pow n / 2 + 1 floats.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace hws {

inline bool isPow2(int n) { return n >= 2 && (n & (n - 1)) == 0; }

inline void fftRadix2(float* re, float* im, int n) {
  // bit reversal
  for (int i = 1, j = 0; i < n; ++i) {
    int bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) {
      std::swap(re[i], re[j]);
      std::swap(im[i], im[j]);
    }
  }
  for (int len = 2; len <= n; len <<= 1) {
    const int half = len >> 1;
    const float ang = -6.28318530718f / static_cast<float>(len);
    for (int k = 0; k < half; ++k) {
      const float wr = cosf(ang * static_cast<float>(k));
      const float wi = sinf(ang * static_cast<float>(k));
      for (int i = k; i < n; i += len) {
        const int j = i + half;
        const float xr = re[j] * wr - im[j] * wi;
        const float xi = re[j] * wi + im[j] * wr;
        re[j] = re[i] - xr;
        im[j] = im[i] - xi;
        re[i] += xr;
        im[i] += xi;
      }
    }
  }
}

struct SpurResult {
  bool  valid = false;
  int   bin = 0;
  float hz = 0;
  float db = -120;          // sine amplitude, dBFS
  float floorDb = -120;     // median bin on the same scale
  float overFloorDb = 0;    // db - floorDb
};

class SpectrumAcc {
 public:
  // pow: n / 2 + 1 floats. Returns false for a bad n.
  bool begin(float* pow, int n) {
    pow_ = pow;
    n_ = isPow2(n) ? n : 0;
    frames_ = 0;
    if (!pow_ || n_ == 0) return false;
    for (int k = 0; k <= n_ / 2; ++k) pow_[k] = 0.0f;
    return true;
  }
  int n() const { return n_; }
  uint32_t frames() const { return frames_; }

  // x: n samples. re / im: n floats of scratch.
  void addFrame(const int16_t* x, float* re, float* im) {
    if (!pow_ || n_ == 0) return;
    float mean = 0.0f;
    for (int i = 0; i < n_; ++i) mean += static_cast<float>(x[i]);
    mean /= static_cast<float>(n_);
    const float w0 = 6.28318530718f / static_cast<float>(n_);
    for (int i = 0; i < n_; ++i) {
      const float w = 0.5f - 0.5f * cosf(w0 * static_cast<float>(i));
      re[i] = (static_cast<float>(x[i]) - mean) * w;
      im[i] = 0.0f;
    }
    fftRadix2(re, im, n_);
    for (int k = 0; k <= n_ / 2; ++k) pow_[k] += re[k] * re[k] + im[k] * im[k];
    ++frames_;
  }

  // Sine amplitude in dBFS of an averaged bin power.
  float binDb(float p) const {
    if (frames_ == 0 || !(p > 0.0f)) return -120.0f;
    const float mag = sqrtf(p / static_cast<float>(frames_));
    const float amp = 4.0f * mag / static_cast<float>(n_);
    const float d = 20.0f * log10f(amp / 32768.0f);
    return d < -120.0f ? -120.0f : d;
  }

  // scratch: n / 2 + 1 floats (may be re from addFrame once all frames are in).
  SpurResult worstSpur(float rate, float minHz, float* scratch) const {
    SpurResult s;
    if (!pow_ || n_ == 0 || frames_ == 0 || !(rate > 0.0f)) return s;
    const float binHz = rate / static_cast<float>(n_);
    int k0 = static_cast<int>(ceilf(minHz / binHz));
    if (k0 < 3) k0 = 3;   // Hann: bins 0..2 hold what is left of DC
    const int k1 = n_ / 2 - 1;
    if (k0 > k1) return s;
    int best = k0;
    for (int k = k0; k <= k1; ++k)
      if (pow_[k] > pow_[best]) best = k;
    const int m = k1 - k0 + 1;
    for (int i = 0; i < m; ++i) scratch[i] = pow_[k0 + i];
    std::nth_element(scratch, scratch + m / 2, scratch + m);
    s.valid = true;
    s.bin = best;
    s.hz = static_cast<float>(best) * binHz;
    // Level from the energy of the Hann main lobe (peak +-2 bins): a sine of
    // amplitude A puts 3 A^2 N^2 / 32 there wherever it falls between bins,
    // so no scalloping loss (50 Hz hum lies 0.3 bin off at 21.5 Hz bins).
    float e = 0.0f;
    for (int k = best - 2; k <= best + 2; ++k)
      if (k >= 1 && k <= n_ / 2) e += pow_[k];
    e /= static_cast<float>(frames_);
    const float amp = sqrtf(32.0f * e / 3.0f) / static_cast<float>(n_);
    s.db = amp > 0.0f ? 20.0f * log10f(amp / 32768.0f) : -120.0f;
    if (s.db < -120.0f) s.db = -120.0f;
    s.floorDb = binDb(scratch[m / 2]);
    const float med = scratch[m / 2];
    s.overFloorDb = med > 0.0f ? 10.0f * log10f(pow_[best] / med) : 120.0f;   // peak bin vs median bin
    return s;
  }

 private:
  float* pow_ = nullptr;
  int n_ = 0;
  uint32_t frames_ = 0;
};

}  // namespace hws
