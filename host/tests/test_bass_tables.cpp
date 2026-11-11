// The bass sine and BLEP tables are constant data generated ahead of time
// (bass_tables_data.cpp, by tools/gen/gen_tables.cpp). This checks the
// checked-in data against the reference formula (bass_tables_ref.cpp). If it
// fails after a formula change, regenerate (see bass_math.h).
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "pg/bass/bass_math.h"

using namespace pg::bass;

static int gFail = 0;

static long ulps(float a, float b) {
  if (a == b) return 0;   // also +0 == -0
  int32_t ia, ib;
  std::memcpy(&ia, &a, 4);
  std::memcpy(&ib, &b, 4);
  if ((ia < 0) != (ib < 0)) return 1L << 30;
  return std::labs(long(ia) - long(ib));
}

static void check(const char* name, const float* gen, const float* ref, int n) {
  int diffs = 0;
  long worst = 0;
  for (int i = 0; i < n; ++i) {
    const long u = ulps(gen[i], ref[i]);
    if (u) ++diffs;
    if (u > worst) worst = u;
  }
  // Bit-identical with the libm the data was generated with; another libm's
  // sinf may differ by an ulp (and the BLEP integrates many of them).
  const long allowed = std::strcmp(name, "gSine") == 0 ? 1 : 64;
  if (worst > allowed) {
    std::printf("  FAIL %s: up to %ld ulp from the formula (regenerate the tables)\n", name, worst);
    ++gFail;
  }
  std::printf("  %-6s %4d entries, %d differ from the formula (max %ld ulp)\n", name, n, diffs, worst);
}

int main() {
  std::printf("bass tables: generated data vs reference formula\n");
  static float sine[kSineSize + 1], blep[(kBlepPhases + 2) * kBlepTaps];
  buildTablesReference(sine, blep);
  check("gSine", gSine, sine, kSineSize + 1);
  check("gBlep", gBlep, blep, (kBlepPhases + 2) * kBlepTaps);
  std::printf("test_bass_tables: %s (%d failure%s)\n", gFail ? "FAILED" : "ok", gFail, gFail == 1 ? "" : "s");
  return gFail ? 1 : 0;
}
