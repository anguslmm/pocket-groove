// Audio file -> a sample pool slot's audio (pg/sampler/sample_format.h): mono
// int16 at 22,050 Hz, trimmed and normalised. Host code (heap, double, the
// standard library). The desktop app (host/app/samples.*) and host/tools/
// sample_convert use it; tools/pg.py has the same algorithm in Python for the
// device (tools/pg sample put), and tools/test_sample_convert.py checks the two
// against each other on shared test vectors (ctest test_sample_convert).
//
// THE ALGORITHM (keep tools/pg.py convert_samples() in step):
//   1. WAV: PCM 8 (unsigned) / 16 / 24 / 32-bit, IEEE float 32 / 64, plain or
//      WAVE_FORMAT_EXTENSIBLE, any channel count and rate. Samples to doubles
//      (/128, /32768, /8388608, /2147483648; floats as they are).
//   2. Mono: the mean of the channels.
//   3. Resample to 22,050 Hz (unless already): output n reads the input at
//      t = n * rate / 22050; a windowed sinc, cutoff fc = 0.5 * 0.95 * min(1, 22050 / rate)
//      cycles per input sample, half width W = ceil(16 / min(1, 22050 / rate)) input
//      samples, Blackman window over [-W, W]; taps k = floor(t) - W + 1 .. floor(t) + W
//      inside the input, weights normalised to sum 1. Length floor(frames * 22050 / rate).
//   4. Trim: peak P = max |x|; nothing above 0 -> "silent". Keep from 110 samples
//      (5 ms) before the first |x| > P * 10^(-48/20) to 441 samples (20 ms) after the last.
//   5. At most kSlotMaxSamples: longer is cut, and its last 110 samples fade out
//      (sample j of the last 110 times (110 - 1 - j) / 110).
//   6. Normalise: gain = min(0.891 / P', 100) with P' the peak of what is left.
//   7. int16: floor(x * gain * 32767 + 0.5), clamped to -32768..32767.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "pg/sampler/sample_format.h"

namespace sconv {

constexpr int kRate = 22050;

struct Audio {
  int rate = 0;
  int channels = 0;
  std::vector<double> mono;   // the mean of the channels
};

inline uint32_t rd32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
inline uint16_t rd16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }

// Parses a WAV file's bytes. false + err if it is not one this reads.
inline bool parseWav(const std::vector<uint8_t>& f, Audio* out, std::string* err) {
  if (f.size() < 12 || std::memcmp(f.data(), "RIFF", 4) != 0 || std::memcmp(f.data() + 8, "WAVE", 4) != 0) {
    *err = "not a WAV file";
    return false;
  }
  int fmtTag = 0, ch = 0, bits = 0, block = 0;
  uint32_t rate = 0;
  const uint8_t* data = nullptr;
  size_t dataLen = 0;
  for (size_t p = 12; p + 8 <= f.size();) {
    const uint32_t len = rd32(&f[p + 4]);
    const uint8_t* body = &f[p + 8];
    const size_t avail = f.size() - (p + 8);
    if (!std::memcmp(&f[p], "fmt ", 4) && len >= 16 && avail >= 16) {
      fmtTag = rd16(body);
      ch = rd16(body + 2);
      rate = rd32(body + 4);
      block = rd16(body + 12);
      bits = rd16(body + 14);
      if (fmtTag == 0xFFFE && len >= 40 && avail >= 26) fmtTag = rd16(body + 24);   // the sub-format GUID's first word
    } else if (!std::memcmp(&f[p], "data", 4)) {
      data = body;
      dataLen = len < avail ? len : avail;   // a truncated or streaming (0xFFFFFFFF) length: what is there
    }
    p += 8 + size_t(len) + (len & 1u);
    if (len > f.size()) break;
  }
  if (!data || !ch || !rate || !block) {
    *err = "no fmt / data chunk";
    return false;
  }
  const bool pcm = fmtTag == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32);
  const bool flt = fmtTag == 3 && (bits == 32 || bits == 64);
  if (!pcm && !flt) {
    char b[96];
    std::snprintf(b, sizeof b, "unsupported WAV encoding (format %d, %d bits)", fmtTag, bits);
    *err = b;
    return false;
  }
  if (rate < 1000 || rate > 384000 || ch > 32 || block < ch * bits / 8) {
    *err = "implausible WAV header";
    return false;
  }
  const size_t frames = dataLen / size_t(block);
  out->rate = int(rate);
  out->channels = ch;
  out->mono.assign(frames, 0.0);
  const int bytes = bits / 8;
  for (size_t i = 0; i < frames; ++i) {
    double sum = 0.0;
    for (int c = 0; c < ch; ++c) {
      const uint8_t* s = data + i * size_t(block) + size_t(c * bytes);
      double v = 0.0;
      if (flt && bits == 32) {
        float x;
        std::memcpy(&x, s, 4);
        v = double(x);
      } else if (flt) {
        std::memcpy(&v, s, 8);
      } else if (bits == 8) {
        v = (double(s[0]) - 128.0) / 128.0;
      } else if (bits == 16) {
        v = double(int16_t(rd16(s))) / 32768.0;
      } else if (bits == 24) {
        int32_t x = int32_t(uint32_t(s[0]) << 8 | uint32_t(s[1]) << 16 | uint32_t(s[2]) << 24) >> 8;
        v = double(x) / 8388608.0;
      } else {
        v = double(int32_t(rd32(s))) / 2147483648.0;
      }
      if (!std::isfinite(v)) v = 0.0;
      sum += v;
    }
    out->mono[i] = sum / double(ch);
  }
  return true;
}

inline double blackman(double u) {
  if (u <= -1.0 || u >= 1.0) return 0.0;
  const double pi = 3.14159265358979323846;
  return 0.42 + 0.5 * std::cos(pi * u) + 0.08 * std::cos(2.0 * pi * u);
}

inline double sinc(double x) {
  if (x == 0.0) return 1.0;
  const double pi = 3.14159265358979323846;
  return std::sin(pi * x) / (pi * x);
}

// Step 3.
inline std::vector<double> resample(const std::vector<double>& in, int rate) {
  if (rate == kRate) return in;
  const double ratio = double(kRate) / double(rate);
  const double s = ratio < 1.0 ? ratio : 1.0;
  const double fc = 0.5 * 0.95 * s;
  const long W = long(std::ceil(16.0 / s));
  const long n = long((uint64_t(in.size()) * uint64_t(kRate)) / uint64_t(rate));
  std::vector<double> out(size_t(n > 0 ? n : 0));
  std::map<uint64_t, std::vector<double>> kernels;   // by the phase numerator (n * rate) mod 22050
  for (long i = 0; i < n; ++i) {
    const uint64_t num = uint64_t(i) * uint64_t(rate);
    const long base = long(num / kRate);
    const uint64_t ph = num % kRate;
    const double frac = double(ph) / double(kRate);
    std::vector<double>& k = kernels[ph];
    if (k.empty()) {
      k.resize(size_t(2 * W));
      for (long j = 0; j < 2 * W; ++j) {
        const double x = double(j - W + 1) - frac;   // tap position minus t
        k[size_t(j)] = 2.0 * fc * sinc(2.0 * fc * x) * blackman(x / double(W));
      }
    }
    double acc = 0.0, wsum = 0.0;
    for (long j = 0; j < 2 * W; ++j) {
      const long idx = base - W + 1 + j;
      if (idx < 0 || idx >= long(in.size())) continue;
      acc += k[size_t(j)] * in[size_t(idx)];
      wsum += k[size_t(j)];
    }
    out[size_t(i)] = wsum != 0.0 ? acc / wsum : 0.0;
  }
  return out;
}

// Steps 3 to 7 on mono audio at `rate`.
inline bool toSlot(const std::vector<double>& mono, int rate, std::vector<int16_t>* out, std::string* err) {
  std::vector<double> x = resample(mono, rate);
  double peak = 0.0;
  for (double v : x) peak = std::fabs(v) > peak ? std::fabs(v) : peak;
  if (!(peak > 0.0)) {
    *err = "silent";
    return false;
  }
  const double thr = peak * std::pow(10.0, -48.0 / 20.0);
  long first = 0, last = long(x.size()) - 1;
  while (first < long(x.size()) && !(std::fabs(x[size_t(first)]) > thr)) ++first;
  while (last > first && !(std::fabs(x[size_t(last)]) > thr)) --last;
  long a = first - 110, b = last + 1 + 441;
  if (a < 0) a = 0;
  if (b > long(x.size())) b = long(x.size());
  std::vector<double> y(x.begin() + a, x.begin() + b);
  if (y.size() > pg::sampler::kSlotMaxSamples) {
    y.resize(pg::sampler::kSlotMaxSamples);
    const size_t L = y.size();
    for (size_t j = 0; j < 110; ++j) y[L - 110 + j] *= double(110 - 1 - j) / 110.0;
  }
  double p2 = 0.0;
  for (double v : y) p2 = std::fabs(v) > p2 ? std::fabs(v) : p2;
  if (!(p2 > 0.0) || y.size() < pg::sampler::kMinSamples) {
    *err = "too short or silent";
    return false;
  }
  double gain = 0.891 / p2;
  if (gain > 100.0) gain = 100.0;
  out->resize(y.size());
  for (size_t i = 0; i < y.size(); ++i) {
    double v = std::floor(y[i] * gain * 32767.0 + 0.5);
    if (v > 32767.0) v = 32767.0;
    if (v < -32768.0) v = -32768.0;
    (*out)[i] = int16_t(v);
  }
  return true;
}

inline bool readFile(const std::string& path, std::vector<uint8_t>* out) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  out->clear();
  uint8_t b[65536];
  size_t n;
  while ((n = std::fread(b, 1, sizeof b, f)) > 0) out->insert(out->end(), b, b + n);
  std::fclose(f);
  return true;
}

// A WAV file to a slot's audio.
inline bool convertFile(const std::string& path, std::vector<int16_t>* out, std::string* err) {
  std::vector<uint8_t> bytes;
  if (!readFile(path, &bytes)) {
    *err = "cannot read " + path;
    return false;
  }
  Audio a;
  if (!parseWav(bytes, &a, err)) return false;
  return toSlot(a.mono, a.rate, out, err);
}

// "03-ohyeah.wav" -> slot 2 and "ohyeah"; -1 if the name does not start with 01..12 and '-' / '_' / '.'.
inline int slotOfName(const std::string& file, std::string* stem) {
  if (file.size() < 3 || file[0] < '0' || file[0] > '9' || file[1] < '0' || file[1] > '9') return -1;
  const int n = (file[0] - '0') * 10 + (file[1] - '0');
  if (n < 1 || n > pg::sampler::kPoolSlots) return -1;
  if (file[2] != '-' && file[2] != '_' && file[2] != '.') return -1;
  if (stem) {
    std::string s = file.substr(file[2] == '.' ? 2 : 3);
    const size_t dot = s.rfind('.');
    if (dot != std::string::npos) s = s.substr(0, dot);
    *stem = s;
  }
  return n - 1;
}

}  // namespace sconv
