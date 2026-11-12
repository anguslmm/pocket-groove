// Pocket Groove hardware spike -- test tone and tone detection for the
// microphone matrix (mic_matrix.cpp). Portable, header-only, unit-tested on
// the host (host/hwspike).
//
//  * ToneTable: one exact period block of a sine at freq / rate. Its length L
//    is rate / gcd(rate, freq) samples and holds freq / gcd cycles, so a table
//    walk repeats with no phase error (1 kHz at 44.1 kHz: L = 441 = 10 cycles;
//    at 16 kHz: L = 16 = 1 cycle).
//  * ToneGen: plays the table (the speaker stimulus).
//  * ToneDetector: correlates the input with the table's cos / sin (a DFT bin
//    exactly on the tone), plus DC / RMS / peak / stuck statistics. Over a
//    whole number of table lengths the bin has no leakage, so the tone's
//    amplitude and the residual (everything else) come out exact. Inner sums
//    are float over one table length; totals are double, at block end only.
//  * judgeMic(): the verdict and ranking score of one capture.
#pragma once
#include <cmath>
#include <cstdint>

namespace hws {

constexpr int kToneTableMax = 512;   // 1 kHz needs 441 at 44.1 kHz, 16 at 16 kHz, 32 at 32 kHz

inline uint32_t gcdU(uint32_t a, uint32_t b) {
  while (b) {
    const uint32_t t = a % b;
    a = b;
    b = t;
  }
  return a;
}

struct ToneTable {
  uint32_t freq = 0, rate = 0;
  int      len = 0;              // samples per exact repeat; 0 = invalid
  float    c[kToneTableMax];
  float    s[kToneTableMax];

  // Returns false (len = 0) if freq / rate need more than kToneTableMax samples per repeat.
  bool init(uint32_t freqHz, uint32_t rateHz) {
    freq = freqHz;
    rate = rateHz;
    len = 0;
    if (freqHz == 0 || rateHz == 0 || 2 * freqHz >= rateHz) return false;
    const uint32_t l = rateHz / gcdU(rateHz, freqHz);
    if (l > static_cast<uint32_t>(kToneTableMax)) return false;
    len = static_cast<int>(l);
    for (int i = 0; i < len; ++i) {
      // phase = 2 pi * ((freq * i) mod rate) / rate: integer mod keeps it exact
      const uint64_t m = (static_cast<uint64_t>(freqHz) * static_cast<uint64_t>(i)) % rateHz;
      const double ph = 6.283185307179586 * static_cast<double>(m) / static_cast<double>(rateHz);
      c[i] = static_cast<float>(std::cos(ph));
      s[i] = static_cast<float>(std::sin(ph));
    }
    return true;
  }
};

struct ToneGen {
  const ToneTable* t = nullptr;
  int pos = 0;
  float amp = 0;   // LSB peak
  void start(const ToneTable* table, float ampLsb) {
    t = table;
    pos = 0;
    amp = ampLsb;
  }
  int16_t next() {
    if (!t || t->len == 0) return 0;
    float v = amp * t->s[pos];
    if (++pos >= t->len) pos = 0;
    if (v > 32767.0f) v = 32767.0f;
    if (v < -32768.0f) v = -32768.0f;
    return static_cast<int16_t>(lrintf(v));
  }
};

struct ToneResult {
  uint32_t n = 0;
  bool     stuck = false;     // every sample identical (n > 1)
  int16_t  mn = 0, mx = 0;
  float    dcLsb = 0;
  float    acRmsDb = -120;    // RMS with DC removed, dBFS (32768 = 0 dBFS)
  float    peakDb = -120;     // max |x|
  float    toneAmpLsb = 0;    // amplitude (peak) of the component at freq
  float    toneDb = -120;     // the same in dBFS
  float    residualDb = -120; // RMS of everything except DC and the tone, dBFS
  float    snrDb = -120;      // tone power / residual power, dB (capped at +120)
};

class ToneDetector {
 public:
  void begin(const ToneTable* t) {
    t_ = t;
    clear();
  }
  void clear() {
    pos_ = 0;
    re_ = im_ = 0.0f;
    reT_ = imT_ = 0.0;
    sum_ = 0;
    sumsq_ = 0;
    n_ = 0;
    peak_ = 0;
    mn_ = 32767;
    mx_ = -32768;
  }
  void add(int16_t x) {
    const int32_t v = x;
    sum_ += v;
    sumsq_ += static_cast<uint64_t>(static_cast<int64_t>(v) * v);
    const int32_t a = v < 0 ? -v : v;
    if (a > peak_) peak_ = a;
    if (x < mn_) mn_ = x;
    if (x > mx_) mx_ = x;
    ++n_;
    if (!t_ || t_->len == 0) return;
    const float f = static_cast<float>(v);
    re_ += f * t_->c[pos_];
    im_ += f * t_->s[pos_];
    if (++pos_ >= t_->len) {
      pos_ = 0;
      reT_ += static_cast<double>(re_);
      imT_ += static_cast<double>(im_);
      re_ = im_ = 0.0f;
    }
  }
  uint32_t count() const { return n_; }

  ToneResult result() const {
    ToneResult r;
    r.n = n_;
    if (n_ == 0) return r;
    r.mn = mn_;
    r.mx = mx_;
    r.stuck = (n_ > 1 && mn_ == mx_);
    const double n = static_cast<double>(n_);
    const double mean = static_cast<double>(sum_) / n;
    double var = static_cast<double>(sumsq_) / n - mean * mean;
    if (var < 0.0) var = 0.0;
    r.dcLsb = static_cast<float>(mean);
    r.acRmsDb = db(std::sqrt(var));
    r.peakDb = db(static_cast<double>(peak_));
    if (t_ && t_->len > 0) {
      const double re = reT_ + static_cast<double>(re_);
      const double im = imT_ + static_cast<double>(im_);
      // X = sum x[n] e^{-jwn}; a sine of amplitude A gives |X| = A n / 2.
      const double amp = 2.0 * std::sqrt(re * re + im * im) / n;
      const double tonePow = amp * amp / 2.0;
      double resid = var - tonePow;
      if (resid < 0.0) resid = 0.0;
      r.toneAmpLsb = static_cast<float>(amp);
      r.toneDb = db(amp);
      r.residualDb = db(std::sqrt(resid));
      if (tonePow <= 0.0) r.snrDb = -120.0f;
      else if (resid <= tonePow * 1e-12) r.snrDb = 120.0f;
      else r.snrDb = static_cast<float>(10.0 * std::log10(tonePow / resid));
    }
    return r;
  }

 private:
  static float db(double lsb) {
    if (!(lsb > 0.0)) return -120.0f;
    const double d = 20.0 * std::log10(lsb / 32768.0);
    return d < -120.0 ? -120.0f : static_cast<float>(d);
  }
  const ToneTable* t_ = nullptr;
  int pos_ = 0;
  float re_ = 0, im_ = 0;
  double reT_ = 0, imT_ = 0;
  int64_t sum_ = 0;
  uint64_t sumsq_ = 0;
  uint32_t n_ = 0;
  int32_t peak_ = 0;
  int16_t mn_ = 32767, mx_ = -32768;
};

// ---------------------------------------------------------------------------
// Verdict of one capture.
//   alive: not stuck, at least kAliveSpanLsb between min and max, and an AC
//          RMS above kAliveRmsDb. A powered-down modulator behind the codec's
//          DC filter settles to a constant (often -1 or 0) or toggles by 1 LSB;
//          a live MEMS mic at 0..30 dB PGA is far above both thresholds.
//   tone:  the stimulus is in the signal: tone level above kToneMinDb and at
//          least kToneSnrDb above everything else.
//   works = alive. score ranks: works, then tone, then (only with the tone)
//   its SNR; without a tone, equal scores keep the matrix order (the plain
//   setups come first), so random noise in the tone bin never decides.
constexpr float kAliveRmsDb = -87.0f;    // ~1.5 LSB RMS
constexpr int   kAliveSpanLsb = 8;
constexpr float kToneMinDb = -90.0f;
constexpr float kToneSnrDb = 10.0f;

struct MicVerdict {
  bool  alive = false;
  bool  tone = false;
  float score = -1.0e9f;
};

inline MicVerdict judgeMic(const ToneResult& r) {
  MicVerdict v;
  if (r.n < 2) return v;
  const int span = static_cast<int>(r.mx) - static_cast<int>(r.mn);
  v.alive = !r.stuck && span >= kAliveSpanLsb && r.acRmsDb > kAliveRmsDb;
  v.tone = v.alive && r.toneDb > kToneMinDb && r.snrDb >= kToneSnrDb;
  float snr = r.snrDb;
  if (snr < -60.0f) snr = -60.0f;
  if (snr > 80.0f) snr = 80.0f;
  v.score = (v.alive ? 1000.0f : 0.0f) + (v.tone ? 500.0f + 2.0f * snr : 0.0f) + (r.stuck ? -100.0f : 0.0f);
  return v;
}

}  // namespace hws
