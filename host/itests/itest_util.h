// Shared helpers for host/itests/itest_*.cpp (not a test itself): a checker,
// render statistics over interleaved int16, and a deterministic RNG.
#pragma once
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "pg/engine/engine_hooks.h"
#include "pg/instrument/instrument.h"

namespace it {

inline int g_fail = 0, g_checks = 0;

inline void check(bool ok, const char* fmt, ...) {
  ++g_checks;
  if (ok) return;
  ++g_fail;
  if (g_fail > 40) return;
  std::fprintf(stderr, "FAIL: ");
  va_list ap;
  va_start(ap, fmt);
  std::vfprintf(stderr, fmt, ap);
  va_end(ap);
  std::fprintf(stderr, "\n");
}

inline int finish(const char* name) {
  std::printf("%s: %d checks, %d failed\n", name, g_checks, g_fail);
  return g_fail ? 1 : 0;
}

struct Rng {
  uint32_t s;
  explicit Rng(uint32_t seed) : s(seed ? seed : 1u) {}
  uint32_t next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
  int range(int lo, int hi) { return lo + int(next() % uint32_t(hi - lo + 1)); }
  uint8_t byte() { return uint8_t(next() >> 24); }
};

// Running statistics of the int16 output.
struct Stats {
  long frames = 0;
  int peak = 0;
  int maxJump = 0;           // largest sample-to-sample step (an int16 wrap would be ~65535)
  long lrDiffer = 0;         // frames where L != R
  long fullScale = 0;        // samples at +-32767/-32768
  long minInt = 0;           // samples at -32768: the master never emits it (ceiling is +-32767), so
                             // one means a wrapped / unsaturated float -> int16 conversion
  double sumSq = 0.0;
  uint64_t hash = 1469598103934665603ull;   // FNV-1a over the bytes
  int16_t lastL = 0, lastR = 0;

  void add(const int16_t* buf, int n) {
    for (int i = 0; i < n; ++i) {
      const int16_t l = buf[2 * i], r = buf[2 * i + 1];
      const int al = std::abs(int(l)), ar = std::abs(int(r));
      if (al > peak) peak = al;
      if (ar > peak) peak = ar;
      if (al >= 32767) ++fullScale;
      if (ar >= 32767) ++fullScale;
      if (l == -32768 || r == -32768) ++minInt;
      const int jl = std::abs(int(l) - int(lastL)), jr = std::abs(int(r) - int(lastR));
      if (jl > maxJump) maxJump = jl;
      if (jr > maxJump) maxJump = jr;
      lastL = l; lastR = r;
      if (l != r) ++lrDiffer;
      sumSq += double(l) * double(l) + double(r) * double(r);
      const uint8_t* b = reinterpret_cast<const uint8_t*>(buf + 2 * i);
      for (int k = 0; k < 4; ++k) { hash ^= b[k]; hash *= 1099511628211ull; }
    }
    frames += n;
  }
  double rmsDb() const {
    return frames ? 10.0 * std::log10(sumSq / (2.0 * double(frames)) / (32767.0 * 32767.0) + 1e-30) : -300.0;
  }
  double peakDb() const { return 20.0 * std::log10(double(peak > 0 ? peak : 1) / 32767.0); }
};

inline void post(pg::Instrument& inst, pg::CmdType t, int a = 0, int b = 0, int c = 0) {
  pg::Command k{};
  k.type = t;
  k.a = uint8_t(a); k.b = uint8_t(b); k.c = uint8_t(c);
  k.step = pg::kDefaultStep;
  inst.engine().post(k);
}

// Render `frames` (multiple of kQuantum) in 256-frame blocks into stats (and
// optionally into `out`, interleaved).
inline void render(pg::Instrument& inst, long frames, Stats& st, int16_t* out = nullptr) {
  static int16_t buf[2 * 256];
  long done = 0;
  while (done < frames) {
    const int n = int(frames - done < 256 ? frames - done : 256);
    inst.process(buf, n);
    st.add(buf, n);
    if (out) std::memcpy(out + 2 * done, buf, sizeof(int16_t) * 2 * size_t(n));
    done += n;
  }
}

}  // namespace it
