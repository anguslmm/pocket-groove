// The lead wavetables are constant data generated ahead of time
// (lead_tables_data.cpp, by tools/gen/gen_tables.cpp). This checks that the
// checked-in data still equals the reference formula (lead_tables_ref.cpp)
// for both table sets, entry for entry, and that tablesFor() picks the right
// set. If it fails after a formula change: regenerate (see lead_tables.h).
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "pg/lead/lead_tables.h"
#include "test_lead_util.h"

using namespace pg::lead;

// Distance in ulps between two finite floats of the same sign.
static long ulps(float a, float b) {
  int32_t ia, ib;
  std::memcpy(&ia, &a, 4);
  std::memcpy(&ib, &b, 4);
  return std::labs(long(ia) - long(ib));
}

static void compareTab(const char* what, int i, const Tab& gen, const Tab& ref, int& lsbDiffs, int& maxLsb) {
  LT_CHECK(gen.shift == ref.shift && gen.mask == ref.mask && gen.fscale == ref.fscale, "%s[%d]: geometry differs", what, i);
  if (gen.shift != ref.shift) return;
  const int n = (1 << (32 - ref.shift)) + 1;
  for (int k = 0; k < n; ++k) {
    const int d = std::abs(int(gen.d[k]) - int(ref.d[k]));
    if (d) ++lsbDiffs;
    if (d > maxLsb) maxLsb = d;
  }
}

int main() {
  std::printf("lead tables: generated data vs reference formula\n");
  for (float fs : {44100.0f, 32000.0f}) {
    std::vector<int16_t> pool(kRefPoolSize);
    Tables ref;
    buildTablesReference(fs, pool.data(), ref);
    const Tables& gen = tablesFor(fs);
    LT_CHECK(gen.sampleRate == fs, "tablesFor(%.0f) returned the %.0f Hz set", double(fs), double(gen.sampleRate));
    int diffs = 0, maxLsb = 0;
    for (int b = 0; b < kNumBands; ++b) compareTab("saw", b, gen.saw[b], ref.saw[b], diffs, maxLsb);
    for (int b = 0; b < kNumBands; ++b) compareTab("tri", b, gen.tri[b], ref.tri[b], diffs, maxLsb);
    for (int r = 0; r < kOrgRegs; ++r)
      for (int l = 0; l < kOrgLevels; ++l) compareTab("organ", r * kOrgLevels + l, gen.organ[r][l], ref.organ[r][l], diffs, maxLsb);
    compareTab("sine", 0, gen.sine, ref.sine, diffs, maxLsb);
    // Bit-identical with the libm the data was generated with; a different
    // libm may round a sum the other way: allow 1 LSB / 1 ulp, never more.
    LT_CHECK(maxLsb <= 1, "fs %.0f: table entries differ by up to %d LSB from the formula (regenerate)", double(fs), maxLsb);
    const long u1 = ulps(gen.organPitch1, ref.organPitch1), u2 = ulps(gen.organPitch2, ref.organPitch2);
    LT_CHECK(u1 <= 1 && u2 <= 1, "fs %.0f: organ thresholds differ by %ld / %ld ulp", double(fs), u1, u2);
    std::printf("  %5.0f Hz set: %d entries differ from the formula (max %d LSB), organ thresholds %ld/%ld ulp\n",
                double(fs), diffs, maxLsb, u1, u2);
  }
  // Rate selection: never a set band-limited for a higher rate than the engine's.
  LT_CHECK(&tablesFor(48000.0f) == &kTables44k && &tablesFor(44100.0f) == &kTables44k, "44.1 kHz set selection");
  LT_CHECK(&tablesFor(32000.0f) == &kTables32k && &tablesFor(44099.0f) == &kTables32k, "32 kHz set selection");
  return lt::finish("test_lead_tables");
}
