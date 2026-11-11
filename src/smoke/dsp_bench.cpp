#include "dsp_bench.h"

#include <atomic>
#include <cmath>
#include <cstring>

namespace smoke {
namespace bench {

namespace {

constexpr int kMaxN = 128;
constexpr int kMaxFrames = 128;

// ---- request (UI -> audio) --------------------------------------------------
std::atomic<uint32_t> s_reqSeq{0};
volatile uint8_t s_reqKind = kOff;
volatile int     s_reqN = 0;
volatile float   s_reqFs = 44100.0f;

// ---- state (audio thread only) ------------------------------------------------
uint32_t s_appliedSeq = 0;
Kind     s_kind = kOff;
int      s_n = 0;
volatile Kind s_activeKind = kOff;   // mirror for the UI
volatile int  s_activeN = 0;

float s_in[kMaxFrames];     // shared test input (a plain saw), regenerated every block
float s_mix[kMaxFrames];
float s_inPhase = 0.0f;
volatile float s_sink = 0.0f;

// saw
float s_ph[kMaxN], s_inc[kMaxN], s_invInc[kMaxN];
// svf
float s_ic1[kMaxN], s_ic2[kMaxN], s_a1[kMaxN], s_a2[kMaxN], s_a3[kMaxN];
// sat
float s_y[kMaxN], s_a[kMaxN], s_drive[kMaxN];
// ladder
constexpr int kMaxLadder = 16;
float s_ls[kMaxLadder][4], s_lg[kMaxLadder], s_lk[kMaxLadder];
// fdn: 8 prime lengths, ~18 KB of float delay memory shared by all instances
constexpr int kFdnLines = 8;
constexpr int kFdnLen[kFdnLines] = {389, 433, 487, 541, 593, 647, 701, 761};
constexpr int kFdnTotal = 389 + 433 + 487 + 541 + 593 + 647 + 701 + 761;
constexpr int kMaxFdn = 4;
// Allocated on first use by configure() (UI thread), never freed: keeps 18 KB
// out of the heap numbers unless the FDN kernel is actually exercised.
float* volatile s_fdnMem = nullptr;
float* (*s_alloc)(unsigned bytes) = nullptr;
int   s_fdnIdx[kMaxFdn][kFdnLines];
float s_fdnLp[kMaxFdn][kFdnLines];
// libm
float s_lx[kMaxN];

inline float tanhRat(float x) {
  // Pade-style rational approximation, exact sign/0, error < 2.5% up to |x| = 3, clamped beyond.
  if (x > 3.0f) x = 3.0f;
  if (x < -3.0f) x = -3.0f;
  const float x2 = x * x;
  return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

void applyRequest() {
  const uint32_t seq = s_reqSeq.load(std::memory_order_acquire);
  if (seq == s_appliedSeq) return;
  s_appliedSeq = seq;
  Kind k = static_cast<Kind>(s_reqKind);
  int n = s_reqN;
  const float fs = s_reqFs;
  if (k >= kNumKinds) k = kOff;
  if (n < 0) n = 0;
  if (n > maxInstances(k)) n = maxInstances(k);
  const float pi = 3.14159265f;
  for (int i = 0; i < kMaxN; ++i) {
    // spread voices over ~55..880 Hz / filters over 200..4000 Hz
    const float t = static_cast<float>(i % 16) / 15.0f;
    const float f = 55.0f * exp2f(4.0f * t) * (1.0f + 0.001f * static_cast<float>(i));
    s_ph[i] = static_cast<float>(i) * 0.0137f;
    s_inc[i] = f / fs;
    s_invInc[i] = 1.0f / s_inc[i];
    const float fc = 200.0f * exp2f(4.3f * t);
    const float g = tanf(pi * fc / fs);
    const float kq = 0.5f;                       // 1/Q, fairly resonant
    s_a1[i] = 1.0f / (1.0f + g * (g + kq));
    s_a2[i] = g * s_a1[i];
    s_a3[i] = g * s_a2[i];
    s_ic1[i] = s_ic2[i] = 0.0f;
    s_y[i] = 0.0f;
    s_a[i] = 1.0f - expf(-2.0f * pi * fc / fs);
    s_drive[i] = 1.0f + 3.0f * t;
    s_lx[i] = 0.1f + 0.01f * static_cast<float>(i);
  }
  for (int i = 0; i < kMaxLadder; ++i) {
    for (float& v : s_ls[i]) v = 0.0f;
    const float fc = 300.0f + 200.0f * static_cast<float>(i);
    s_lg[i] = 1.0f - expf(-2.0f * pi * fc / (2.0f * fs));   // at the 2x rate
    s_lk[i] = 3.2f;                                          // near self-oscillation
  }
  if (k == kFdn && !s_fdnMem) { k = kOff; n = 0; }                      // no memory: refuse
  if (k == kFdn) memset(s_fdnMem, 0, sizeof(float) * kFdnTotal);
  for (int j = 0; j < kMaxFdn; ++j)
    for (int l = 0; l < kFdnLines; ++l) { s_fdnIdx[j][l] = (j * 37) % kFdnLen[l]; s_fdnLp[j][l] = 0.0f; }
  s_kind = k;
  s_n = n;
  s_activeKind = k;
  s_activeN = n;
}

void runSaw(int frames) {
  for (int v = 0; v < s_n; ++v) {
    float p = s_ph[v];
    const float dt = s_inc[v], idt = s_invInc[v];
    for (int i = 0; i < frames; ++i) {
      p += dt;
      if (p >= 1.0f) p -= 1.0f;
      float y = 2.0f * p - 1.0f;
      if (p < dt) {
        const float t = p * idt;
        y -= t + t - t * t - 1.0f;
      } else if (p > 1.0f - dt) {
        const float t = (p - 1.0f) * idt;
        y -= t * t + t + t + 1.0f;
      }
      s_mix[i] += y;
    }
    s_ph[v] = p;
  }
}

void runSvf(int frames) {
  for (int v = 0; v < s_n; ++v) {
    float ic1 = s_ic1[v], ic2 = s_ic2[v];
    const float a1 = s_a1[v], a2 = s_a2[v], a3 = s_a3[v];
    for (int i = 0; i < frames; ++i) {
      const float v3 = s_in[i] - ic2;
      const float v1 = a1 * ic1 + a2 * v3;
      const float v2 = ic2 + a2 * ic1 + a3 * v3;
      ic1 = 2.0f * v1 - ic1;
      ic2 = 2.0f * v2 - ic2;
      s_mix[i] += v2;
    }
    s_ic1[v] = ic1;
    s_ic2[v] = ic2;
  }
}

void runSat(int frames) {
  for (int v = 0; v < s_n; ++v) {
    float y = s_y[v];
    const float a = s_a[v], d = s_drive[v];
    for (int i = 0; i < frames; ++i) {
      y += a * (s_in[i] - y);
      s_mix[i] += tanhRat(y * d);
    }
    s_y[v] = y;
  }
}

void runLadder(int frames) {
  for (int v = 0; v < s_n; ++v) {
    float s1 = s_ls[v][0], s2 = s_ls[v][1], s3 = s_ls[v][2], s4 = s_ls[v][3];
    const float g = s_lg[v], k = s_lk[v];
    float prevIn = 0.0f;
    for (int i = 0; i < frames; ++i) {
      const float x = s_in[i];
      float acc = 0.0f;
      for (int os = 0; os < 2; ++os) {
        const float xi = os ? x : 0.5f * (x + prevIn);   // linear-interpolated upsample
        const float t4 = tanhRat(s4);
        const float u = tanhRat(xi - k * t4);
        const float t1 = tanhRat(s1), t2 = tanhRat(s2), t3 = tanhRat(s3);
        s1 += g * (u - t1);
        s2 += g * (t1 - t2);
        s3 += g * (t2 - t3);
        s4 += g * (t3 - t4);
        acc += s4;
      }
      prevIn = x;
      s_mix[i] += 0.5f * acc;                            // 2-tap decimation
    }
    s_ls[v][0] = s1; s_ls[v][1] = s2; s_ls[v][2] = s3; s_ls[v][3] = s4;
  }
}

void runFdn(int frames) {
  for (int j = 0; j < s_n; ++j) {
    int* idx = s_fdnIdx[j];
    float* lp = s_fdnLp[j];
    for (int i = 0; i < frames; ++i) {
      float o[kFdnLines];
      float* base = s_fdnMem;
      for (int l = 0; l < kFdnLines; ++l) {
        const float r = base[idx[l]];
        lp[l] += 0.6f * (r - lp[l]);                     // damping
        o[l] = lp[l];
        base += kFdnLen[l];
      }
      // 8-point fast Walsh-Hadamard transform (orthogonal mix)
      for (int h = 1; h < kFdnLines; h <<= 1)
        for (int a = 0; a < kFdnLines; a += h << 1)
          for (int b = a; b < a + h; ++b) {
            const float p = o[b], q = o[b + h];
            o[b] = p + q;
            o[b + h] = p - q;
          }
      const float fb = 0.85f * 0.35355339f;              // decay * 1/sqrt(8)
      const float x = s_in[i] * 0.25f;
      base = s_fdnMem;
      float out = 0.0f;
      for (int l = 0; l < kFdnLines; ++l) {
        base[idx[l]] = x + fb * o[l];
        out += lp[l];
        if (++idx[l] >= kFdnLen[l]) idx[l] = 0;
        base += kFdnLen[l];
      }
      s_mix[i] += out;
    }
  }
}

void runLibm(int frames, Kind k) {
  float acc = 0.0f;
  for (int v = 0; v < s_n; ++v) {
    float x = s_lx[v];
    for (int i = 0; i < frames; ++i) {
      x += 0.0007f;
      if (x > 2.5f) x -= 2.4f;
      float y;
      switch (k) {
        case kDiv:   y = 1.0f / (x + s_in[i] * 0.1f); break;
        case kSinf:  y = sinf(x * 2.5f); break;
        case kExpf:  y = expf(-x); break;
        default:     y = tanhf(x - 1.2f); break;
      }
      s_mix[i] += y;
    }
    s_lx[v] = x;
    acc += x;
  }
  s_sink = s_sink + acc;
}

}  // namespace

const char* name(Kind k) {
  switch (k) {
    case kOff:    return "off";
    case kSaw:    return "polyblep saw";
    case kSvf:    return "TPT SVF";
    case kSat:    return "1pole+tanhRat";
    case kLadder: return "ladder 2xOS";
    case kFdn:    return "FDN8 reverb";
    case kDiv:    return "float div";
    case kSinf:   return "sinf";
    case kExpf:   return "expf";
    case kTanhf:  return "tanhf";
    default:      return "?";
  }
}

int maxInstances(Kind k) {
  switch (k) {
    case kOff:    return 0;
    case kLadder: return kMaxLadder;
    case kFdn:    return kMaxFdn;
    default:      return kMaxN;
  }
}

int autoInstances(Kind k) {
  switch (k) {
    case kSaw: case kSvf: case kSat: return 16;
    case kLadder: return 2;
    case kFdn: return 1;
    case kDiv: return 8;
    default: return 1;      // libm
  }
}

void setAllocator(float* (*alloc)(unsigned bytes)) { s_alloc = alloc; }

void configure(Kind k, int n, float sampleRate) {
  if (k == kFdn && !s_fdnMem && s_alloc) s_fdnMem = s_alloc(sizeof(float) * kFdnTotal);
  s_reqKind = k;
  s_reqN = n;
  s_reqFs = sampleRate;
  s_reqSeq.fetch_add(1, std::memory_order_release);
}

Kind activeKind() { return s_activeKind; }
int activeInstances() { return s_activeN; }

void process(int frames) {
  applyRequest();
  if (s_kind == kOff || s_n == 0) return;
  if (frames > kMaxFrames) frames = kMaxFrames;
  // test input: a 110 Hz-ish naive saw at the configured rate
  const float inc = 110.0f / s_reqFs;
  float p = s_inPhase;
  for (int i = 0; i < frames; ++i) {
    p += inc;
    if (p >= 1.0f) p -= 1.0f;
    s_in[i] = 2.0f * p - 1.0f;
    s_mix[i] = 0.0f;
  }
  s_inPhase = p;
  switch (s_kind) {
    case kSaw:    runSaw(frames); break;
    case kSvf:    runSvf(frames); break;
    case kSat:    runSat(frames); break;
    case kLadder: runLadder(frames); break;
    case kFdn:    runFdn(frames); break;
    default:      runLibm(frames, s_kind); break;
  }
  float acc = 0.0f;
  for (int i = 0; i < frames; ++i) acc += s_mix[i];
  s_sink = s_sink + acc;
}

// ---- primitives ----------------------------------------------------------------
namespace {

constexpr int kIters = 256;   // x 8 unrolled ops
volatile float  s_vf = 1.0001f;
volatile double s_vd = 1.0001;
volatile int    s_vi = 0;
float s_lut[257];

#define PG_REP8(x) x x x x x x x x

template <typename F>
float minCycles(CycleFn cycles, F&& body) {
  uint32_t best = 0xFFFFFFFFu;
  for (int run = 0; run < 7; ++run) {
    const uint32_t t0 = cycles();
    body();
    const uint32_t dt = cycles() - t0;
    if (dt < best) best = dt;
  }
  return static_cast<float>(best) / static_cast<float>(kIters * 8);
}

}  // namespace

void measurePrimitives(CycleFn cycles, Primitives* out) {
  if (!out || !cycles) return;
  for (int i = 0; i <= 256; ++i) s_lut[i] = sinf(6.2831853f * static_cast<float>(i) / 256.0f);

  // Loop overhead: same loop shape with a dependent float add only.
  const float base = minCycles(cycles, [] {
    float x = s_vf;
    const float b = 1e-7f;
    for (int i = 0; i < kIters; ++i) { PG_REP8(x = x + b;) }
    s_vf = x;
  });
  // Dependent chains below are reported raw (cycles per op incl. ~1/8 of the
  // loop branch). Composite ones that also advance x (f2i16, lerpLut) have
  // the cost of that dependent add subtracted.
  auto net = [base](float v) { return v - base > 0.0f ? v - base : 0.0f; };
  out->fadd = base;

  out->fmadd = (minCycles(cycles, [] {
    float x = s_vf;
    const float a = 0.9999f, b = 1e-4f;
    for (int i = 0; i < kIters; ++i) { PG_REP8(x = x * a + b;) }
    s_vf = x;
  }));   // mul-add replaces the add, so add the base back as its own cost
  out->fdiv = (minCycles(cycles, [] {
    float x = s_vf;
    const float a = 1.0001f;
    for (int i = 0; i < kIters; ++i) { PG_REP8(x = a / x;) }
    s_vf = x;
  }));
  out->f2i16 = net(minCycles(cycles, [] {
    float x = s_vf;
    int acc = 0;
    const float b = 1e-7f;
    for (int i = 0; i < kIters; ++i) {
      PG_REP8({
        x = x + b;
        float s = x * 32767.0f;
        if (s > 32767.0f) s = 32767.0f;
        if (s < -32768.0f) s = -32768.0f;
        acc += static_cast<int16_t>(s);
      })
    }
    s_vf = x;
    s_vi = acc;
  }));
  out->lerpLut = net(minCycles(cycles, [] {
    float p = s_vf;
    float acc = 0.0f;
    const float inc = 3.7f;
    for (int i = 0; i < kIters; ++i) {
      PG_REP8({
        p = p + inc;
        if (p >= 256.0f) p -= 256.0f;
        const int k = static_cast<int>(p);
        const float f = p - static_cast<float>(k);
        acc += s_lut[k] + f * (s_lut[k + 1] - s_lut[k]);
      })
    }
    s_vf = p + acc * 1e-9f;
  }));
  out->sinf_ = (minCycles(cycles, [] {
    float x = s_vf;
    for (int i = 0; i < kIters; ++i) { PG_REP8(x = sinf(x) + 0.5f;) }
    s_vf = x;
  }));
  out->expf_ = (minCycles(cycles, [] {
    float x = s_vf;
    for (int i = 0; i < kIters; ++i) { PG_REP8(x = expf(-x) + 0.1f;) }
    s_vf = x;
  }));
  out->tanhf_ = (minCycles(cycles, [] {
    float x = s_vf;
    for (int i = 0; i < kIters; ++i) { PG_REP8(x = tanhf(x) + 0.5f;) }
    s_vf = x;
  }));
  out->tanhRat = (minCycles(cycles, [] {
    float x = s_vf;
    for (int i = 0; i < kIters; ++i) { PG_REP8(x = tanhRat(x) + 0.5f;) }
    s_vf = x;
  }));
  out->dmadd = (minCycles(cycles, [] {
    double x = s_vd;
    const double a = 0.9999, b = 1e-4;
    for (int i = 0; i < kIters; ++i) { PG_REP8(x = x * a + b;) }
    s_vd = x;
  }));
  out->ddiv = (minCycles(cycles, [] {
    double x = s_vd;
    const double a = 1.0001;
    for (int i = 0; i < kIters; ++i) { PG_REP8(x = a / x;) }
    s_vd = x;
  }));
  out->sinD = (minCycles(cycles, [] {
    double x = s_vd;
    for (int i = 0; i < kIters; ++i) { PG_REP8(x = sin(x) + 0.5;) }
    s_vd = x;
  }));
}

#undef PG_REP8

}  // namespace bench
}  // namespace smoke
