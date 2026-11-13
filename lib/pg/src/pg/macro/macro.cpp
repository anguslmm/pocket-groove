// Macro set evaluation (macro.h). Float only, no heap, no libm.
#include "macro.h"

#include <cstring>

namespace pg {
namespace macro {

namespace {
inline float clamp01(float x) { return x > 0.0f ? (x < 1.0f ? x : 1.0f) : 0.0f; }   // NaN -> 0
inline char lower(char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; }

bool ieq(const char* a, const char* b) {
  while (*a && *b && lower(*a) == lower(*b)) ++a, ++b;
  return *a == 0 && *b == 0;
}

// 2^x for |x| <= 4: integer part by repeated scaling, fraction by its Taylor
// series to the 6th power (relative error below 2e-5, a ten-thousandth of a dB).
float exp2Small(float x) {
  if (x > 4.0f) x = 4.0f;
  if (x < -4.0f) x = -4.0f;
  float scale = 1.0f;
  while (x >= 1.0f) { x -= 1.0f; scale *= 2.0f; }
  while (x < 0.0f) { x += 1.0f; scale *= 0.5f; }
  const float y = x * 0.69314718f;   // 2^x = e^y, 0 <= y < ln 2
  const float e = 1.0f + y * (1.0f + y * (0.5f + y * (1.0f / 6.0f + y * (1.0f / 24.0f + y * (1.0f / 120.0f + y * (1.0f / 720.0f))))));
  return scale * e;
}
}  // namespace

float milliToFloat(int32_t m) {
  const bool neg = m < 0;
  const int32_t a = neg ? -m : m;
  const float v = float(a / 1000) + float(a % 1000) / 1000.0f;
  return neg ? -v : v;
}

namespace {
// Point decoding: exact (the parser's floats, what evaluate() has always used), or
// Fast for the fractional path, which multiplies by 0.001 instead of dividing by
// 1000 (within an ulp; a float division is a dozen instructions on the device).
template <bool Fast>
inline float posOf(Point p) {
  if (!Fast) return pointPos(p);
  const int32_t m = pointPosMilli(p);
  return m >= 1000 ? 1.0f : float(m) * 0.001f;
}
template <bool Fast>
inline float valOf(Point p) {
  return Fast ? float(pointValMilli(p)) * 0.001f : pointVal(p);
}

template <bool Fast>
float curveT(const Point* pts, int n, Interp interp, float x) {
  if (n <= 0) return 0.0f;
  x = clamp01(x);
  if (x >= posOf<Fast>(pts[n - 1])) return valOf<Fast>(pts[n - 1]);
  const float p0 = posOf<Fast>(pts[0]);
  if (x < p0) return valOf<Fast>(pts[0]);
  // the last point at or left of x; pos[i + 1] > x >= pos[i], so the segment has width
  int i = n - 2;
  float pi = posOf<Fast>(pts[i]);
  while (i > 0 && pi > x) pi = posOf<Fast>(pts[--i]);
  const float vi = valOf<Fast>(pts[i]);
  if (interp == Interp::Step) return vi;
  float t = (x - pi) / (posOf<Fast>(pts[i + 1]) - pi);
  if (interp == Interp::Smooth) t = t * t * (3.0f - 2.0f * t);
  return vi + (valOf<Fast>(pts[i + 1]) - vi) * t;
}

template <bool Fast>
uint32_t evalRawT(const MacroSet& set, uint32_t enumMask, const float* positions, const uint8_t* under, float* v);
}  // namespace

float curveAt(const Point* pts, int n, Interp interp, float x) { return curveT<false>(pts, n, interp, x); }

void MacroSet::clear() { std::memset(this, 0, sizeof(*this)); }

uint32_t MacroSet::ownedMask() const {
  uint32_t m = baseMask;
  const int nt = numTargets < kMaxTargets ? numTargets : kMaxTargets;
  for (int j = 0; j < nt; ++j)
    if (targets[j].param < kParamsPerTrack) m |= 1u << targets[j].param;
  const int nmo = numMorphs < kMaxMorphs ? numMorphs : kMaxMorphs;
  for (int k = 0; k < nmo; ++k) {
    if (morphs[k].patch >= numPatches || morphs[k].patch >= kMaxPatches) continue;
    const Patch& p = patches[morphs[k].patch];
    for (int e = 0; e < p.n && p.first + e < kMaxPatchValues; ++e)
      if (patchValues[p.first + e].param < kParamsPerTrack) m |= 1u << patchValues[p.first + e].param;
  }
  if (numTrims) m |= 1u << kMixLevel;
  return m;
}

int MacroSet::findMacro(const char* nm) const {
  for (int i = 0; i < numMacros && i < kMaxMacros; ++i)
    if (ieq(macros[i].name, nm)) return i;
  return -1;
}

int MacroSet::findPatch(const char* nm) const {
  for (int i = 0; i < numPatches && i < kMaxPatches; ++i)
    if (ieq(patches[i].name, nm)) return i;
  return -1;
}

uint32_t evaluate(const MacroSet& set, const ParamTable& params, const float* positions, const uint8_t* under,
                  uint8_t* out) {
  return evaluate(set, params.info[set.track < kNumTracks ? set.track : 0], positions, under, out);
}

uint32_t evaluate(const MacroSet& set, TrackParams info, const float* positions, const uint8_t* under,
                  uint8_t* out) {
  uint32_t enums = 0;
  for (int p = 0; p < kParamsPerTrack; ++p)
    if (info[p] && info[p]->fmt == ParamFormat::Enum) enums |= 1u << p;
  float v[kParamsPerTrack];
  const uint32_t owned = evaluateRaw(set, enums, positions, under, v);
  // 5. clamp and round
  for (int p = 0; p < kParamsPerTrack; ++p) {
    out[p] = under[p];
    if (!((owned >> p) & 1)) continue;
    const ParamInfo* in = info[p];
    float lo = 0.0f, hi = 127.0f;
    if (in) lo = float(in->min), hi = float(in->max);
    float x = v[p];
    if (!(x >= lo)) x = lo;   // also catches NaN
    if (x > hi) x = hi;
    out[p] = uint8_t(int(x + 0.5f));
  }
  return owned;
}

uint32_t evaluateRaw(const MacroSet& set, uint32_t enumMask, const float* positions, const uint8_t* under,
                     float* v) {
  return evalRawT<false>(set, enumMask, positions, under, v);
}

namespace {
template <bool Fast>
uint32_t evalRawT(const MacroSet& set, uint32_t enumMask, const float* positions, const uint8_t* under, float* v) {
  const uint32_t owned = set.ownedMask();
  for (int p = 0; p < kParamsPerTrack; ++p) v[p] = float(((set.baseMask >> p) & 1) ? set.base[p] : under[p]);
  const int nm = set.numMacros < kMaxMacros ? set.numMacros : kMaxMacros;
  float pos[kMaxMacros] = {0.0f, 0.0f, 0.0f, 0.0f};
  for (int m = 0; m < nm; ++m) pos[m] = clamp01(positions[m]);

  // 2. morphs: a mix of the base and the patches
  const int nmo = set.numMorphs < kMaxMorphs ? set.numMorphs : kMaxMorphs;
  if (nmo > 0) {
    float amt[kMaxMorphs];
    float sum = 0.0f;
    for (int k = 0; k < nmo; ++k) {
      const Morph& mo = set.morphs[k];
      const float x = mo.macro < nm ? pos[mo.macro] : 0.0f;
      amt[k] = mo.n ? clamp01(curveT<Fast>(set.points + mo.first, mo.n, Interp(mo.interp), x)) : x;
      sum += amt[k];
    }
    const float norm = sum > 1.0f ? 1.0f / sum : 1.0f;
    float start[kParamsPerTrack];
    std::memcpy(start, v, sizeof(start));
    float best[kParamsPerTrack];   // enum: the winning line's amount so far (-1 = none)
    for (int p = 0; p < kParamsPerTrack; ++p) best[p] = -1.0f;
    for (int k = 0; k < nmo; ++k) {
      const Morph& mo = set.morphs[k];
      if (mo.patch >= set.numPatches || amt[k] <= 0.0f) continue;
      const Patch& pa = set.patches[mo.patch];
      const float w = amt[k] * norm;
      for (int e = 0; e < pa.n; ++e) {
        const PatchValue& pv = set.patchValues[pa.first + e];
        if (pv.param >= kParamsPerTrack) continue;
        if ((enumMask >> pv.param) & 1u) {
          const int sw = pv.sw != kNoSwitch ? pv.sw : mo.sw != kNoSwitch ? mo.sw : kDefaultSwitch;
          if (amt[k] * 100.0f >= float(sw) && amt[k] >= best[pv.param]) {
            best[pv.param] = amt[k];
            v[pv.param] = float(pv.value);
          }
        } else {
          v[pv.param] += w * (float(pv.value) - start[pv.param]);
        }
      }
    }
  }

  // 3. targets, in file order
  const int nt = set.numTargets < kMaxTargets ? set.numTargets : kMaxTargets;
  for (int m = 0; m < nm; ++m) {
    const Macro& mac = set.macros[m];
    for (int j = 0; j < mac.n && mac.first + j < nt; ++j) {
      const Target& t = set.targets[mac.first + j];
      if (t.param >= kParamsPerTrack) continue;
      const float c = curveT<Fast>(set.points + t.first, t.n, t.interp(), pos[m]);
      const uint8_t by = t.by();
      const float s = by < nm ? pos[by] : 1.0f;
      float& x = v[t.param];
      switch (t.op()) {
        case Op::Set: x += (c - x) * s; break;
        case Op::Add: x += c * s; break;
        case Op::Mul: x *= 1.0f + (c - 1.0f) * s; break;
      }
    }
  }

  // 4. trims: dB on the level, through the mixer's cubic taper (gain = (lvl / 100)^3)
  const int ntr = set.numTrims < kMaxTrims ? set.numTrims : kMaxTrims;
  if (ntr > 0) {
    float db = 0.0f;
    for (int k = 0; k < ntr; ++k) {
      const Trim& tr = set.trims[k];
      const float x = tr.macro < nm ? pos[tr.macro] : 0.0f;
      const float s = tr.by < nm ? pos[tr.by] : 1.0f;
      db += curveT<Fast>(set.points + tr.first, tr.n, Interp(tr.interp), x) * s;
    }
    // 10^(db / 60) = 2^(db * log2(10) / 60)
    v[kMixLevel] *= exp2Small(db * (3.32192809f / 60.0f));
  }
  return owned;
}
}  // namespace

uint32_t evaluateFine(const MacroSet& set, const FineRanges& r, const float* positions, const uint8_t* under,
                      float* out) {
  const uint32_t owned = evalRawT<true>(set, r.enumMask, positions, under, out);
  for (int p = 0; p < kParamsPerTrack; ++p) {
    if (!((owned >> p) & 1)) { out[p] = float(under[p]); continue; }
    const float lo = p < kSoundParams ? float(r.lo[p]) : 0.0f, hi = p < kSoundParams ? float(r.hi[p]) : 127.0f;
    float x = out[p];
    if (!(x >= lo)) x = lo;   // also catches NaN
    if (x > hi) x = hi;
    if ((r.stepMask >> p) & 1u) x = float(int(x + 0.5f));   // enums and semitones jump, as in evaluate()
    out[p] = x;
  }
  return owned;
}

int enumJumps(const MacroSet& set, const ParamTable& params, int m, const float* positions, const uint8_t* under,
              float* outPos, uint8_t* outParam, int max) {
  if (m < 0 || m >= set.numMacros || max <= 0) return 0;
  const int track = set.track < kNumTracks ? set.track : 0;
  uint32_t enums = 0;
  for (int p = 0; p < kParamsPerTrack; ++p)
    if (params.info[track][p] && params.info[track][p]->fmt == ParamFormat::Enum) enums |= 1u << p;
  enums &= set.ownedMask();
  if (!enums) return 0;
  float pos[kMaxMacros];
  for (int i = 0; i < kMaxMacros; ++i) pos[i] = i < set.numMacros ? positions[i] : 0.0f;
  uint8_t prev[kParamsPerTrack], cur[kParamsPerTrack];
  int found = 0;
  for (int k = 0; k <= kJumpSteps && found < max; ++k) {
    pos[m] = float(k) / float(kJumpSteps);
    evaluate(set, params, pos, under, cur);
    if (k > 0)
      for (int p = 0; p < kParamsPerTrack && found < max; ++p)
        if (((enums >> p) & 1) && cur[p] != prev[p]) {
          outPos[found] = pos[m];
          outParam[found] = uint8_t(p);
          ++found;
        }
    std::memcpy(prev, cur, sizeof(cur));
  }
  return found;
}

uint16_t setId(int track, const char* name) {
  uint32_t h = 2166136261u;
  auto mix = [&](char c) { h = (h ^ uint8_t(c)) * 16777619u; };
  for (const char* s = trackName(track); *s; ++s) mix(*s);
  mix(':');
  for (const char* s = name; *s; ++s) mix(*s >= 'a' && *s <= 'z' ? char(*s - 'a' + 'A') : *s);
  return uint16_t((h >> 16) ^ (h & 0xFFFFu));
}

uint8_t posToByte(float p) {
  const float x = clamp01(p) * float(kPosMax) + 0.5f;
  return uint8_t(int(x));
}

void combineMixerRows(const uint8_t* player, const uint8_t* inst, const uint8_t* def, uint8_t* out) {
  auto clamp127 = [](int v) { return uint8_t(v < 0 ? 0 : (v > 127 ? 127 : v)); };
  constexpr int L = kMixLevel - kMixLevel, D = kMixDelaySend - kMixLevel, R = kMixReverbSend - kMixLevel;
  const int d = def[L];
  const int lvl = d > 0 ? (int(player[kMixLevel]) * int(inst[L]) + d / 2) / d : player[kMixLevel];
  const uint8_t dly = clamp127(int(player[kMixDelaySend]) + int(inst[D]) - int(def[D]));
  const uint8_t rev = clamp127(int(player[kMixReverbSend]) + int(inst[R]) - int(def[R]));
  out[kMixLevel] = clamp127(lvl);
  out[kMixPan] = player[kMixPan];
  out[kMixDelaySend] = dly;
  out[kMixReverbSend] = rev;
  out[kMixDuck] = player[kMixDuck];
}

void combineMixerFine(const uint8_t* player, const float* inst, const uint8_t* def, float* out) {
  auto clamp127 = [](float v) { return v > 0.0f ? (v < 127.0f ? v : 127.0f) : 0.0f; };
  constexpr int L = 0, D = kMixDelaySend - kMixLevel, R = kMixReverbSend - kMixLevel;
  const float d = float(def[L]);
  out[0] = clamp127(def[L] > 0 ? float(player[L]) * inst[L] / d : float(player[L]));
  out[1] = clamp127(float(player[D]) + inst[D] - float(def[D]));
  out[2] = clamp127(float(player[R]) + inst[R] - float(def[R]));
}

void combineMixer(const uint8_t* player, const uint8_t* inst, const uint8_t* def, uint8_t* out) {
  combineMixerRows(player, inst + kMixLevel, def + kMixLevel, out);
}

}  // namespace macro
}  // namespace pg
