#include "lead_tables.h"

#include <cmath>

#include "lead_math.h"

namespace pg {
namespace lead {
namespace {

// log2 sizes. Saw tables are sized so that linear-interpolation images stay
// below about -54 dB re the fundamental (image power ~ 2 H^3 / (3 N^4)).
constexpr uint8_t kSawLog2[kNumBands] = {11, 10, 10, 10, 9, 9, 8, 8, 8, 7, 7, 7, 6, 6, 6};
// Triangle harmonics fall as 1/k^2: one table serves bands 0..3.
constexpr int     kTriTables = 12;  // band 0..3 -> table 0, band b>=4 -> table b-3
constexpr uint8_t kTriLog2[kTriTables] = {8, 7, 7, 7, 7, 7, 6, 6, 6, 6, 6, 6};
constexpr uint8_t kOrgLog2[kOrgLevels] = {8, 7, 6};
constexpr uint8_t kSineLog2 = 9;

constexpr int poolSize() {
  int n = 0;
  for (int i = 0; i < kNumBands; ++i) n += (1 << kSawLog2[i]) + 1;
  for (int i = 0; i < kTriTables; ++i) n += (1 << kTriLog2[i]) + 1;
  for (int i = 0; i < kOrgLevels; ++i) n += kOrgRegs * ((1 << kOrgLog2[i]) + 1);
  n += (1 << kSineLog2) + 1;
  return n;
}
constexpr int kPoolSize = poolSize();

int16_t gPool[kPoolSize];
Tables  gTables;

// Drawbars 16', 5 1/3', 8', 4', 2 2/3', 2', 1 3/5', 1 1/3', 1' as harmonics of the 16' pitch.
constexpr int     kOrgPartials = 9;
constexpr uint8_t kOrgHarm[kOrgPartials] = {1, 3, 2, 4, 6, 8, 10, 12, 16};
// Fixed scattered start phases (in 1/2048 cycles) keep the crest factor low.
constexpr uint16_t kOrgPhase[kOrgPartials] = {0, 731, 1290, 402, 1811, 955, 1517, 148, 1102};
constexpr uint8_t kOrgReg[kOrgRegs][kOrgPartials] = {
    {0, 0, 8, 0, 0, 0, 0, 0, 0},  // pure 8' sine
    {8, 0, 8, 0, 0, 0, 0, 0, 0},  // 16' + 8': deep
    {8, 8, 8, 0, 0, 0, 0, 0, 0},  // 88 8000 000: the classic house organ
    {8, 8, 8, 8, 0, 0, 0, 0, 0},  // + 4'
    {8, 8, 8, 8, 0, 6, 0, 0, 6},  // + 2' and 1' whistle
    {8, 8, 8, 8, 8, 8, 8, 8, 8},  // full
};
constexpr float kOrgRms = 0.40f;  // every registration is normalised to this RMS

// sin(2*pi*i/2048) from a quarter-wave table q[0..512].
inline float qsin(const float* q, uint32_t i) {
  i &= 2047u;
  const uint32_t j = i & 1023u;
  const float v = (j <= 512u) ? q[j] : q[1024u - j];
  return (i & 1024u) ? -v : v;
}

inline int16_t toI16(float v) {
  v = clampf(v, -32767.0f, 32767.0f);
  return int16_t(v >= 0.0f ? v + 0.5f : v - 0.5f);
}

Tab makeTab(const int16_t* d, int log2n) {
  Tab t;
  t.d = d;
  t.shift = uint32_t(32 - log2n);
  t.mask = (1u << t.shift) - 1u;
  t.fscale = 1.0f / float(1u << t.shift);
  return t;
}

}  // namespace

const Tables& tables() { return gTables; }

void buildTables(float fs) {
  float q[513];
  for (int i = 0; i <= 512; ++i) q[i] = sinf(float(i) * (kPi / 1024.0f));
  float recip[256];
  recip[0] = 0.0f;
  for (int k = 1; k < 256; ++k) recip[k] = 1.0f / float(k);

  int16_t* p = gPool;
  const float fMax = 0.49f * fs;
  int bandH[kNumBands];

  // ---- saw
  for (int b = 0; b < kNumBands; ++b) {
    const int log2n = kSawLog2[b];
    const int n = 1 << log2n;
    const float fHi = 440.0f * powf(2.0f, (kBandBasePitch + 6.0f * float(b + 1) - 69.0f) / 12.0f);
    int h = int(fMax / fHi);
    if (h < 1) h = 1;
    if (h > 255) h = 255;
    if (h > n / 2 - 1) h = n / 2 - 1;
    bandH[b] = h;
    const uint32_t step = uint32_t(2048 >> log2n);
    const float amp = -kTabAmp * (2.0f / kPi);
    for (int i = 0; i < n; ++i) {
      float acc = 0.0f;
      const uint32_t base = uint32_t(i) * step;
      for (int k = h; k >= 1; --k) acc += recip[k] * qsin(q, base * uint32_t(k));
      p[i] = toI16(amp * acc);
    }
    p[n] = p[0];
    gTables.saw[b] = makeTab(p, log2n);
    p += n + 1;
  }

  // ---- triangle
  for (int t = 0; t < kTriTables; ++t) {
    const int b = t + 3;  // the band whose harmonic limit applies
    const int log2n = kTriLog2[t];
    const int n = 1 << log2n;
    int h = bandH[b];
    if (h > 63) h = 63;
    if (h > n / 2 - 1) h = n / 2 - 1;
    const uint32_t step = uint32_t(2048 >> log2n);
    const float amp = -kTabAmp * (8.0f / (kPi * kPi));
    for (int i = 0; i < n; ++i) {
      float acc = 0.0f;
      const uint32_t base = uint32_t(i) * step;
      for (int k = 1; k <= h; k += 2) {
        const float c = recip[k] * recip[k];
        acc += (((k >> 1) & 1) ? -c : c) * qsin(q, base * uint32_t(k));
      }
      p[i] = toI16(amp * acc);
    }
    p[n] = p[0];
    const Tab tab = makeTab(p, log2n);
    if (t == 0) {
      for (int bb = 0; bb <= 3; ++bb) gTables.tri[bb] = tab;
    } else {
      gTables.tri[b] = tab;
    }
    p += n + 1;
  }

  // ---- organ
  for (int r = 0; r < kOrgRegs; ++r) {
    float sumSq = 0.0f;
    for (int j = 0; j < kOrgPartials; ++j) sumSq += float(kOrgReg[r][j]) * float(kOrgReg[r][j]);
    const float amp = kTabAmp * kOrgRms / sqrtf(0.5f * sumSq);
    for (int l = 0; l < kOrgLevels; ++l) {
      const int log2n = kOrgLog2[l];
      const int n = 1 << log2n;
      const int hMax = 16 >> l;
      const uint32_t step = uint32_t(2048 >> log2n);
      for (int i = 0; i < n; ++i) {
        float acc = 0.0f;
        for (int j = 0; j < kOrgPartials; ++j) {
          if (kOrgHarm[j] > hMax || kOrgReg[r][j] == 0) continue;
          acc += float(kOrgReg[r][j]) * qsin(q, uint32_t(i) * step * kOrgHarm[j] + kOrgPhase[j]);
        }
        p[i] = toI16(amp * acc);
      }
      p[n] = p[0];
      gTables.organ[r][l] = makeTab(p, log2n);
      p += n + 1;
    }
  }

  // ---- sine
  {
    const int n = 1 << kSineLog2;
    const uint32_t step = uint32_t(2048 >> kSineLog2);
    for (int i = 0; i < n; ++i) p[i] = toI16(kTabAmp * qsin(q, uint32_t(i) * step));
    p[n] = p[0];
    gTables.sine = makeTab(p, kSineLog2);
    p += n + 1;
  }

  // Organ mip thresholds: level 0 has partials up to 8x the note, level 1 up to 4x, level 2 up to 2x.
  gTables.organPitch1 = 69.0f + 12.0f * log2f(fMax / 8.0f / 440.0f);
  gTables.organPitch2 = 69.0f + 12.0f * log2f(fMax / 4.0f / 440.0f);
  gTables.sampleRate = fs;
}

}  // namespace lead
}  // namespace pg
