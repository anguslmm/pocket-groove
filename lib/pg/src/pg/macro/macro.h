// Macro sets (docs/MACROLAB.md): a few designed controls per track that drive
// that track's real parameters through breakpoint curves. Portable, heap-free,
// float only: the same tables and text format can run on the device later.
//
// The model, in full:
//   * A MacroSet belongs to one track. It has a name, an optional base patch
//     (fixed values for any of the track's 32 parameters) and up to kMaxMacros
//     macros, each with a short name and a position 0..1.
//   * Each macro drives up to kMaxTargets targets. A target is one parameter,
//     an operation (set / add / mul), a curve from the macro's position to a
//     number (up to kMaxPoints points; linear, smooth or step between them;
//     flat beyond the first and last point; any shape, not only rising) and,
//     optionally, another macro whose position scales the target's effect.
//   * Evaluation, per owned parameter: start from the base patch value if the
//     set has one, else from the value the parameter had underneath; then
//     apply every target on that parameter in file order:
//         set: v = c      add: v = v + c      mul: v = v * c
//     where c is the curve at the macro's position. With "by M" the effect is
//     scaled by M's position s: set v += (c - v) s, add v += c s,
//     mul v *= 1 + (c - 1) s. Finally clamp to the ParamInfo range and round.
//   * A set owns every parameter in its base patch or named by a target; the
//     others are left as they are.
#pragma once
#include <cstddef>
#include <cstdint>

#include "../params.h"

namespace pg {
namespace macro {

constexpr int kMaxMacros = 4;       // per set
constexpr int kMaxTargets = 8;      // per macro
constexpr int kMaxPoints = 8;       // per curve
constexpr int kSetNameLen = 12;     // characters
constexpr int kMacroNameLen = 6;    // characters (fits the 240x135 screen style)
constexpr int kMaxLineLen = 200;    // characters per line of a .pgm file
constexpr int kMaxDecimals = 3;     // digits after the point in a .pgm number
constexpr uint8_t kNoMacro = 0xFF;

enum class Op : uint8_t { Set = 0, Add, Mul };
enum class Interp : uint8_t { Linear = 0, Smooth, Step };

struct Curve {
  uint8_t n;                 // 1..kMaxPoints
  Interp interp;
  float pos[kMaxPoints];     // 0..1, non-decreasing (two equal positions make a jump)
  float val[kMaxPoints];
  // The curve at x (clamped to 0..1): flat before the first and after the last point.
  float at(float x) const;
};

struct Target {
  uint8_t param;             // 0..kParamsPerTrack-1 (24..28 = the mixer rows)
  Op op;
  uint8_t by;                // index of the scaling macro, or kNoMacro
  Curve curve;
};

struct Macro {
  char name[kMacroNameLen + 1];
  float start;               // position when the set is first selected
  uint8_t numTargets;
  Target targets[kMaxTargets];
};

struct MacroSet {
  uint8_t track;             // TrackId
  uint8_t numMacros;
  char name[kSetNameLen + 1];   // may be empty: the lab names it after the file
  uint32_t baseMask;         // bit i: base[i] is given
  uint8_t base[kParamsPerTrack];
  Macro macros[kMaxMacros];

  void clear();
  uint32_t ownedMask() const;   // base patch | every target's parameter
  int findMacro(const char* name) const;   // case-insensitive, -1 if none
};

// The parameter metadata (Engine::trackParamInfo for every track and index;
// nullptr = no such parameter). Filled by the caller, so this module depends
// on neither the engine nor the voices.
struct ParamTable {
  const ParamInfo* info[kNumTracks][kParamsPerTrack];
};

// out[0..31] = the track's parameters with the set applied at `positions`
// (set.numMacros entries, each clamped to 0..1). under[0..31] = the values
// underneath (what the track had before the set); parameters the set does not
// own are copied from it. Returns set.ownedMask().
uint32_t evaluate(const MacroSet& set, const ParamTable& params, const float* positions, const uint8_t* under,
                  uint8_t* out);

// ---- the text format (macro_parse.cpp) --------------------------------------------------
struct ParseError {
  int line;                  // 1-based line of the problem (0: the file as a whole)
  char msg[100];
};

// Parses one .pgm file (text, len bytes; need not be NUL-terminated). On
// success fills *out and returns true. On failure returns false with *err
// set; *out is then unspecified (keep the last good set elsewhere). Never
// allocates; stack use is about one line plus a few hundred bytes.
bool parse(const char* text, size_t len, const ParamTable& params, MacroSet* out, ParseError* err);

// Writes `set` in the canonical form of the format (parse(format(s)) == s).
// Returns the length written (NUL-terminated), or 0 if cap is too small.
size_t format(const MacroSet& set, const ParamTable& params, char* buf, size_t cap);

// Lookups used by the parser, the lab and the tests.
int trackFromName(const char* name);           // "bass", "chh", ... -> TrackId, or -1
const char* trackName(int track);              // "kick" ... "lead"
// A parameter of `track` by its short or long name, case-insensitive, ignoring
// spaces and punctuation ("cutoff", "CUT", "Env Mod" = "envmod" = "env_mod"), or
// else by a unique start of its long name of at least 3 letters ("reso").
// -1 if none, kAmbiguous if that start fits more than one.
constexpr int kAmbiguous = -2;
int findParam(const ParamTable& params, int track, const char* name);

}  // namespace macro
}  // namespace pg
