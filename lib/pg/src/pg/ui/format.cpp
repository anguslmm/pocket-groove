#include "format.h"

#include "../engine/scales.h"   // header-only constexpr table, documented as UI-includable

namespace pg {
namespace ui {

namespace {
const char* const kNoteNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
const char* const kChordNames[kChordNum] = {"-", "OCT", "5TH", "MAJ", "MIN", "MAJ7", "MIN7", "DOM7", "SUS4", "MIN9"};
const char* const kChordShort[kChordNum] = {"", "8", "5", "M", "m", "M7", "m7", "7", "s4", "m9"};

char* put(char* o, const char* s) {
  while (*s) *o++ = *s++;
  *o = 0;
  return o;
}
char* putSigned(char* o, int d) {
  if (d == 0) { *o++ = '0'; *o = 0; return o; }
  *o++ = d > 0 ? '+' : '-';
  return o + itoa10(d > 0 ? d : -d, o);
}
}  // namespace

int itoa10(int v, char* out) {
  char tmp[12];
  int n = 0;
  bool neg = v < 0;
  unsigned u = neg ? unsigned(-v) : unsigned(v);
  do { tmp[n++] = char('0' + u % 10); u /= 10; } while (u && n < 11);
  int k = 0;
  if (neg) out[k++] = '-';
  while (n) out[k++] = tmp[--n];
  out[k] = 0;
  return k;
}

void upperCopy(const char* s, char* out, int cap) {
  int i = 0;
  for (; s && s[i] && i < cap - 1; ++i) {
    char c = s[i];
    out[i] = (c >= 'a' && c <= 'z') ? char(c - 32) : c;
  }
  out[i] = 0;
}

void noteName(int note, char* out) {
  if (note < 0) note = 0;
  if (note > 127) note = 127;
  char* o = put(out, kNoteNames[note % 12]);
  itoa10(note / 12 - 1, o);
}

static bool isSwing(const ParamInfo* info, const ValueRef& r) {
  return info->fmt == ParamFormat::Swing || (r.global && r.index == kGSwing);
}
static bool isDj(const ParamInfo* info, const ValueRef& r) {
  return info->fmt == ParamFormat::DjFilter || (r.global && r.index == kGMasterFilter);
}

void formatValue(const ParamInfo* info, const ValueRef& ref, int v, char* out) {
  out[0] = 0;
  if (!info) {   // lock on a param the voice no longer has (UX 16.6): raw value
    itoa10(v, out);
    return;
  }
  const int lo = info->min, hi = info->max;
  if (isSwing(info, ref)) {   // 0..50 -> 50..75 % in half-percent steps: "54.5%"
    char* o = out + itoa10(50 + v / 2, out);
    if (v & 1) o = put(o, ".5");
    put(o, "%");
    return;
  }
  if (isDj(info, ref)) {
    if (v == 64) put(out, "OFF");
    else if (v < 64) itoa10(64 - v, put(out, "LP"));
    else itoa10(v - 64, put(out, "HP"));
    return;
  }
  if (!ref.global && ref.index == kMixPan && v == 64) { put(out, "C"); return; }
  switch (info->fmt) {
    case ParamFormat::Percent: {
      const int span = hi - lo;
      const int pct = span > 0 ? (2 * (v - lo) * 100 + span) / (2 * span) : 0;
      put(out + itoa10(pct, out), "%");
      return;
    }
    case ParamFormat::Bipolar: putSigned(out, v - (lo + hi + 1) / 2); return;
    case ParamFormat::Semitones: putSigned(out, v - (lo + hi) / 2); return;
    case ParamFormat::Enum:
      if (info->enumNames && v >= lo && v <= hi) {
        upperCopy(info->enumNames[v - lo], out, 6);
        return;
      }
      itoa10(v, out);
      return;
    case ParamFormat::OnOff: put(out, v ? "ON" : "OFF"); return;
    default: itoa10(v, out); return;
  }
}

BarKind barKind(const ParamInfo* info, const ValueRef& ref) {
  if (!info) return BarKind::Unipolar;
  if (info->fmt == ParamFormat::Enum || info->max - info->min <= 8) return BarKind::Segments;
  if (info->fmt == ParamFormat::Bipolar || info->fmt == ParamFormat::Semitones || isDj(info, ref))
    return BarKind::Bipolar;
  return BarKind::Unipolar;
}

float valueFrac(const ParamInfo* info, int v) {
  if (!info || info->max <= info->min) return 0.0f;
  float f = float(v - info->min) / float(info->max - info->min);
  return f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
}

bool isSmallRange(const ParamInfo* info) {
  return info && (info->fmt == ParamFormat::Enum || info->max - info->min + 1 <= 16);
}

// ---- scales ---------------------------------------------------------------
int scaleCount() { return engine_detail::kNumScales; }

int scaleSemitone(int scale, int degree) { return engine_detail::scaleDegreeToSemitone(scale, degree); }

bool scaleHasSemitone(int scale, int semi) {
  if (scale < 0 || scale >= engine_detail::kNumScales) scale = 0;
  semi %= 12;
  if (semi < 0) semi += 12;
  return (engine_detail::kScales[scale].mask >> semi) & 1;
}

int degreeNote(int root, int scale, int octave, int degree) {
  int n = 12 * (octave + 1) + root + scaleSemitone(scale, degree);
  return n < 0 ? 0 : (n > 127 ? 127 : n);
}

// ---- chords ---------------------------------------------------------------
const char* chordName(int c) { return (c >= 0 && c < kChordNum) ? kChordNames[c] : "?"; }
const char* chordShort(int c) { return (c >= 0 && c < kChordNum) ? kChordShort[c] : ""; }

const char* chordModeName(int m) {
  if (m == kModeNone) return "NONE";
  if (m == kModeAuto) return "AUTO";
  if (m == kModeAuto7) return "AUTO7";
  if (m >= kModeFixedFirst && m < kNumChordModes) return kChordNames[m - kModeFixedFirst + 1];
  return "?";
}

uint8_t chordFor(int mode, int scale, int d) {
  if (mode == kModeNone) return kChordNone;
  if (mode >= kModeFixedFirst && mode < kNumChordModes) return uint8_t(mode - kModeFixedFirst + 1);
  const int r = scaleSemitone(scale, d);
  const int i3 = scaleSemitone(scale, d + 2) - r;
  const int i5 = scaleSemitone(scale, d + 4) - r;
  if (mode == kModeAuto7) {
    const int i7 = scaleSemitone(scale, d + 6) - r;
    if (i3 == 4 && i5 == 7 && i7 == 11) return kChordMaj7;
    if (i3 == 3 && i5 == 7 && i7 == 10) return kChordMin7;
    if (i3 == 4 && i5 == 7 && i7 == 10) return kChordDom7;
  }
  if (i3 == 4 && i5 == 7) return kChordMaj;
  if (i3 == 3 && i5 == 7) return kChordMin;
  if (i3 == 5 && i5 == 7) return kChordSus4;
  return scaleHasSemitone(scale, r + 7) ? kChordFifth : kChordOct;
}

}  // namespace ui
}  // namespace pg
