// The .pgm text format (docs/MACROLAB.md): parser and canonical writer.
// No heap, no libc number parsing (strtod is double): numbers are read and
// written here, with at most kMaxDecimals digits after the point, so that
// parse(format(set)) reproduces every float exactly.
#include <cstdio>
#include <cstring>

#include "macro.h"

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

// [+-]digits[.digits] with at most kMaxDecimals decimals. Exact for what format() writes.
bool parseNum(const char* s, float* out) {
  bool neg = false;
  if (*s == '+' || *s == '-') neg = *s++ == '-';
  int32_t ip = 0, fp = 0, scale = 1, digits = 0, decimals = 0;
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
      scale *= 10;
      ++digits;
    }
  }
  if (*s || digits == 0) return false;
  const float v = float(ip) + float(fp) / float(scale);
  *out = neg ? -v : v;
  return true;
}

// Writes v with up to kMaxDecimals decimals, trailing zeros trimmed.
int fmtNum(float v, char* o) {
  const bool neg = v < 0.0f;
  const float a = neg ? -v : v;
  const int32_t milli = int32_t(a * 1000.0f + 0.5f);
  int32_t ip = milli / 1000, fp = milli % 1000;
  char tmp[24];
  int n = 0;
  if (neg && milli) tmp[n++] = '-';
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

constexpr int kMaxTokens = 40;

struct Parser {
  const ParamTable& pt;
  MacroSet* set;
  ParseError* err;
  int line = 0;
  bool haveVoice = false, haveName = false;
  int cur = -1;   // macro being filled
  // "by" names, resolved at the end of the file (forward references allowed)
  char byName[kMaxMacros][kMaxTargets][kMacroNameLen + 1];
  int byLine[kMaxMacros][kMaxTargets];

  explicit Parser(const ParamTable& p, MacroSet* s, ParseError* e) : pt(p), set(s), err(e) {
    std::memset(byName, 0, sizeof(byName));
    std::memset(byLine, 0, sizeof(byLine));
  }

  bool fail(const char* fmt, const char* a = "", const char* b = "") {
    err->line = line;
    std::snprintf(err->msg, sizeof(err->msg), fmt, a, b);
    return false;
  }

  const ParamInfo* info(int p) const { return pt.info[set->track][p]; }

  // A parameter value (set targets, base patch): an enum name, or a number in range.
  bool paramValue(int p, const char* tok, float* out) {
    const ParamInfo* in = info(p);
    if (in->fmt == ParamFormat::Enum && in->enumNames) {
      for (int v = in->min; v <= in->max; ++v)
        if (in->enumNames[v - in->min] && ieq(in->enumNames[v - in->min], tok)) {
          *out = float(v);
          return true;
        }
    }
    float v;
    if (!parseNum(tok, &v)) {
      if (in->fmt == ParamFormat::Enum) return fail("'%s' is not a value of %s (see docs/MACROLAB.md)", tok, in->name);
      return fail("'%s' is not a number (at most 3 decimals)", tok);
    }
    if (v < float(in->min) || v > float(in->max)) {
      char r[24];
      std::snprintf(r, sizeof(r), "%d..%d", in->min, in->max);
      return fail("%s is out of range (%s)", tok, r);
    }
    *out = v;
    return true;
  }

  int param(const char* tok) {
    const int p = findParam(pt, set->track, tok);
    if (p == kAmbiguous) fail("'%s' could be more than one %s parameter: write more of the name", tok, kTrackNames[set->track]);
    else if (p < 0) fail("unknown parameter '%s' for %s", tok, kTrackNames[set->track]);
    return p < 0 ? -1 : p;
  }

  bool name(const char* tok, char* dst, int cap, const char* what) {
    const size_t n = std::strlen(tok);
    if (n > size_t(cap)) {
      char c[8];
      std::snprintf(c, sizeof(c), "%d", cap);
      return fail(what, tok, c);
    }
    for (size_t i = 0; i <= n; ++i) dst[i] = upper(tok[i]);
    return true;
  }

  bool statement(char** w, int n) {
    const char* kw = w[0];
    if (!haveVoice) {
      if (!ieq(kw, "voice")) return fail("the first line must be 'voice <track>' (kick snare clap chh ohh perc bass lead)");
      if (n != 2) return fail("'voice' takes one track name");
      const int t = trackFromName(w[1]);
      if (t < 0) return fail("unknown track '%s' (kick snare clap chh ohh perc bass lead)", w[1]);
      set->track = uint8_t(t);
      haveVoice = true;
      return true;
    }
    if (ieq(kw, "voice")) return fail("'voice' given twice");
    if (ieq(kw, "name")) {
      if (haveName) return fail("'name' given twice");
      if (n != 2) return fail("'name' takes one word");
      haveName = true;
      return name(w[1], set->name, kSetNameLen, "set name '%s' is longer than %s characters");
    }
    if (ieq(kw, "base")) {
      if (n < 2) return fail("'base' needs param=value pairs");
      for (int i = 1; i < n; ++i) {
        char* eq = std::strchr(w[i], '=');
        if (!eq || eq == w[i] || !eq[1]) return fail("'%s': write base values as param=value, no spaces", w[i]);
        *eq = 0;
        const int p = param(w[i]);
        if (p < 0) return false;
        if ((set->baseMask >> p) & 1) return fail("base value for %s given twice", info(p)->name);
        float v;
        if (!paramValue(p, eq + 1, &v)) return false;
        set->base[p] = uint8_t(int(v + 0.5f));
        set->baseMask |= 1u << p;
      }
      return true;
    }
    if (ieq(kw, "macro")) {
      if (n < 2 || n > 3) return fail("write 'macro NAME' or 'macro NAME START' (START 0..1)");
      if (set->numMacros >= kMaxMacros) {
        char c[8];
        std::snprintf(c, sizeof(c), "%d", kMaxMacros);
        return fail("more than %s macros", c);
      }
      if (set->findMacro(w[1]) >= 0) return fail("macro '%s' declared twice", w[1]);
      for (const char* c = w[1]; *c; ++c)
        if (!isAlnum(*c) && *c != '_' && *c != '-') return fail("macro name '%s': letters, digits, - and _ only", w[1]);
      Macro& m = set->macros[set->numMacros];
      if (!name(w[1], m.name, kMacroNameLen, "macro name '%s' is longer than %s characters")) return false;
      m.start = 0.0f;
      if (n == 3 && (!parseNum(w[2], &m.start) || m.start < 0.0f || m.start > 1.0f))
        return fail("macro start '%s' must be a number 0..1", w[2]);
      cur = set->numMacros++;
      return true;
    }
    return target(w, n);
  }

  bool target(char** w, int n) {
    if (cur < 0) return fail("'%s': a target line needs a 'macro NAME' line above it", w[0]);
    Macro& m = set->macros[cur];
    if (m.numTargets >= kMaxTargets) {
      char c[8];
      std::snprintf(c, sizeof(c), "%d", kMaxTargets);
      return fail("macro %s has more than %s targets", m.name, c);
    }
    const int p = param(w[0]);
    if (p < 0) return false;
    Target& t = m.targets[m.numTargets];
    std::memset(&t, 0, sizeof(t));
    t.param = uint8_t(p);
    t.op = Op::Set;
    t.by = kNoMacro;
    const ParamInfo* in = info(p);
    t.curve.interp = in->fmt == ParamFormat::Enum ? Interp::Step : Interp::Linear;
    bool haveOp = false, haveInterp = false, haveBy = false;
    // words first (the op decides how point values read), then the points in order
    for (int i = 1; i < n; ++i) {
      const char* s = w[i];
      if (std::strchr(s, ':')) continue;
      Op op;
      Interp ip;
      if (ieq(s, "set") || ieq(s, "add") || ieq(s, "mul")) {
        op = ieq(s, "set") ? Op::Set : ieq(s, "add") ? Op::Add : Op::Mul;
        if (haveOp) return fail("'%s': only one of set / add / mul per target", s);
        haveOp = true;
        t.op = op;
      } else if (ieq(s, "linear") || ieq(s, "smooth") || ieq(s, "step")) {
        ip = ieq(s, "linear") ? Interp::Linear : ieq(s, "smooth") ? Interp::Smooth : Interp::Step;
        if (haveInterp) return fail("'%s': only one of linear / smooth / step per target", s);
        haveInterp = true;
        t.curve.interp = ip;
      } else if (ieq(s, "by")) {
        if (haveBy) return fail("'by' given twice");
        if (i + 1 >= n || std::strchr(w[i + 1], ':')) return fail("'by' needs a macro name after it");
        if (std::strlen(w[i + 1]) > size_t(kMacroNameLen)) return fail("'by %s': no macro has that name", w[i + 1]);
        std::strcpy(byName[cur][m.numTargets], w[i + 1]);
        byLine[cur][m.numTargets] = line;
        haveBy = true;
        w[i + 1][0] = 0;   // consumed
        ++i;
      } else if (*s) {
        return fail("'%s': expected a point pos:value, set/add/mul, linear/smooth/step or 'by MACRO'", s);
      }
    }
    for (int i = 1; i < n; ++i) {
      char* s = w[i];
      char* colon = std::strchr(s, ':');
      if (!colon) continue;
      *colon = 0;
      if (t.curve.n >= kMaxPoints) {
        char c[8];
        std::snprintf(c, sizeof(c), "%d", kMaxPoints);
        return fail("more than %s points on one curve", c);
      }
      float pos, v;
      if (!parseNum(s, &pos) || pos < 0.0f || pos > 1.0f) return fail("point position '%s' must be a number 0..1", s);
      const char* vs = colon + 1;
      if (!*vs) return fail("point '%s:' has no value", s);
      if (t.op == Op::Set) {
        if (!paramValue(p, vs, &v)) return false;
      } else if (!parseNum(vs, &v)) {
        return fail("'%s': add and mul take plain numbers (at most 3 decimals)", vs);
      } else if (t.op == Op::Add && (v < -255.0f || v > 255.0f)) {
        return fail("add offset %s is outside -255..255", vs);
      } else if (t.op == Op::Mul && (v < 0.0f || v > 16.0f)) {
        return fail("mul factor %s is outside 0..16", vs);
      }
      if (t.curve.n > 0 && pos < t.curve.pos[t.curve.n - 1]) {
        char prev[16];
        fmtNum(t.curve.pos[t.curve.n - 1], prev);
        return fail("point positions go backwards (%s after %s)", s, prev);
      }
      t.curve.pos[t.curve.n] = pos;
      t.curve.val[t.curve.n] = v;
      ++t.curve.n;
    }
    if (t.curve.n == 0) return fail("target %s has no points (write pos:value, e.g. 0:20 1:120)", in->name);
    ++m.numTargets;
    return true;
  }

  bool finish() {
    if (!haveVoice) {
      line = 0;
      return fail("empty file: the first line must be 'voice <track>'");
    }
    for (int i = 0; i < set->numMacros; ++i)
      for (int j = 0; j < set->macros[i].numTargets; ++j) {
        if (!byName[i][j][0]) continue;
        const int b = set->findMacro(byName[i][j]);
        line = byLine[i][j];
        if (b < 0) return fail("'by %s': no macro has that name", byName[i][j]);
        set->macros[i].targets[j].by = uint8_t(b);
      }
    return true;
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
  if (track < 0 || track >= kNumTracks || !name || !*name) return -1;
  for (int p = 0; p < kParamsPerTrack; ++p) {
    const ParamInfo* in = params.info[track][p];
    if (!in) continue;
    if (looseEq(in->name, name) || looseEq(in->longName, name)) return p;
  }
  // else a unique start of a long name, at least 3 letters ("reso" = Resonance)
  int n = 0;
  for (const char* c = name; *c; ++c) n += isAlnum(*c);
  if (n < 3) return -1;
  int found = -1;
  for (int p = 0; p < kParamsPerTrack; ++p) {
    const ParamInfo* in = params.info[track][p];
    if (!in || !loosePrefix(name, in->longName)) continue;
    if (found >= 0) return kAmbiguous;
    found = p;
  }
  return found;
}

bool parse(const char* text, size_t len, const ParamTable& params, MacroSet* out, ParseError* err) {
  out->clear();
  err->line = 0;
  err->msg[0] = 0;
  Parser ps(params, out, err);
  char buf[kMaxLineLen + 2];
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
    if (tooLong) {
      char c[8];
      std::snprintf(c, sizeof(c), "%d", kMaxLineLen);
      return ps.fail("line longer than %s characters", c);
    }
    buf[n] = 0;
    for (size_t k = 0; k < n; ++k) {
      if (buf[k] == '#') { buf[k] = 0; break; }
      if (buf[k] == '\t' || buf[k] == '\r') buf[k] = ' ';
      else if (uint8_t(buf[k]) < 0x20 || uint8_t(buf[k]) >= 0x7F) return ps.fail("a character that is not plain ASCII");
    }
    char* w[kMaxTokens];
    int nw = 0;
    for (char* s = buf; *s;) {
      while (*s == ' ') ++s;
      if (!*s) break;
      if (nw >= kMaxTokens) return ps.fail("too many words on one line");
      w[nw++] = s;
      while (*s && *s != ' ') ++s;
      if (*s) *s++ = 0;
    }
    if (nw == 0) continue;
    if (!ps.statement(w, nw)) return false;
  }
  return ps.finish();
}

// ---- writer ------------------------------------------------------------------------------
namespace {
struct Out {
  char* b;
  size_t cap, n = 0;
  bool ok = true;
  void put(const char* s) {
    const size_t l = std::strlen(s);
    if (n + l + 1 > cap) { ok = false; return; }
    std::memcpy(b + n, s, l);
    n += l;
    b[n] = 0;
  }
  void num(float v) {
    char t[24];
    fmtNum(v, t);
    put(t);
  }
};

void lowerName(const char* s, char* o) {
  while (*s) *o++ = lower(*s++);
  *o = 0;
}

// a value of parameter p as the file writes it: the enum name if it has one
void putValue(Out& o, const ParamInfo* in, float v) {
  const int iv = int(v);
  if (in && in->fmt == ParamFormat::Enum && in->enumNames && float(iv) == v && iv >= in->min && iv <= in->max &&
      in->enumNames[iv - in->min]) {
    char t[16];
    lowerName(in->enumNames[iv - in->min], t);
    o.put(t);
    return;
  }
  o.num(v);
}
}  // namespace

size_t format(const MacroSet& set, const ParamTable& params, char* buf, size_t cap) {
  if (cap == 0) return 0;
  buf[0] = 0;
  Out o{buf, cap};
  const int tr = set.track < kNumTracks ? set.track : 0;
  char t[48];
  o.put("voice ");
  o.put(kTrackNames[tr]);
  o.put("\n");
  if (set.name[0]) {
    o.put("name ");
    o.put(set.name);
    o.put("\n");
  }
  if (set.baseMask) {
    o.put("base");
    for (int p = 0; p < kParamsPerTrack; ++p) {
      if (!((set.baseMask >> p) & 1) || !params.info[tr][p]) continue;
      lowerName(params.info[tr][p]->name, t);
      o.put(" ");
      o.put(t);
      o.put("=");
      putValue(o, params.info[tr][p], float(set.base[p]));
    }
    o.put("\n");
  }
  for (int i = 0; i < set.numMacros && i < kMaxMacros; ++i) {
    const Macro& m = set.macros[i];
    o.put("macro ");
    o.put(m.name);
    if (m.start != 0.0f) {
      o.put(" ");
      o.num(m.start);
    }
    o.put("\n");
    for (int j = 0; j < m.numTargets && j < kMaxTargets; ++j) {
      const Target& tg = m.targets[j];
      const ParamInfo* in = tg.param < kParamsPerTrack ? params.info[tr][tg.param] : nullptr;
      if (!in) continue;
      lowerName(in->name, t);
      o.put("  ");
      o.put(t);
      if (tg.op != Op::Set) o.put(tg.op == Op::Add ? " add" : " mul");
      for (int k = 0; k < tg.curve.n && k < kMaxPoints; ++k) {
        o.put(" ");
        o.num(tg.curve.pos[k]);
        o.put(":");
        if (tg.op == Op::Set) putValue(o, in, tg.curve.val[k]);
        else o.num(tg.curve.val[k]);
      }
      const Interp def = in->fmt == ParamFormat::Enum ? Interp::Step : Interp::Linear;
      if (tg.curve.interp != def)
        o.put(tg.curve.interp == Interp::Linear ? " linear" : tg.curve.interp == Interp::Smooth ? " smooth" : " step");
      if (tg.by < set.numMacros) {
        o.put(" by ");
        o.put(set.macros[tg.by].name);
      }
      o.put("\n");
    }
  }
  return o.ok ? o.n : 0;
}

}  // namespace macro
}  // namespace pg
