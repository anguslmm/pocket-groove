// The .pgm text format (docs/MACROLAB.md): parser and canonical writer.
// No heap, no libc number parsing (strtod is double): numbers are read as
// exact thousandths here (at most kMaxDecimals digits after the point) and
// written back the same way, so parse(format(set)) reproduces every value.
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "macro.h"

#include "../sampler/sampler_params.h"

namespace pg {
namespace macro {

namespace {

const char* const kTrackNames[kNumTracks] = {"kick", "snare", "clap", "chh", "ohh", "perc", "bass", "lead"};
struct Alias {
  const char* name;
  uint8_t track;
};
const Alias kTrackAliases[] = {
    {"snar", kTrackSnare}, {"closedhat", kTrackHatClosed}, {"hatclosed", kTrackHatClosed},
    {"openhat", kTrackHatOpen}, {"hatopen", kTrackHatOpen},
};

inline char lower(char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; }
inline char upper(char c) { return (c >= 'a' && c <= 'z') ? char(c - 'a' + 'A') : c; }
inline bool isAlnum(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

bool ieq(const char* a, const char* b) {
  while (*a && *b && lower(*a) == lower(*b)) ++a, ++b;
  return *a == 0 && *b == 0;
}

// a == b ignoring case and every character that is not a letter or digit
bool looseEq(const char* a, const char* b) {
  for (;;) {
    while (*a && !isAlnum(*a)) ++a;
    while (*b && !isAlnum(*b)) ++b;
    if (!*a || !*b) return !*a && !*b;
    if (lower(*a) != lower(*b)) return false;
    ++a, ++b;
  }
}

// a is the start of b, ignoring case and every character that is not a letter or digit
bool loosePrefix(const char* a, const char* b) {
  for (;;) {
    while (*a && !isAlnum(*a)) ++a;
    while (*b && !isAlnum(*b)) ++b;
    if (!*a) return true;
    if (!*b || lower(*a) != lower(*b)) return false;
    ++a, ++b;
  }
}

// [+-]digits[.digits] with at most kMaxDecimals decimals, as exact thousandths.
bool parseMilli(const char* s, int32_t* out) {
  bool neg = false;
  if (*s == '+' || *s == '-') neg = *s++ == '-';
  int32_t ip = 0, fp = 0, digits = 0, decimals = 0;
  while (*s >= '0' && *s <= '9') {
    if (ip > 100000) return false;
    ip = ip * 10 + (*s++ - '0');
    ++digits;
  }
  if (*s == '.') {
    ++s;
    while (*s >= '0' && *s <= '9') {
      if (++decimals > kMaxDecimals) return false;
      fp = fp * 10 + (*s++ - '0');
      ++digits;
    }
  }
  if (*s || digits == 0) return false;
  for (int d = decimals; d < kMaxDecimals; ++d) fp *= 10;
  const int32_t v = ip * 1000 + fp;
  *out = neg ? -v : v;
  return true;
}

// Writes thousandths m as a number, trailing zeros trimmed.
int fmtMilli(int32_t m, char* o) {
  const bool neg = m < 0;
  const int32_t a = neg ? -m : m;
  int32_t ip = a / 1000, fp = a % 1000;
  char tmp[24];
  int n = 0;
  if (neg && a) tmp[n++] = '-';
  char d[12];
  int dn = 0;
  do { d[dn++] = char('0' + ip % 10); ip /= 10; } while (ip);
  while (dn) tmp[n++] = d[--dn];
  if (fp) {
    tmp[n++] = '.';
    char f[3] = {char('0' + fp / 100), char('0' + (fp / 10) % 10), char('0' + fp % 10)};
    int fl = 3;
    while (fl > 0 && f[fl - 1] == '0') --fl;
    for (int i = 0; i < fl; ++i) tmp[n++] = f[i];
  }
  std::memcpy(o, tmp, size_t(n));
  o[n] = 0;
  return n;
}

bool validName(const char* s) {
  for (const char* c = s; *c; ++c)
    if (!isAlnum(*c) && *c != '_' && *c != '-') return false;
  return true;
}

inline bool isString(const char* w) { return w[0] == '"'; }

// The tilt responses as the format writes them (pg/tilt/tilt.h, same order).
const char* const kTiltResponseNames[4] = {"fast", "smooth", "fine", "wide"};
const char* const kTiltAxisNames[2] = {"x", "y"};   // a quoted word (the tokenizer keeps the opening quote)

constexpr int kMaxTokens = 40;

enum class Kind { Target, Morph, Trim };

struct Parser {
  const ParamTable& pt;
  MacroSet* set;
  ParseError* err;
  Warnings* warn;
  int line = 0;
  bool haveVoice = false, haveName = false, haveTagline = false;
  int cur = -1;   // macro being filled
  // resolved at the end of the file (forward references allowed)
  char tBy[kMaxTargets][kMacroNameLen + 1];
  int tLine[kMaxTargets];
  uint8_t tMacro[kMaxTargets];
  char trBy[kMaxTrims][kMacroNameLen + 1];
  int trLine[kMaxTrims];
  char moPatch[kMaxMorphs][kPatchNameLen + 1];
  int moLine[kMaxMorphs];
  int paLine[kMaxPatches];
  char tiltName[2][kMacroNameLen + 1];   // the macro each tilt line names ("" = none / not given)
  int tiltLine[2];                       // 0 = no tilt line for that axis
  uint8_t tiltResp[2];
  const ParamInfo* kindScratch[kParamsPerTrack];
  TrackParams tp = nullptr;   // the list the set is checked against (its track and voice kind), from the voice line

  explicit Parser(const ParamTable& p, MacroSet* s, ParseError* e, Warnings* w) : pt(p), set(s), err(e), warn(w) {
    std::memset(tBy, 0, sizeof(tBy));
    std::memset(tLine, 0, sizeof(tLine));
    std::memset(tMacro, 0, sizeof(tMacro));
    std::memset(trBy, 0, sizeof(trBy));
    std::memset(trLine, 0, sizeof(trLine));
    std::memset(moPatch, 0, sizeof(moPatch));
    std::memset(moLine, 0, sizeof(moLine));
    std::memset(paLine, 0, sizeof(paLine));
    std::memset(tiltName, 0, sizeof(tiltName));
    std::memset(tiltLine, 0, sizeof(tiltLine));
    std::memset(tiltResp, 0, sizeof(tiltResp));
  }

  bool fail(const char* fmt, const char* a = "", const char* b = "") {
    err->line = line;
    std::snprintf(err->msg, sizeof(err->msg), fmt, a, b);
    return false;
  }
  bool failN(const char* fmt, int n, const char* a = "") {   // fmt has %s for a, then %d
    char c[12];
    std::snprintf(c, sizeof(c), "%d", n);
    return fail(fmt, a, c);
  }

  void warnf(int ln, const char* fmt, ...) {
    if (!warn) return;
    if (warn->count < kMaxWarnings) {
      ParseError& w = warn->w[warn->count];
      w.line = ln;
      va_list ap;
      va_start(ap, fmt);
      std::vsnprintf(w.msg, sizeof(w.msg), fmt, ap);
      va_end(ap);
    }
    ++warn->count;
  }

  const ParamInfo* info(int p) const { return tp[p]; }

  // A parameter value (set targets, base and patch values): an enum name, or a number in range.
  bool paramValue(int p, const char* tok, int32_t* out) {
    const ParamInfo* in = info(p);
    if (in->fmt == ParamFormat::Enum && in->enumNames) {
      for (int v = in->min; v <= in->max; ++v)
        if (in->enumNames[v - in->min] && ieq(in->enumNames[v - in->min], tok)) {
          *out = v * 1000;
          return true;
        }
    }
    int32_t v;
    if (!parseMilli(tok, &v)) {
      if (in->fmt == ParamFormat::Enum) return fail("'%s' is not a value of %s (see docs/MACROLAB.md)", tok, in->name);
      return fail("'%s' is not a number (at most 3 decimals)", tok);
    }
    if (v < in->min * 1000 || v > in->max * 1000) {
      char r[24];
      std::snprintf(r, sizeof(r), "%d..%d", in->min, in->max);
      return fail("%s is out of range (%s)", tok, r);
    }
    *out = v;
    return true;
  }
  static uint8_t roundValue(int32_t milli) { return uint8_t(int(milliToFloat(milli) + 0.5f)); }

  int param(const char* tok) {
    const int p = findParam(tp, tok);
    if (p == kAmbiguous) fail("'%s' could be more than one %s parameter: write more of the name", tok, kTrackNames[set->track]);
    else if (p < 0) fail("unknown parameter '%s' for %s", tok, kTrackNames[set->track]);
    return p < 0 ? -1 : p;
  }

  bool name(const char* tok, char* dst, int cap, const char* what) {
    const size_t n = std::strlen(tok);
    if (n > size_t(cap)) return failN(what, cap, tok);
    for (size_t i = 0; i <= n; ++i) dst[i] = upper(tok[i]);
    return true;
  }

  // a quoted word into dst (as written), 1..cap characters
  bool str(const char* w, char* dst, int cap, const char* what) {
    const char* s = w + 1;
    const size_t n = std::strlen(s);
    if (n == 0) return fail("an empty string \"\" (%s)", what);
    if (n > size_t(cap)) return failN("%s is longer than %s characters", cap, what);
    std::memcpy(dst, s, n + 1);
    return true;
  }

  bool statement(char** w, int n) {
    const char* kw = w[0];
    if (isString(kw)) return fail("a line cannot start with a string");
    if (!haveVoice) {
      if (!ieq(kw, "voice")) return fail("the first line must be 'voice <track>' (kick snare clap chh ohh perc bass lead)");
      if (n != 2 && n != 3) return fail("'voice' takes a track name and, optionally, a voice kind ('voice perc sampler')");
      const int t = trackFromName(w[1]);
      if (t < 0) return fail("unknown track '%s' (kick snare clap chh ohh perc bass lead)", w[1]);
      set->track = uint8_t(t);
      if (n == 3) {
        const int k = voiceKindFromName(w[2]);
        if (k < 0) return fail("unknown voice kind '%s' (sampler, or nothing for the track's own voice)", w[2]);
        if (!voiceKindOnTrack(k, t)) return fail("a %s instrument cannot go on %s (perc and lead host one)", w[2], w[1]);
        set->voice = uint8_t(k);
      }
      tp = kindParams(pt, t, set->voice, kindScratch);
      haveVoice = true;
      return true;
    }
    if (ieq(kw, "about")) {
      if (haveTagline) return fail("'about' given twice");
      if (n != 2 || !isString(w[1])) return fail("write 'about \"ONE SHORT SENTENCE\"' (up to 38 characters)");
      haveTagline = true;
      static_assert(kTaglineLen == 38, "the message above");
      return str(w[1], set->tagline, kTaglineLen, "the about line");
    }
    for (int i = 1; i < n; ++i)
      if (isString(w[i]) && !ieq(kw, "macro")) return fail("'%s': only a 'macro' or 'about' line takes strings", w[i] + 1);
    if (ieq(kw, "voice")) return fail("'voice' given twice");
    if (ieq(kw, "name")) {
      if (haveName) return fail("'name' given twice");
      if (n != 2) return fail("'name' takes one word");
      haveName = true;
      return name(w[1], set->name, kSetNameLen, "set name '%s' is longer than %s characters");
    }
    if (ieq(kw, "base")) return base(w, n);
    if (ieq(kw, "patch")) return patch(w, n);
    if (ieq(kw, "macro")) return macro(w, n);
    if (ieq(kw, "morph")) return curveLine(w, n, Kind::Morph, -1);
    if (ieq(kw, "trim")) return curveLine(w, n, Kind::Trim, -1);
    if (ieq(kw, "tilt")) return tilt(w, n);
    if (cur < 0) return fail("'%s': a target line needs a 'macro NAME' line above it", w[0]);
    const int p = param(w[0]);
    if (p < 0) return false;
    return curveLine(w, n, Kind::Target, p);
  }

  // tilt x|y NAME [fast|smooth|fine|wide]   or   tilt x|y none   (NAME resolved at the end)
  bool tilt(char** w, int n) {
    if (n < 3 || n > 4) return fail("write 'tilt x MACRO [fast|smooth|fine|wide]' or 'tilt y none'");
    const int a = ieq(w[1], "x") ? 0 : (ieq(w[1], "y") ? 1 : -1);
    if (a < 0) return fail("'tilt %s': the axis is x (roll left/right) or y (tilt away/toward)", w[1]);
    if (tiltLine[a]) return fail("'tilt %s' given twice", kTiltAxisNames[a]);
    int r = kTiltDefaultResponse;
    if (n == 4) {
      r = -1;
      for (int k = 0; k < 4; ++k)
        if (ieq(w[3], kTiltResponseNames[k])) r = k;
      if (r < 0) return fail("'%s' is not a tilt response (fast, smooth, fine, wide)", w[3]);
    }
    if (ieq(w[2], "none")) {
      if (n == 4) return fail("'tilt %s none' takes no response", kTiltAxisNames[a]);
      tiltName[a][0] = 0;
    } else {
      if (std::strlen(w[2]) > size_t(kMacroNameLen)) return fail("'tilt %s %s': no macro has that name", w[1], w[2]);
      for (size_t i = 0; i <= std::strlen(w[2]); ++i) tiltName[a][i] = upper(w[2][i]);
    }
    tiltLine[a] = line;
    tiltResp[a] = uint8_t(r);
    return true;
  }

  bool base(char** w, int n) {
    if (n < 2) return fail("'base' needs param=value pairs");
    for (int i = 1; i < n; ++i) {
      char* eq = std::strchr(w[i], '=');
      if (!eq || eq == w[i] || !eq[1]) return fail("'%s': write base values as param=value, no spaces", w[i]);
      *eq = 0;
      const int p = param(w[i]);
      if (p < 0) return false;
      if ((set->baseMask >> p) & 1) return fail("base value for %s given twice", info(p)->name);
      int32_t v;
      if (!paramValue(p, eq + 1, &v)) return false;
      set->base[p] = roundValue(v);
      set->baseMask |= 1u << p;
    }
    return true;
  }

  // A switch point (0 < s <= 1, at most 2 decimals) as hundredths.
  bool switchPoint(const char* tok, uint8_t* out) {
    int32_t v;
    if (!parseMilli(tok, &v) || v <= 0 || v > 1000 || v % 10)
      return fail("switch point '%s' must be a number above 0, at most 1, with at most 2 decimals", tok);
    *out = uint8_t(v / 10);
    return true;
  }

  bool patch(char** w, int n) {
    if (n < 3) return fail("write 'patch NAME param=value ...'");
    if (!validName(w[1])) return fail("patch name '%s': letters, digits, - and _ only", w[1]);
    int pi = set->findPatch(w[1]);
    if (pi < 0) {
      if (set->numPatches >= kMaxPatches) return failN("more than %s%s patches", kMaxPatches);
      Patch& pa = set->patches[set->numPatches];
      if (!name(w[1], pa.name, kPatchNameLen, "patch name '%s' is longer than %s characters")) return false;
      pa.first = set->numPatchValues;
      pa.n = 0;
      paLine[set->numPatches] = line;
      pi = set->numPatches++;
    }
    Patch& pa = set->patches[pi];
    for (int i = 2; i < n; ++i) {
      char* eq = std::strchr(w[i], '=');
      if (!eq || eq == w[i] || !eq[1]) return fail("'%s': write patch values as param=value, no spaces", w[i]);
      *eq = 0;
      char* at = std::strchr(eq + 1, '@');
      if (at) *at = 0;
      const int p = param(w[i]);
      if (p < 0) return false;
      for (int e = 0; e < pa.n; ++e)
        if (set->patchValues[pa.first + e].param == p) return fail("patch %s: %s given twice", pa.name, info(p)->name);
      int32_t v;
      if (!paramValue(p, eq + 1, &v)) return false;
      uint8_t sw = kNoSwitch;
      if (at) {
        if (info(p)->fmt != ParamFormat::Enum)
          return fail("'@%s': a switch point is only for enum parameters (%s is continuous)", at + 1, info(p)->name);
        if (!switchPoint(at + 1, &sw)) return false;
      }
      if (set->numPatchValues >= kMaxPatchValues) return failN("more than %s%s patch values in the set", kMaxPatchValues);
      // insert at the end of this patch (a patch may be continued after another one)
      const int at0 = pa.first + pa.n;
      std::memmove(&set->patchValues[at0 + 1], &set->patchValues[at0],
                   sizeof(PatchValue) * size_t(set->numPatchValues - at0));
      for (int q = 0; q < set->numPatches; ++q)
        if (q != pi && set->patches[q].first >= at0) ++set->patches[q].first;
      set->patchValues[at0] = PatchValue{uint8_t(p), roundValue(v), sw};
      ++pa.n;
      ++set->numPatchValues;
    }
    return true;
  }

  bool macro(char** w, int n) {
    if (n < 2 || isString(w[1])) return fail("write 'macro NAME', 'macro NAME START' (START 0..1), then optionally \"LOW\" \"HIGH\" \"description\"");
    if (set->numMacros >= kMaxMacros) return failN("more than %s%s macros", kMaxMacros);
    if (set->findMacro(w[1]) >= 0) return fail("macro '%s' declared twice", w[1]);
    if (!validName(w[1])) return fail("macro name '%s': letters, digits, - and _ only", w[1]);
    Macro& m = set->macros[set->numMacros];
    if (!name(w[1], m.name, kMacroNameLen, "macro name '%s' is longer than %s characters")) return false;
    m.start1000 = 0;
    m.first = set->numTargets;
    m.n = 0;
    int words = 0, strs = 0;
    const char* s[3] = {nullptr, nullptr, nullptr};
    for (int i = 2; i < n; ++i) {
      if (isString(w[i])) {
        if (strs >= 3) return fail("a macro takes at most three strings: \"LOW\" \"HIGH\" \"description\"");
        s[strs++] = w[i];
        continue;
      }
      if (strs > 0 || ++words > 1)
        return fail("write 'macro NAME', 'macro NAME START' (START 0..1), then optionally \"LOW\" \"HIGH\" \"description\"");
      int32_t v;
      if (!parseMilli(w[i], &v) || v < 0 || v > 1000) return fail("macro start '%s' must be a number 0..1", w[i]);
      m.start1000 = uint16_t(v);
    }
    // one string: the description; two: the end labels; three: both
    if (strs >= 2) {
      if (!str(s[0], m.lo, kLabelLen, "an end label") || !str(s[1], m.hi, kLabelLen, "an end label")) return false;
    }
    if (strs == 1 || strs == 3) {
      if (!str(s[strs - 1], m.desc, kDescLen, "a description")) return false;
    }
    cur = set->numMacros++;
    return true;
  }

  // A target (Kind::Target, parameter p), a morph line or a trim line of the current macro.
  bool curveLine(char** w, int n, Kind kind, int p) {
    if (cur < 0) return fail("'%s': needs a 'macro NAME' line above it", w[0]);
    Macro& m = set->macros[cur];
    const ParamInfo* in = p >= 0 ? info(p) : nullptr;
    Op op = Op::Set;
    Interp ip = (in && in->fmt == ParamFormat::Enum) ? Interp::Step : Interp::Linear;
    bool haveOp = false, haveInterp = false, haveBy = false, over = false, haveSw = false;
    const char* byName = nullptr;
    const char* patchName = nullptr;
    uint8_t sw = kNoSwitch;
    int i0 = 1;
    if (kind == Kind::Morph) {
      if (n < 2 || std::strchr(w[1], ':')) return fail("write 'morph PATCH' and optionally the amount curve");
      if (std::strlen(w[1]) > size_t(kPatchNameLen)) return fail("'morph %s': no patch has that name", w[1]);
      patchName = w[1];
      i0 = 2;
    }
    // words first (the op decides how point values read), then the points in order
    for (int i = i0; i < n; ++i) {
      const char* s = w[i];
      if (std::strchr(s, ':')) continue;
      if (kind == Kind::Target && (ieq(s, "set") || ieq(s, "add") || ieq(s, "mul"))) {
        if (haveOp) return fail("'%s': only one of set / add / mul per target", s);
        haveOp = true;
        op = ieq(s, "set") ? Op::Set : ieq(s, "add") ? Op::Add : Op::Mul;
      } else if (ieq(s, "linear") || ieq(s, "smooth") || ieq(s, "step")) {
        if (haveInterp) return fail("'%s': only one of linear / smooth / step per line", s);
        haveInterp = true;
        ip = ieq(s, "linear") ? Interp::Linear : ieq(s, "smooth") ? Interp::Smooth : Interp::Step;
      } else if (ieq(s, "by") && kind != Kind::Morph) {
        if (haveBy) return fail("'by' given twice");
        if (i + 1 >= n || std::strchr(w[i + 1], ':')) return fail("'by' needs a macro name after it");
        if (std::strlen(w[i + 1]) > size_t(kMacroNameLen)) return fail("'by %s': no macro has that name", w[i + 1]);
        byName = w[i + 1];
        haveBy = true;
        ++i;
      } else if (ieq(s, "override") && kind == Kind::Target) {
        if (over) return fail("'override' given twice");
        over = true;
      } else if (ieq(s, "switch") && kind == Kind::Morph) {
        if (haveSw) return fail("'switch' given twice");
        if (i + 1 >= n) return fail("'switch' needs a point after it (0 < point <= 1)");
        if (!switchPoint(w[i + 1], &sw)) return false;
        haveSw = true;
        ++i;
      } else if (*s) {
        if (kind == Kind::Morph && ieq(s, "by")) return fail("'by' is not for morph lines (the amount is the macro's own)");
        if (kind == Kind::Morph) return fail("'%s': expected a point pos:amount, linear/smooth/step or 'switch POINT'", s);
        if (kind == Kind::Trim) return fail("'%s': expected a point pos:dB, linear/smooth/step or 'by MACRO'", s);
        return fail("'%s': expected a point pos:value, set/add/mul, linear/smooth/step, 'by MACRO' or 'override'", s);
      }
    }
    const int first = set->numPoints;
    int np = 0;
    for (int i = i0; i < n; ++i) {
      char* s = w[i];
      char* colon = std::strchr(s, ':');
      if (!colon) continue;
      *colon = 0;
      if (np >= kMaxPoints) return failN("more than %s%s points on one curve", kMaxPoints);
      if (first + np >= kMaxSetPoints) return failN("more than %s%s curve points in the set", kMaxSetPoints);
      int32_t pos, v;
      if (!parseMilli(s, &pos) || pos < 0 || pos > 1000) return fail("point position '%s' must be a number 0..1", s);
      const char* vs = colon + 1;
      if (!*vs) return fail("point '%s:' has no value", s);
      if (kind == Kind::Morph) {
        if (!parseMilli(vs, &v) || v < 0 || v > 1000) return fail("morph amount '%s' must be a number 0..1", vs);
      } else if (kind == Kind::Trim) {
        if (!parseMilli(vs, &v)) return fail("'%s': trim points take plain numbers in dB (at most 3 decimals)", vs);
        if (v < -kTrimMaxDb * 1000 || v > kTrimMaxDb * 1000) return fail("trim %s dB is outside -24..24", vs);
      } else if (op == Op::Set) {
        if (!paramValue(p, vs, &v)) return false;
      } else if (!parseMilli(vs, &v)) {
        return fail("'%s': add and mul take plain numbers (at most 3 decimals)", vs);
      } else if (op == Op::Add && (v < -255000 || v > 255000)) {
        return fail("add offset %s is outside -255..255", vs);
      } else if (op == Op::Mul && (v < 0 || v > 16000)) {
        return fail("mul factor %s is outside 0..16", vs);
      }
      if (np > 0 && pos < pointPosMilli(set->points[first + np - 1])) {
        char prev[16];
        fmtMilli(pointPosMilli(set->points[first + np - 1]), prev);
        return fail("point positions go backwards (%s after %s)", s, prev);
      }
      set->points[first + np] = packPoint(pos, v);
      ++np;
    }
    if (kind == Kind::Target) {
      if (np == 0) return fail("target %s has no points (write pos:value, e.g. 0:20 1:120)", in->name);
      if (m.n >= kMaxMacroTargets) return failN("macro %s has more than %s targets", kMaxMacroTargets, m.name);
      if (set->numTargets >= kMaxTargets) return failN("more than %s%s targets in the set", kMaxTargets);
      const int j = set->numTargets++;
      Target& t = set->targets[j];
      t.param = uint8_t(p);
      t.first = uint8_t(first);
      t.n = uint8_t(np);
      t.setBits(op, ip, kNoMacro, over);
      if (byName) std::strcpy(tBy[j], byName);
      tLine[j] = line;
      tMacro[j] = uint8_t(cur);
      ++m.n;
    } else if (kind == Kind::Morph) {
      if (set->numMorphs >= kMaxMorphs) return failN("more than %s%s morph lines in the set", kMaxMorphs);
      const int k = set->numMorphs++;
      set->morphs[k] = Morph{uint8_t(cur), 0, sw, uint8_t(ip), uint8_t(np ? first : 0), uint8_t(np)};
      std::strcpy(moPatch[k], patchName);
      moLine[k] = line;
    } else {
      if (np == 0) return fail("a trim line needs points (pos:dB, e.g. 0:0 1:-3)");
      if (set->numTrims >= kMaxTrims) return failN("more than %s%s trim lines in the set", kMaxTrims);
      const int k = set->numTrims++;
      set->trims[k] = Trim{uint8_t(cur), kNoMacro, uint8_t(ip), uint8_t(first), uint8_t(np)};
      if (byName) std::strcpy(trBy[k], byName);
      trLine[k] = line;
    }
    set->numPoints = uint8_t(first + np);
    return true;
  }

  bool patchHas(int patch, int param) const {
    const Patch& pa = set->patches[patch];
    for (int e = 0; e < pa.n; ++e)
      if (set->patchValues[pa.first + e].param == param) return true;
    return false;
  }

  bool finish() {
    if (!haveVoice) {
      line = 0;
      return fail("empty file: the first line must be 'voice <track>'");
    }
    for (int j = 0; j < set->numTargets; ++j) {
      if (!tBy[j][0]) continue;
      const int b = set->findMacro(tBy[j]);
      line = tLine[j];
      if (b < 0) return fail("'by %s': no macro has that name", tBy[j]);
      Target& t = set->targets[j];
      t.setBits(t.op(), t.interp(), uint8_t(b), t.isOverride());
    }
    for (int k = 0; k < set->numTrims; ++k) {
      if (!trBy[k][0]) continue;
      const int b = set->findMacro(trBy[k]);
      line = trLine[k];
      if (b < 0) return fail("'by %s': no macro has that name", trBy[k]);
      set->trims[k].by = uint8_t(b);
    }
    for (int k = 0; k < set->numMorphs; ++k) {
      const int pa = set->findPatch(moPatch[k]);
      line = moLine[k];
      if (pa < 0) return fail("'morph %s': no patch has that name", moPatch[k]);
      set->morphs[k].patch = uint8_t(pa);
      for (int q = 0; q < k; ++q)
        if (set->morphs[q].macro == set->morphs[k].macro && set->morphs[q].patch == pa)
          return fail("'morph %s' given twice in macro %s", moPatch[k], set->macros[set->morphs[k].macro].name);
    }
    for (int a = 0; a < 2; ++a) {
      if (!tiltLine[a]) continue;
      line = tiltLine[a];
      int m = -1;
      if (tiltName[a][0]) {
        m = set->findMacro(tiltName[a]);
        if (m < 0) return fail("'tilt %s %s': no macro has that name", kTiltAxisNames[a], tiltName[a]);
      }
      set->tilt[a] = packTilt(m, tiltResp[a]);
    }
    if (tiltLine[0] && tiltLine[1]) {
      const TiltAssign x = set->tiltAssign(0), y = set->tiltAssign(1);
      if (x.macro >= 0 && x.macro == y.macro) {
        line = tiltLine[1];
        return fail("both tilt axes move %s: give one of them another macro or none", set->macros[x.macro].name);
      }
    }
    // the about line repeats the set name: "ACID -- family: ..." -> "family: ..."
    const size_t nl = std::strlen(set->name);
    if (nl && std::strlen(set->about) > nl + 4) {
      char head[kSetNameLen + 1];
      std::memcpy(head, set->about, nl);
      head[nl] = 0;
      if (ieq(head, set->name) && std::strncmp(set->about + nl, " -- ", 4) == 0)
        std::memmove(set->about, set->about + nl + 4, std::strlen(set->about + nl + 4) + 1);
    }
    warnings();
    return true;
  }

  void warnings() {
    if (!warn) return;
    // a plain set (no "by", not marked override) throws away whatever an earlier line did to its parameter
    for (int j = 0; j < set->numTargets; ++j) {
      const Target& t = set->targets[j];
      if (t.op() != Op::Set || t.by() != kNoMacro || t.isOverride()) continue;
      const char* pn = info(t.param)->name;
      bool done = false;
      for (int i = 0; i < j && !done; ++i)
        if (set->targets[i].param == t.param) {
          warnf(tLine[j], "%s: plain set undoes line %d (%s); add 'override' if meant", pn,
                tLine[i], set->macros[tMacro[i]].name);
          done = true;
        }
      for (int k = 0; k < set->numMorphs && !done; ++k)
        if (set->morphs[k].macro != tMacro[j] && patchHas(set->morphs[k].patch, t.param)) {
          warnf(tLine[j], "%s: plain set undoes 'morph %s' (%s); add 'override' if meant", pn,
                set->patches[set->morphs[k].patch].name, set->macros[set->morphs[k].macro].name);
          done = true;
        }
    }
    for (int q = 0; q < set->numPatches; ++q) {
      bool used = false;
      for (int k = 0; k < set->numMorphs; ++k) used |= set->morphs[k].patch == q;
      if (!used) warnf(paLine[q], "patch %s is not used by any morph line", set->patches[q].name);
    }
  }
};

}  // namespace

int trackFromName(const char* name) {
  for (int t = 0; t < kNumTracks; ++t)
    if (ieq(name, kTrackNames[t])) return t;
  for (const Alias& a : kTrackAliases)
    if (ieq(name, a.name)) return a.track;
  return -1;
}

const char* trackName(int track) { return track >= 0 && track < kNumTracks ? kTrackNames[track] : "?"; }

int findParam(const ParamTable& params, int track, const char* name) {
  if (track < 0 || track >= kNumTracks) return -1;
  return findParam(params.info[track], name);
}

int findParam(TrackParams info, const char* name) {
  if (!info || !name || !*name) return -1;
  for (int p = 0; p < kParamsPerTrack; ++p) {
    const ParamInfo* in = info[p];
    if (!in) continue;
    if (looseEq(in->name, name) || looseEq(in->longName, name)) return p;
  }
  // else a unique start of a long name, at least 3 letters ("reso" = Resonance)
  int n = 0;
  for (const char* c = name; *c; ++c) n += isAlnum(*c);
  if (n < 3) return -1;
  int found = -1;
  for (int p = 0; p < kParamsPerTrack; ++p) {
    const ParamInfo* in = info[p];
    if (!in || !loosePrefix(name, in->longName)) continue;
    if (found >= 0) return kAmbiguous;
    found = p;
  }
  return found;
}

bool parse(const char* text, size_t len, const ParamTable& params, MacroSet* out, ParseError* err, Warnings* warn) {
  out->clear();
  err->line = 0;
  err->msg[0] = 0;
  if (warn) warn->count = 0;
  Parser ps(params, out, err, warn);
  char buf[kMaxLineLen + 2];
  bool firstLine = true;   // the first line with anything on it; a comment there is the about line
  size_t i = 0;
  while (i < len) {
    ++ps.line;
    size_t n = 0;
    bool tooLong = false;
    while (i < len && text[i] != '\n') {
      const char c = text[i++];
      if (n < size_t(kMaxLineLen)) buf[n++] = c;
      else tooLong = true;
    }
    if (i < len) ++i;   // the '\n'
    if (tooLong) return ps.failN("line longer than %s%s characters", kMaxLineLen);
    buf[n] = 0;
    bool any = false;   // anything but blanks: the first such line may be the about line
    for (size_t k = 0; k < n && !any; ++k) any = buf[k] != ' ' && buf[k] != '\t' && buf[k] != '\r';
    bool inStr = false;
    for (size_t k = 0; k < n; ++k) {
      if (buf[k] == '"') inStr = !inStr;
      if (buf[k] == '#' && !inStr) {
        bool lead = true;   // nothing before the '#': a comment line
        for (size_t b = 0; b < k; ++b) lead &= buf[b] == ' ' || buf[b] == '\t' || buf[b] == '\r';
        if (firstLine && lead) {   // keep it as the set's about line (printable ASCII, cut to fit)
          size_t a = k + 1;
          while (a < n && (buf[a] == ' ' || buf[a] == '\t')) ++a;
          size_t m = 0;
          for (; a < n && m < size_t(kAboutLen); ++a)
            if (buf[a] != '\r') out->about[m++] = (uint8_t(buf[a]) >= 0x20 && uint8_t(buf[a]) < 0x7F) ? buf[a] : '?';
          while (m > 0 && out->about[m - 1] == ' ') --m;
          out->about[m] = 0;
        }
        buf[k] = 0;
        break;
      }
      if (buf[k] == '\t' || buf[k] == '\r') buf[k] = ' ';
      else if (uint8_t(buf[k]) < 0x20 || uint8_t(buf[k]) >= 0x7F) return ps.fail("a character that is not plain ASCII");
    }
    if (any) firstLine = false;
    if (inStr) return ps.fail("a string without its closing quote \"");
    char* w[kMaxTokens];
    int nw = 0;
    for (char* s = buf; *s;) {
      while (*s == ' ') ++s;
      if (!*s) break;
      if (nw >= kMaxTokens) return ps.fail("too many words on one line");
      w[nw++] = s;
      if (*s == '"') {   // a string: up to the closing quote, which must end the word
        char* q = std::strchr(s + 1, '"');
        if (!q) return ps.fail("a string without its closing quote \"");
        if (q[1] && q[1] != ' ') return ps.fail("write a space after a closing quote \"");
        *q = 0;
        s = q[1] ? q + 2 : q + 1;
        continue;
      }
      while (*s && *s != ' ') {
        if (*s == '"') return ps.fail("a quote \" inside a word: put strings in quotes on their own");
        ++s;
      }
      if (*s) *s++ = 0;
    }
    if (nw == 0) continue;
    if (!ps.statement(w, nw)) return false;
  }
  if (!ps.finish()) return false;
  out->id = out->name[0] ? setId(out->track, out->name) : kNoInstrument;
  // Canonical bytes: nothing after a string's NUL (the about line is trimmed in
  // place), so two parses of the same text, and the built-in table, compare equal.
  auto tail = [](char* s, size_t cap) {
    const size_t n = std::strlen(s);
    if (n < cap) std::memset(s + n, 0, cap - n);
  };
  tail(out->name, sizeof(out->name));
  tail(out->about, sizeof(out->about));
  tail(out->tagline, sizeof(out->tagline));
  for (Macro& m : out->macros) {
    tail(m.name, sizeof(m.name));
    tail(m.lo, sizeof(m.lo));
    tail(m.hi, sizeof(m.hi));
    tail(m.desc, sizeof(m.desc));
  }
  for (Patch& p : out->patches) tail(p.name, sizeof(p.name));
  return true;
}

// ---- writer ------------------------------------------------------------------------------
namespace {
struct Out {
  char* b;
  size_t cap, n = 0;
  bool ok = true;
  size_t lineStart = 0;
  void put(const char* s) {
    const size_t l = std::strlen(s);
    if (n + l + 1 > cap) { ok = false; return; }
    std::memcpy(b + n, s, l);
    n += l;
    b[n] = 0;
  }
  void nl() {
    put("\n");
    lineStart = n;
  }
  size_t col() const { return n - lineStart; }
  void milli(int32_t v) {
    char t[24];
    fmtMilli(v, t);
    put(t);
  }
  void str(const char* s) {
    put(" \"");
    put(s);
    put("\"");
  }
};

void lowerName(const char* s, char* o) {
  while (*s) *o++ = lower(*s++);
  *o = 0;
}

// a value of parameter p as the file writes it (thousandths): the enum name if it has one
void putValue(Out& o, const ParamInfo* in, int32_t milli) {
  const int iv = milli / 1000;
  if (in && in->fmt == ParamFormat::Enum && in->enumNames && milli % 1000 == 0 && iv >= in->min && iv <= in->max &&
      in->enumNames[iv - in->min]) {
    char t[16];
    lowerName(in->enumNames[iv - in->min], t);
    o.put(t);
    return;
  }
  o.milli(milli);
}

void putPoints(Out& o, const MacroSet& set, int first, int n, const ParamInfo* setParam) {
  for (int k = 0; k < n; ++k) {
    const Point p = set.points[first + k];
    o.put(" ");
    o.milli(pointPosMilli(p));
    o.put(":");
    if (setParam) putValue(o, setParam, pointValMilli(p));
    else o.milli(pointValMilli(p));
  }
}

void putInterp(Out& o, Interp ip, Interp def) {
  if (ip != def) o.put(ip == Interp::Linear ? " linear" : ip == Interp::Smooth ? " smooth" : " step");
}
}  // namespace

size_t format(const MacroSet& set, const ParamTable& params, char* buf, size_t cap) {
  if (cap == 0) return 0;
  buf[0] = 0;
  Out o{buf, cap};
  const int tr = set.track < kNumTracks ? set.track : 0;
  const ParamInfo* kindScratch[kParamsPerTrack];
  const TrackParams info = paramsFor(set, params, kindScratch);
  char t[48];
  if (set.about[0]) {
    o.put("# ");
    if (set.name[0]) {
      o.put(set.name);
      o.put(" -- ");
    }
    o.put(set.about);
    o.nl();
  }
  o.put("voice ");
  o.put(kTrackNames[tr]);
  if (set.voice != kVoiceNative && voiceKindName(set.voice)) {
    o.put(" ");
    o.put(voiceKindName(set.voice));
  }
  o.nl();
  if (set.name[0]) {
    o.put("name ");
    o.put(set.name);
    o.nl();
  }
  if (set.tagline[0]) {
    o.put("about \"");
    o.put(set.tagline);
    o.put("\"");
    o.nl();
  }
  for (int a = 0; a < 2; ++a) {   // only what the file gave (0 = the default)
    if (!(set.tilt[a] & kTiltGiven)) continue;
    const int m = set.tilt[a] & 0x0F;
    o.put("tilt ");
    o.put(kTiltAxisNames[a]);
    o.put(" ");
    if (m >= set.numMacros || m >= kMaxMacros) {
      o.put("none");
    } else {
      o.put(set.macros[m].name);
      const int r = (set.tilt[a] >> 4) & 3;
      if (r != kTiltDefaultResponse) {
        o.put(" ");
        o.put(kTiltResponseNames[r]);
      }
    }
    o.nl();
  }
  if (set.baseMask) {
    o.put("base");
    for (int p = 0; p < kParamsPerTrack; ++p) {
      if (!((set.baseMask >> p) & 1) || !info[p]) continue;
      if (o.col() > 150) {
        o.nl();
        o.put("base");
      }
      lowerName(info[p]->name, t);
      o.put(" ");
      o.put(t);
      o.put("=");
      putValue(o, info[p], int32_t(set.base[p]) * 1000);
    }
    o.nl();
  }
  for (int q = 0; q < set.numPatches && q < kMaxPatches; ++q) {
    const Patch& pa = set.patches[q];
    o.put("patch ");
    o.put(pa.name);
    for (int e = 0; e < pa.n; ++e) {
      const PatchValue& pv = set.patchValues[pa.first + e];
      const ParamInfo* in = pv.param < kParamsPerTrack ? info[pv.param] : nullptr;
      if (!in) continue;
      if (o.col() > 150) {
        o.nl();
        o.put("patch ");
        o.put(pa.name);
      }
      lowerName(in->name, t);
      o.put(" ");
      o.put(t);
      o.put("=");
      putValue(o, in, int32_t(pv.value) * 1000);
      if (pv.sw != kNoSwitch) {
        o.put("@");
        o.milli(int32_t(pv.sw) * 10);
      }
    }
    o.nl();
  }
  for (int i = 0; i < set.numMacros && i < kMaxMacros; ++i) {
    const Macro& m = set.macros[i];
    o.put("macro ");
    o.put(m.name);
    if (m.start1000) {
      o.put(" ");
      o.milli(m.start1000);
    }
    if (m.lo[0] || m.hi[0]) {
      o.str(m.lo);
      o.str(m.hi);
    }
    if (m.desc[0]) o.str(m.desc);
    o.nl();
    for (int k = 0; k < set.numMorphs && k < kMaxMorphs; ++k) {
      const Morph& mo = set.morphs[k];
      if (mo.macro != i || mo.patch >= set.numPatches) continue;
      o.put("  morph ");
      o.put(set.patches[mo.patch].name);
      putPoints(o, set, mo.first, mo.n, nullptr);
      putInterp(o, Interp(mo.interp), Interp::Linear);
      if (mo.sw != kNoSwitch) {
        o.put(" switch ");
        o.milli(int32_t(mo.sw) * 10);
      }
      o.nl();
    }
    for (int j = 0; j < m.n && m.first + j < kMaxTargets; ++j) {
      const Target& tg = set.targets[m.first + j];
      const ParamInfo* in = tg.param < kParamsPerTrack ? info[tg.param] : nullptr;
      if (!in) continue;
      lowerName(in->name, t);
      o.put("  ");
      o.put(t);
      if (tg.op() != Op::Set) o.put(tg.op() == Op::Add ? " add" : " mul");
      putPoints(o, set, tg.first, tg.n, tg.op() == Op::Set ? in : nullptr);
      putInterp(o, tg.interp(), in->fmt == ParamFormat::Enum ? Interp::Step : Interp::Linear);
      if (tg.by() < set.numMacros) {
        o.put(" by ");
        o.put(set.macros[tg.by()].name);
      }
      if (tg.isOverride()) o.put(" override");
      o.nl();
    }
    for (int k = 0; k < set.numTrims && k < kMaxTrims; ++k) {
      const Trim& tm = set.trims[k];
      if (tm.macro != i) continue;
      o.put("  trim");
      putPoints(o, set, tm.first, tm.n, nullptr);
      putInterp(o, Interp(tm.interp), Interp::Linear);
      if (tm.by < set.numMacros) {
        o.put(" by ");
        o.put(set.macros[tm.by].name);
      }
      o.nl();
    }
  }
  return o.ok ? o.n : 0;
}

}  // namespace macro
}  // namespace pg
