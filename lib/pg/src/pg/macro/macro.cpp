// Macro set evaluation (macro.h). Float only, no heap, no libm.
#include "macro.h"

#include <cstring>

namespace pg {
namespace macro {

namespace {
inline float clamp01(float x) { return x > 0.0f ? (x < 1.0f ? x : 1.0f) : 0.0f; }   // NaN -> 0
inline char lower(char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; }
}  // namespace

float Curve::at(float x) const {
  if (n == 0) return 0.0f;
  x = clamp01(x);
  if (x >= pos[n - 1]) return val[n - 1];
  if (x < pos[0]) return val[0];
  // the last point at or left of x; pos[i + 1] > x >= pos[i], so the segment has width
  int i = n - 2;
  while (i > 0 && pos[i] > x) --i;
  if (interp == Interp::Step) return val[i];
  float t = (x - pos[i]) / (pos[i + 1] - pos[i]);
  if (interp == Interp::Smooth) t = t * t * (3.0f - 2.0f * t);
  return val[i] + (val[i + 1] - val[i]) * t;
}

void MacroSet::clear() { std::memset(this, 0, sizeof(*this)); }

uint32_t MacroSet::ownedMask() const {
  uint32_t m = baseMask;
  for (int i = 0; i < numMacros && i < kMaxMacros; ++i)
    for (int j = 0; j < macros[i].numTargets && j < kMaxTargets; ++j)
      if (macros[i].targets[j].param < kParamsPerTrack) m |= 1u << macros[i].targets[j].param;
  return m;
}

int MacroSet::findMacro(const char* nm) const {
  for (int i = 0; i < numMacros && i < kMaxMacros; ++i) {
    const char* a = macros[i].name;
    const char* b = nm;
    while (*a && *b && lower(*a) == lower(*b)) ++a, ++b;
    if (*a == 0 && *b == 0) return i;
  }
  return -1;
}

uint32_t evaluate(const MacroSet& set, const ParamTable& params, const float* positions, const uint8_t* under,
                  uint8_t* out) {
  const uint32_t owned = set.ownedMask();
  const int track = set.track < kNumTracks ? set.track : 0;
  float v[kParamsPerTrack];
  for (int p = 0; p < kParamsPerTrack; ++p) {
    out[p] = under[p];
    v[p] = float(((set.baseMask >> p) & 1) ? set.base[p] : under[p]);
  }
  const int nm = set.numMacros < kMaxMacros ? set.numMacros : kMaxMacros;
  float pos[kMaxMacros] = {0.0f, 0.0f, 0.0f, 0.0f};
  for (int m = 0; m < nm; ++m) pos[m] = clamp01(positions[m]);
  for (int m = 0; m < nm; ++m) {
    const Macro& mac = set.macros[m];
    for (int j = 0; j < mac.numTargets && j < kMaxTargets; ++j) {
      const Target& t = mac.targets[j];
      if (t.param >= kParamsPerTrack) continue;
      const float c = t.curve.at(pos[m]);
      const float s = t.by < nm ? pos[t.by] : 1.0f;
      float& x = v[t.param];
      switch (t.op) {
        case Op::Set: x += (c - x) * s; break;
        case Op::Add: x += c * s; break;
        case Op::Mul: x *= 1.0f + (c - 1.0f) * s; break;
      }
    }
  }
  for (int p = 0; p < kParamsPerTrack; ++p) {
    if (!((owned >> p) & 1)) continue;
    const ParamInfo* info = params.info[track][p];
    float lo = 0.0f, hi = 127.0f;
    if (info) lo = float(info->min), hi = float(info->max);
    float x = v[p];
    if (!(x >= lo)) x = lo;   // also catches NaN
    if (x > hi) x = hi;
    out[p] = uint8_t(int(x + 0.5f));
  }
  return owned;
}

}  // namespace macro
}  // namespace pg
