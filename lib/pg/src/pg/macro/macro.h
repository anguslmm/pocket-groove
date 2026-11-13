// Macro sets (docs/MACROLAB.md): a few designed controls per track that drive
// that track's real parameters through breakpoint curves. A set is an
// instrument: a character (base patch, named patches to morph toward) plus up
// to four controls. Portable, heap-free, float only: the same tables and text
// format are meant to run on the device later, eight sets loaded at once.
//
// The model (v2), in full:
//   * A MacroSet belongs to one track. It has a name, an optional "about"
//     line, an optional base patch (fixed values for any of the track's 32
//     parameters), up to kMaxPatches named patches (lists of parameter=value
//     pairs) and up to kMaxMacros macros, each with a short name, optional end
//     labels and description, and a position 0..1.
//   * A macro has targets, morph lines and trim lines. A target is one
//     parameter, an operation (set / add / mul), a curve from the macro's
//     position to a number (linear, smooth or step between points; flat beyond
//     the first and last point; any shape) and, optionally, another macro whose
//     position scales the target's effect. A morph line blends from the base
//     toward a named patch by an amount (the macro's position, or a curve of
//     it). A trim line is a curve in dB on the track level.
//   * Evaluation, per owned parameter:
//       1. start: the base patch value if the set has one, else the value the
//          parameter had underneath (start[p]);
//       2. morphs (all morph lines together, wherever they are in the file):
//          each line k has an amount a_k in 0..1 and a patch P_k. If the
//          amounts add up to more than 1 they are scaled down to add up to 1
//          (w_k = a_k / max(1, sum a)). A continuous parameter becomes
//              start[p] + sum over the lines whose patch lists p of w_k (P_k[p] - start[p])
//          i.e. a mix of the base and the patches, the base weighted by what
//          is left. An enum parameter takes the value of the patch whose line
//          has the largest a_k among those past their switch point (a_k >= sw;
//          sw = the value's own "@sw", else the line's "switch", else 0.5;
//          a later line wins a tie), else stays at start[p];
//       3. every target in file order:
//              set: v = c      add: v = v + c      mul: v = v * c
//          where c is the curve at the macro's position. With "by M" the effect
//          is scaled by M's position s: set v += (c - v) s, add v += c s,
//          mul v *= 1 + (c - 1) s;
//       4. trims: the level is multiplied by 10^(dB / 60) (the mixer's level
//          taper is cubic, so that is dB on the sound), dB = the sum of every
//          trim line's curve (times its "by" macro's position);
//       5. clamp to the ParamInfo range and round.
//   * A set owns every parameter in its base patch, named by a target, listed
//     in a patch that a morph line uses, and the level if it has trims; the
//     others are left as they are.
//   * A v1 file (no patch, morph or trim lines) evaluates exactly as model v1
//     did (host/itests/itest_macro_pin.cpp pins that).
//
// Storage: one set is a single flat table with pooled storage (one pool of
// targets, one of curve points, one of patch values, shared by its macros),
// about 1.9 KB. Curve points are packed in 32 bits: the position in
// thousandths (10 bits) and the value in thousandths (22 bits, signed). The
// format's numbers have at most 3 decimals, so every file value is stored
// exactly, and decoding reproduces the float the v1 parser produced.
#pragma once
#include <cstddef>
#include <cstdint>

#include "../params.h"

namespace pg {
namespace macro {

constexpr int kMaxMacros = 4;          // per set
constexpr int kMaxMacroTargets = 32;   // targets per macro
constexpr int kMaxTargets = 64;        // targets per set (the pool its macros share)
constexpr int kMaxPoints = 16;         // points per curve
constexpr int kMaxSetPoints = 192;     // curve points per set (the pool every curve shares)
constexpr int kMaxPatches = 4;         // named patches per set
constexpr int kMaxPatchValues = 96;    // parameter=value pairs per set, all patches together
constexpr int kMaxMorphs = 8;          // morph lines per set
constexpr int kMaxTrims = 8;           // trim lines per set
constexpr int kSetNameLen = 12;        // characters
constexpr int kMacroNameLen = 6;       // characters (fits the 240x135 screen style)
constexpr int kPatchNameLen = 8;       // characters
constexpr int kLabelLen = 8;           // characters per end label ("DARK", "BRIGHT")
constexpr int kDescLen = 40;           // characters of a macro's description
constexpr int kAboutLen = 72;          // characters of the set's about line (longer is cut)
constexpr int kMaxLineLen = 200;       // characters per line of a .pgm file
constexpr int kMaxDecimals = 3;        // digits after the point in a .pgm number
constexpr int kTrimMaxDb = 24;         // a trim point is -24..+24 dB
constexpr int kMaxWarnings = 8;        // warnings kept per parse (all are counted)
constexpr uint8_t kNoMacro = 0xFF;
constexpr uint8_t kNoSwitch = 0xFF;    // Morph::sw / PatchValue::sw: not given
constexpr int kDefaultSwitch = 50;     // hundredths: an enum switches at amount 0.5

enum class Op : uint8_t { Set = 0, Add, Mul };
enum class Interp : uint8_t { Linear = 0, Smooth, Step };

// A curve point: position 0..1000 (thousandths) in the low 10 bits, value in
// thousandths (-2097151..2097151) in the high 22 bits, two's complement.
using Point = uint32_t;
constexpr int32_t kPointValMax = (1 << 21) - 1;
inline Point packPoint(int32_t posMilli, int32_t valMilli) {
  return uint32_t(posMilli & 1023) | (uint32_t(valMilli) << 10);
}
inline int32_t pointPosMilli(Point p) { return int32_t(p & 1023u); }
inline int32_t pointValMilli(Point p) { return int32_t(p) >> 10; }   // arithmetic shift
// Thousandths to float exactly as the parser reads the same number from text
// (integer part + fraction / 10^decimals: a correctly rounded quotient, so the
// result does not depend on how many decimals were written).
float milliToFloat(int32_t milli);
// (a position is 0..1000: below 1000 the integer part is 0 and the fraction alone is the value)
inline float pointPos(Point p) {
  const int32_t m = pointPosMilli(p);
  return m >= 1000 ? 1.0f : float(m) / 1000.0f;
}
inline float pointVal(Point p) { return milliToFloat(pointValMilli(p)); }

// The curve through n packed points at x (clamped to 0..1): flat before the
// first and after the last point; two points at one position make a jump.
float curveAt(const Point* pts, int n, Interp interp, float x);

struct Target {
  uint8_t param;   // 0..kParamsPerTrack-1 (24..28 = the mixer rows)
  uint8_t bits;    // op (bits 0-1) | interp (2-3) | by macro (4-6, 7 = none) | override (7)
  uint8_t first;   // first point in MacroSet::points
  uint8_t n;       // 1..kMaxPoints
  Op op() const { return Op(bits & 3u); }
  Interp interp() const { return Interp((bits >> 2) & 3u); }
  uint8_t by() const { return ((bits >> 4) & 7u) == 7u ? kNoMacro : uint8_t((bits >> 4) & 7u); }
  bool isOverride() const { return (bits >> 7) != 0; }
  void setBits(Op o, Interp ip, uint8_t byMacro, bool over) {
    bits = uint8_t(unsigned(o) | (unsigned(ip) << 2) | ((byMacro == kNoMacro ? 7u : (byMacro & 7u)) << 4) |
                   (over ? 0x80u : 0u));
  }
};

struct Morph {
  uint8_t macro;    // the macro whose position drives it
  uint8_t patch;    // index into MacroSet::patches
  uint8_t sw;       // switch point for enum values, hundredths (1..100), or kNoSwitch
  uint8_t interp;   // Interp of the amount curve
  uint8_t first, n; // amount curve (values 0..1); n = 0: the amount is the position itself
};

struct Trim {
  uint8_t macro;
  uint8_t by;       // scaling macro, or kNoMacro
  uint8_t interp;   // Interp
  uint8_t first, n; // curve in dB
};

struct PatchValue {
  uint8_t param, value;
  uint8_t sw;       // enum switch point override, hundredths, or kNoSwitch
};

struct Patch {
  char name[kPatchNameLen + 1];
  uint8_t first, n;   // range in MacroSet::patchValues
};

struct Macro {
  char name[kMacroNameLen + 1];
  char lo[kLabelLen + 1], hi[kLabelLen + 1];   // end labels, "" = none
  char desc[kDescLen + 1];                      // "" = none
  uint16_t start1000;   // position when the set is first selected, thousandths
  uint8_t first, n;     // range in MacroSet::targets (contiguous, file order)
  float start() const { return milliToFloat(start1000); }
};

struct MacroSet {
  uint8_t track;   // TrackId
  uint8_t numMacros, numTargets, numPoints, numPatches, numPatchValues, numMorphs, numTrims;
  uint16_t id;                  // setId(track, name): how a project names this instrument; 0 = no name yet
  char name[kSetNameLen + 1];   // may be empty: the lab names it after the file
  char about[kAboutLen + 1];    // the file's first comment line (minus "NAME -- "), may be empty
  uint32_t baseMask;            // bit i: base[i] is given
  uint8_t base[kParamsPerTrack];
  Macro macros[kMaxMacros];
  Patch patches[kMaxPatches];
  Morph morphs[kMaxMorphs];
  Trim trims[kMaxTrims];
  Target targets[kMaxTargets];
  PatchValue patchValues[kMaxPatchValues];
  Point points[kMaxSetPoints];

  void clear();
  uint32_t ownedMask() const;   // base | targets | patches used by morphs | the level if trimmed
  int findMacro(const char* name) const;   // case-insensitive, -1 if none
  int findPatch(const char* name) const;   // case-insensitive, -1 if none
  const Target& target(int macro, int j) const { return targets[macros[macro].first + j]; }
  float curve(const Target& t, float x) const { return curveAt(points + t.first, t.n, t.interp(), x); }
};

// The parameter metadata (Engine::trackParamInfo for every track and index;
// nullptr = no such parameter). Filled by the caller, so this module depends
// on neither the engine nor the voices.
struct ParamTable {
  const ParamInfo* info[kNumTracks][kParamsPerTrack];
};

// One track's parameter metadata (ParamTable::info[track]), for callers that
// have no whole table (the engine builds it on the stack).
using TrackParams = const ParamInfo* const*;   // kParamsPerTrack entries

// out[0..31] = the track's parameters with the set applied at `positions`
// (set.numMacros entries, each clamped to 0..1). under[0..31] = the values
// underneath (what the track had before the set); parameters the set does not
// own are copied from it. Returns set.ownedMask().
uint32_t evaluate(const MacroSet& set, const ParamTable& params, const float* positions, const uint8_t* under,
                  uint8_t* out);
// The same with only the set's track's metadata (info[0..31] = ParamTable::info[set.track]).
uint32_t evaluate(const MacroSet& set, TrackParams info, const float* positions, const uint8_t* under,
                  uint8_t* out);

// ---- instruments (docs/MACROLAB.md "Instruments") ------------------------------------------
// A track slot holds one macro set, its instrument. A project names it by a
// 16-bit id derived from the voice and the set's name (FNV-1a of "voice:NAME",
// upper case, folded to 16 bits); 0 means "no instrument" (CLASSIC: the
// track's stored parameters play as they are). Macro positions are stored as
// 0..255 (kPosMax), position = byte / 255.
constexpr uint16_t kNoInstrument = 0;
constexpr int kPosMax = 255;
uint16_t setId(int track, const char* name);
inline float posFromByte(uint8_t b) { return float(b) * (1.0f / float(kPosMax)); }
uint8_t posToByte(float p);                     // clamped, rounded
inline uint8_t startByte(const Macro& m) { return uint8_t((int(m.start1000) * kPosMax + 500) / 1000); }

// The mixer rule: level, pan, sends and duck (24..28) are the player's (MIX
// view). An instrument may move the level and the two sends; they combine
// with the player's values relative to the parameter's default:
//   level = player * inst / default      (the instrument's level is a gain trim:
//                                         through the cubic taper, its dB add to the player's)
//   send  = player + (inst - default)    (the instrument adds to the player's send)
// each rounded and clamped to 0..127. Pan and duck are the player's only. With
// the player at the defaults the result is exactly what the instrument
// evaluated to. out may be player. Only indices 24..28 are written.
void combineMixer(const uint8_t* player, const uint8_t* inst, const uint8_t* def, uint8_t* out);

// The instrument library: the built-in sets (generated from macros/ into
// builtin_data.cpp by host/tools/macro_builtin, in the order macros/builtin.txt
// gives), plus whatever a host lays over them (the desktop lab: a .pgm file of
// the same voice and name replaces the built-in one, other files are extra
// instruments). The device has no overlay. Every function here is safe to
// call from the audio thread as long as the overlay's are.
class LibraryOverlay {
 public:
  virtual const MacroSet* find(uint16_t id) const = 0;      // a replacement or an extra, else nullptr
  virtual int extraCount(int track) const = 0;              // extras listed after the built-ins
  virtual const MacroSet* extra(int track, int i) const = 0;
 protected:
  ~LibraryOverlay() = default;
};
void setLibraryOverlay(const LibraryOverlay* overlay);       // nullptr = built-ins only
int builtinCount();                                          // every voice
const MacroSet& builtinAt(int i);                            // track order, then the list order
const MacroSet* findBuiltin(uint16_t id);
const MacroSet* findInstrument(uint16_t id);                 // the overlay first; nullptr if unknown (or 0)
int instrumentCount(int track);                              // built-ins, then the overlay's extras
const MacroSet* instrumentAt(int track, int i);              // with the overlay's replacements applied
int instrumentIndex(int track, uint16_t id);                 // its place in that list, -1 if none

// Where moving macro m (the others at `positions`) makes an enum parameter
// change value: sampled at kJumpSteps + 1 positions; outPos[k] is the first
// sampled position with the new value, outParam[k] the parameter. Returns the
// number found (at most max). For the lab's slider ticks and the audit.
constexpr int kJumpSteps = 200;
int enumJumps(const MacroSet& set, const ParamTable& params, int m, const float* positions, const uint8_t* under,
              float* outPos, uint8_t* outParam, int max);

// ---- the text format (macro_parse.cpp) --------------------------------------------------
struct ParseError {
  int line;                  // 1-based line of the problem (0: the file as a whole)
  char msg[100];
};

// Problems that do not stop the load (a plain `set` that silently replaces an
// earlier line's work on the same parameter, a patch no morph uses).
struct Warnings {
  int count;                 // all found; the first kMaxWarnings are kept
  ParseError w[kMaxWarnings];
};

// Parses one .pgm file (text, len bytes; need not be NUL-terminated). On
// success fills *out (and *warn, if given) and returns true. On failure
// returns false with *err set; *out is then unspecified (keep the last good set
// elsewhere). Never allocates; stack use is about 1.5 KB.
bool parse(const char* text, size_t len, const ParamTable& params, MacroSet* out, ParseError* err,
           Warnings* warn = nullptr);

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
