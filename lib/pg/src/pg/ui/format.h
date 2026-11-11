// Value display rules (docs/UX.md "Value display rules"), note names, scales
// and the lead chord rules (section 9.3). Pure functions, no state.
#pragma once
#include <cstdint>
#include "../params.h"

namespace pg {
namespace ui {

// What a value belongs to, for the special display cases (swing, DJ filter, pan "C").
struct ValueRef {
  bool global;     // true: index is a GlobalParam
  uint8_t track;   // track params only
  uint8_t index;   // track param 0..31 or GlobalParam
};

enum class BarKind : uint8_t { Segments, Bipolar, Unipolar };

constexpr int kValueTextCap = 8;   // max 5 visible chars + NUL, with slack

// Writes the display text of raw value v into out (>= kValueTextCap bytes).
void formatValue(const ParamInfo* info, const ValueRef& ref, int v, char* out);
BarKind barKind(const ParamInfo* info, const ValueRef& ref);
float valueFrac(const ParamInfo* info, int v);     // (v-min)/(max-min), 0 for a 1-value range
bool isSmallRange(const ParamInfo* info);          // enum or <= 16 values: no acceleration

// "A1", "C#3"
void noteName(int note, char* out);
// Upper-case copy, truncated to cap-1 chars.
void upperCopy(const char* s, char* out, int cap);
// Integer to decimal text; returns chars written.
int itoa10(int v, char* out);

// ---- scales ---------------------------------------------------------------
int scaleCount();                                    // number of scale types
int scaleSemitone(int scale, int degree);            // semitone offset of a scale degree
bool scaleHasSemitone(int scale, int semi);          // semi taken mod 12
// MIDI note of degree k at `octave` (UX 4.4): 12*(octave+1) + root + offset, clamped 0..127.
int degreeNote(int root, int scale, int octave, int degree);

// ---- chords ---------------------------------------------------------------
enum ChordMode : uint8_t {
  kModeNone = 0, kModeAuto, kModeAuto7,
  // the fixed types follow in ChordType order: OCT 5TH MAJ MIN MAJ7 MIN7 DOM7 SUS4 MIN9
  kModeFixedFirst, kNumChordModes = kModeFixedFirst + (kChordNum - 1)
};
const char* chordModeName(int mode);                 // "AUTO", "MIN7", "NONE"
const char* chordName(int chord);                    // "-", "OCT", .. (STEP EDIT field)
const char* chordShort(int chord);                   // "", "8", "5", "M", "m", ... (step pads)
// ChordType a note key produces on the lead (9.3).
uint8_t chordFor(int mode, int scale, int degree);

}  // namespace ui
}  // namespace pg
