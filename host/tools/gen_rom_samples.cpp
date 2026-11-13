// gen_rom_samples: synthesises the sampler's built-in sounds (what an empty
// sample pool slot plays, lib/pg/src/pg/sampler/rom_samples.h) and writes them
// as a checked-in C++ table, the way macro_builtin writes the instrument table.
//
//   build/host/gen_rom_samples            rewrite lib/pg/src/pg/sampler/rom_samples_data.cpp
//   build/host/gen_rom_samples --check    exit 1 if the checked-in table differs by more
//                                         than 2 LSB anywhere (ctest check_rom_samples; the
//                                         tolerance absorbs libm differences between hosts)
//   build/host/gen_rom_samples --wav DIR  also write each phrase as DIR/<NAME>.wav
//
// Run it from the repository root. THE SOUNDS: voice-like phrases made by
// additive synthesis, a harmonic source whose partials are weighted by a
// formant envelope (four resonance peaks per vowel) that glides between vowels
// along keyframes, with a pitch contour, vibrato and a little jitter; a noise
// source shaped by two band-passes at the vowel's 2nd and 3rd formant gives the
// breath and the "h". Mono, 22,050 Hz, peak at -1 dBFS, 5 ms fades at the ends,
// TPDF dither from a fixed seed. About 3.1 s in all (137 KB of flash).
//
//   OHYEAH  "oh-yeah", a rise and a fall (1.15 s)
//   HEY     a shouted "hey!" with an upward flick (0.75 s)
//   AHOO    a sung "aah" with vibrato that falls a fourth into "ooh" (1.2 s)
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "common/wav.h"
#include "pg/sampler/rom_samples.h"
#include "pg/sampler/sample_format.h"

namespace ps = pg::sampler;

namespace {

constexpr double kSr = 22050.0;
constexpr double kPi = 3.14159265358979323846;

struct Vowel { double f[4], b[4]; };
// Formant frequencies (Hz) and bandwidths, an adult voice between the classic
// male and female tables.
const Vowel kA = {{760, 1180, 2600, 3500}, {90, 100, 130, 170}};
const Vowel kE = {{520, 1900, 2600, 3550}, {70, 100, 130, 170}};
const Vowel kI = {{300, 2350, 3050, 3650}, {60, 100, 140, 180}};
const Vowel kO = {{560, 880, 2550, 3450}, {80, 90, 130, 170}};
const Vowel kU = {{330, 860, 2350, 3400}, {60, 90, 130, 170}};
const Vowel kAe = {{680, 1750, 2500, 3450}, {90, 110, 130, 170}};

struct Key {
  double t;        // seconds
  Vowel v;
  double f0;       // Hz
  double voiced;   // 0..1
  double breath;   // 0..1 (noise through the formants)
  double vib;      // vibrato depth in semitones
};

struct Phrase {
  const char* name;
  double length;
  std::vector<Key> keys;
};

double smooth(double x) { return x * x * (3.0 - 2.0 * x); }

Key at(const Phrase& p, double t) {
  const std::vector<Key>& k = p.keys;
  if (t <= k.front().t) return k.front();
  if (t >= k.back().t) return k.back();
  size_t i = 0;
  while (i + 1 < k.size() && k[i + 1].t <= t) ++i;
  const Key& a = k[i];
  const Key& b = k[i + 1];
  const double x = smooth((t - a.t) / (b.t - a.t));
  Key o;
  o.t = t;
  for (int j = 0; j < 4; ++j) {
    o.v.f[j] = a.v.f[j] + (b.v.f[j] - a.v.f[j]) * x;
    o.v.b[j] = a.v.b[j] + (b.v.b[j] - a.v.b[j]) * x;
  }
  o.f0 = a.f0 * std::pow(b.f0 / a.f0, x);
  o.voiced = a.voiced + (b.voiced - a.voiced) * x;
  o.breath = a.breath + (b.breath - a.breath) * x;
  o.vib = a.vib + (b.vib - a.vib) * x;
  return o;
}

double envelope(const Vowel& v, double f) {
  static const double g[4] = {1.0, 0.7, 0.32, 0.18};
  double s = 0.0;
  for (int j = 0; j < 4; ++j) {
    const double d = (f - v.f[j]) / (0.5 * v.b[j]);
    s += g[j] / std::sqrt(1.0 + d * d);
  }
  return s;
}

struct Lcg {
  uint32_t s;
  double next() {   // uniform -1..1
    s = s * 1664525u + 1013904223u;
    return double(int32_t(s)) / 2147483648.0;
  }
};

// Two-pole band-pass (constant peak gain), coefficients per sample.
struct Bp {
  double y1 = 0, y2 = 0;
  double tick(double x, double f, double bw) {
    const double r = std::exp(-kPi * bw / kSr);
    const double c = 2.0 * r * std::cos(2.0 * kPi * f / kSr);
    const double y = (1.0 - r) * x + c * y1 - r * r * y2;
    y2 = y1;
    y1 = y;
    return y;
  }
};

std::vector<int16_t> synth(const Phrase& p, uint32_t seed) {
  const int n = int(p.length * kSr);
  std::vector<double> y(size_t(n), 0.0);
  Lcg rng{seed};
  double theta = 0.0, jit = 0.0;
  Bp b2, b3;
  for (int i = 0; i < n; ++i) {
    const double t = double(i) / kSr;
    const Key k = at(p, t);
    jit = 0.995 * jit + 0.005 * rng.next();                // slow random pitch wander
    const double vib = k.vib * std::sin(2.0 * kPi * 5.6 * t);
    const double f0 = k.f0 * std::pow(2.0, (vib + 0.6 * jit) / 12.0);
    theta += 2.0 * kPi * f0 / kSr;
    if (theta > 2.0 * kPi) theta -= 2.0 * kPi;
    double v = 0.0;
    for (int h = 1; h * f0 < 9000.0; ++h) {
      const double fh = h * f0;
      v += envelope(k.v, fh) / double(h) * std::sin(double(h) * theta);
    }
    const double nz = rng.next();
    const double br = 3.0 * b2.tick(nz, k.v.f[1], 220.0) + 2.0 * b3.tick(nz, k.v.f[2], 300.0);
    y[size_t(i)] = k.voiced * v * 0.25 + k.breath * br;
  }
  // DC / rumble: one-pole high-pass at 50 Hz
  double prevX = 0.0, prevY = 0.0;
  const double a = std::exp(-2.0 * kPi * 50.0 / kSr);
  for (double& s : y) {
    const double o = a * (prevY + s - prevX);
    prevX = s;
    prevY = o;
    s = o;
  }
  // 5 ms fades, normalise to -1 dBFS
  const int fade = int(0.005 * kSr);
  for (int i = 0; i < fade && i < n; ++i) {
    const double g = double(i) / fade;
    y[size_t(i)] *= g;
    y[size_t(n - 1 - i)] *= g;
  }
  // Level: the 99.9th percentile of |y| to 0.8, and the few peaks above it rounded
  // off toward -1 dBFS (tanh), so an onset spike does not set the whole phrase's level.
  std::vector<double> mag(y.size());
  for (size_t i = 0; i < y.size(); ++i) mag[i] = std::fabs(y[i]);
  std::sort(mag.begin(), mag.end());
  const double p999 = std::max(1e-9, mag[size_t(double(mag.size() - 1) * 0.999)]);
  const double g = 0.8 / p999, ceil = 0.891;
  std::vector<int16_t> out(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) {
    const double d = 0.5 * (rng.next() + rng.next());     // TPDF, +-1 LSB
    const double v = ceil * std::tanh(g * y[size_t(i)] / ceil);
    double s = std::floor(v * 32767.0 + d + 0.5);
    if (s > 32767) s = 32767;
    if (s < -32768) s = -32768;
    out[size_t(i)] = int16_t(s);
  }
  return out;
}

std::vector<Phrase> phrases() {
  std::vector<Phrase> v;
  v.push_back({"OHYEAH", 1.15,
               {{0.00, kO, 170, 0.0, 0.12, 0}, {0.05, kO, 178, 1.0, 0.05, 0}, {0.28, kO, 196, 1.0, 0.03, 0},
                {0.38, kI, 214, 0.85, 0.04, 0}, {0.47, kE, 226, 1.0, 0.03, 0}, {0.68, kAe, 205, 1.0, 0.03, 0.1},
                {0.92, kA, 172, 0.9, 0.04, 0.25}, {1.10, kA, 150, 0.6, 0.06, 0.3}, {1.15, kA, 145, 0.0, 0.0, 0.3}}});
  v.push_back({"HEY", 0.75,
               {{0.00, kE, 230, 0.0, 0.0, 0}, {0.02, kE, 230, 0.0, 0.25, 0}, {0.09, kE, 232, 0.4, 0.15, 0},
                {0.14, kE, 236, 1.0, 0.08, 0}, {0.36, kE, 246, 1.0, 0.04, 0}, {0.56, kI, 290, 0.9, 0.04, 0},
                {0.70, kI, 300, 0.6, 0.06, 0}, {0.75, kI, 300, 0.0, 0.0, 0}}});
  v.push_back({"AHOO", 1.20,
               {{0.00, kA, 216, 0.0, 0.1, 0.0}, {0.05, kA, 220, 1.0, 0.04, 0.0}, {0.30, kA, 220, 1.0, 0.03, 0.35},
                {0.58, kA, 220, 1.0, 0.03, 0.45}, {0.74, kU, 166, 1.0, 0.03, 0.1}, {1.00, kU, 165, 0.9, 0.03, 0.4},
                {1.17, kU, 163, 0.6, 0.05, 0.4}, {1.20, kU, 163, 0.0, 0.0, 0.4}}});
  return v;
}

std::string emit(const std::vector<Phrase>& ph, const std::vector<std::vector<int16_t>>& data) {
  std::string o;
  char b[256];
  o += "// GENERATED by host/tools/gen_rom_samples.cpp: do not edit. The sampler's built-in\n"
       "// sounds (rom_samples.h): mono int16 at 22,050 Hz, in flash.\n"
       "#include \"rom_samples.h\"\n\nnamespace pg {\nnamespace sampler {\nnamespace {\n\n";
  size_t total = 0;
  for (size_t p = 0; p < ph.size(); ++p) {
    const std::vector<int16_t>& d = data[p];
    total += d.size() * 2;
    std::snprintf(b, sizeof b, "const int16_t kData%zu[%zu] = {", p, d.size());
    o += b;
    for (size_t i = 0; i < d.size(); ++i) {
      if (i % 16 == 0) o += "\n   ";
      std::snprintf(b, sizeof b, " %d,", int(d[i]));
      o += b;
    }
    o += "\n};\n";
    int8_t ov[ps::kOverviewCols][2];
    ps::makeOverview(d.data(), uint32_t(d.size()), ov);
    std::snprintf(b, sizeof b, "const int8_t kOverview%zu[%d][2] = {", p, ps::kOverviewCols);
    o += b;
    for (int c = 0; c < ps::kOverviewCols; ++c) {
      if (c % 8 == 0) o += "\n   ";
      std::snprintf(b, sizeof b, " {%d, %d},", int(ov[c][0]), int(ov[c][1]));
      o += b;
    }
    o += "\n};\n\n";
  }
  o += "}  // namespace\n\nconst RomSample kRom[kRomCount] = {\n";
  for (size_t p = 0; p < ph.size(); ++p) {
    int peak = 0;
    for (int16_t s : data[p]) peak = std::max(peak, std::abs(int(s)));
    std::snprintf(b, sizeof b, "    {kData%zu, %zu, \"%s\", %d, kOverview%zu},\n", p, data[p].size(), ph[p].name, peak, p);
    o += b;
  }
  std::snprintf(b, sizeof b, "};\n// %zu bytes of audio in all.\n\n}  // namespace sampler\n}  // namespace pg\n", total);
  o += b;
  return o;
}

// Every integer in the text, in order (the --check comparison).
std::vector<long> numbers(const std::string& s) {
  std::vector<long> v;
  for (size_t i = 0; i < s.size();) {
    if ((s[i] == '-' && i + 1 < s.size() && std::isdigit(static_cast<unsigned char>(s[i + 1]))) ||
        std::isdigit(static_cast<unsigned char>(s[i]))) {
      if (i > 0 && (std::isalpha(static_cast<unsigned char>(s[i - 1])) || s[i - 1] == '_')) {   // kData0
        while (i < s.size() && std::isalnum(static_cast<unsigned char>(s[i]))) ++i;
        continue;
      }
      char* e = nullptr;
      v.push_back(std::strtol(s.c_str() + i, &e, 10));
      i = size_t(e - s.c_str());
    } else {
      ++i;
    }
  }
  return v;
}

}  // namespace

int main(int argc, char** argv) {
  bool check = false;
  std::string wavDir, out = "lib/pg/src/pg/sampler/rom_samples_data.cpp";
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--check")) check = true;
    else if (!std::strcmp(argv[i], "--wav") && i + 1 < argc) wavDir = argv[++i];
    else out = argv[i];
  }
  const std::vector<Phrase> ph = phrases();
  if (int(ph.size()) != ps::kRomCount) {
    std::fprintf(stderr, "kRomCount is %d, %zu phrases\n", ps::kRomCount, ph.size());
    return 1;
  }
  std::vector<std::vector<int16_t>> data;
  for (size_t p = 0; p < ph.size(); ++p) data.push_back(synth(ph[p], 0x5EED0000u + uint32_t(p)));
  const std::string text = emit(ph, data);
  if (!wavDir.empty())
    for (size_t p = 0; p < ph.size(); ++p)
      wav::write((wavDir + "/" + ph[p].name + ".wav").c_str(), data[p], int(kSr), 1);
  if (check) {
    FILE* f = std::fopen(out.c_str(), "rb");
    if (!f) { std::fprintf(stderr, "cannot read %s\n", out.c_str()); return 1; }
    std::string old;
    char b[65536];
    size_t n;
    while ((n = std::fread(b, 1, sizeof b, f)) > 0) old.append(b, n);
    std::fclose(f);
    const std::vector<long> a = numbers(old), c = numbers(text);
    if (a.size() != c.size()) { std::fprintf(stderr, "%s is stale (size)\n", out.c_str()); return 1; }
    long worst = 0;
    for (size_t i = 0; i < a.size(); ++i) worst = std::max(worst, std::labs(a[i] - c[i]));
    if (worst > 2) { std::fprintf(stderr, "%s is stale (differs by %ld)\n", out.c_str(), worst); return 1; }
    std::printf("%s is current (largest difference %ld)\n", out.c_str(), worst);
    return 0;
  }
  FILE* f = std::fopen(out.c_str(), "wb");
  if (!f) { std::fprintf(stderr, "cannot write %s\n", out.c_str()); return 1; }
  std::fwrite(text.data(), 1, text.size(), f);
  std::fclose(f);
  std::printf("wrote %s\n", out.c_str());
  return 0;
}
