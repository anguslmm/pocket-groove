// The few JSON lines `tools/pg --json` prints (one object per line), parsed
// for the Stems list. A small complete JSON reader (objects, arrays, strings
// with escapes, numbers, true/false/null); no exceptions, no allocation tricks.
#pragma once
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace desk {

struct JVal {
  enum Type { Null, Bool, Num, Str, Arr, Obj } type = Null;
  bool b = false;
  double n = 0.0;
  std::string s;
  std::vector<JVal> a;
  std::vector<std::pair<std::string, JVal>> o;

  const JVal* get(const char* key) const {
    if (type != Obj) return nullptr;
    for (const auto& kv : o)
      if (kv.first == key) return &kv.second;
    return nullptr;
  }
  std::string str(const char* key, const char* def = "") const {
    const JVal* v = get(key);
    return v && v->type == Str ? v->s : std::string(def);
  }
  double num(const char* key, double def = 0.0) const {
    const JVal* v = get(key);
    return v && v->type == Num ? v->n : def;
  }
  bool boolean(const char* key, bool def = false) const {
    const JVal* v = get(key);
    return v && v->type == Bool ? v->b : def;
  }
};

class JsonReader {
 public:
  static bool parse(const std::string& text, JVal* out) {
    JsonReader r(text);
    r.ws();
    if (!r.value(out, 0)) return false;
    r.ws();
    return r.i_ == text.size();
  }

 private:
  explicit JsonReader(const std::string& t) : t_(t) {}
  void ws() {
    while (i_ < t_.size() && (t_[i_] == ' ' || t_[i_] == '\t' || t_[i_] == '\n' || t_[i_] == '\r')) ++i_;
  }
  bool lit(const char* w) {
    size_t k = 0;
    while (w[k]) {
      if (i_ + k >= t_.size() || t_[i_ + k] != w[k]) return false;
      ++k;
    }
    i_ += k;
    return true;
  }
  static void utf8(std::string& s, unsigned cp) {
    if (cp < 0x80) s += char(cp);
    else if (cp < 0x800) { s += char(0xC0 | (cp >> 6)); s += char(0x80 | (cp & 0x3F)); }
    else { s += char(0xE0 | (cp >> 12)); s += char(0x80 | ((cp >> 6) & 0x3F)); s += char(0x80 | (cp & 0x3F)); }
  }
  bool string(std::string* s) {
    if (i_ >= t_.size() || t_[i_] != '"') return false;
    ++i_;
    while (i_ < t_.size()) {
      const char c = t_[i_++];
      if (c == '"') return true;
      if (c != '\\') { *s += c; continue; }
      if (i_ >= t_.size()) return false;
      const char e = t_[i_++];
      switch (e) {
        case '"': *s += '"'; break;
        case '\\': *s += '\\'; break;
        case '/': *s += '/'; break;
        case 'b': *s += '\b'; break;
        case 'f': *s += '\f'; break;
        case 'n': *s += '\n'; break;
        case 'r': *s += '\r'; break;
        case 't': *s += '\t'; break;
        case 'u': {
          if (i_ + 4 > t_.size()) return false;
          unsigned cp = unsigned(std::strtoul(t_.substr(i_, 4).c_str(), nullptr, 16));
          i_ += 4;
          utf8(*s, cp);
          break;
        }
        default: return false;
      }
    }
    return false;
  }
  bool value(JVal* v, int depth) {
    if (depth > 32 || i_ >= t_.size()) return false;
    const char c = t_[i_];
    if (c == '{') {
      v->type = JVal::Obj;
      ++i_;
      ws();
      if (i_ < t_.size() && t_[i_] == '}') { ++i_; return true; }
      for (;;) {
        ws();
        std::string k;
        if (!string(&k)) return false;
        ws();
        if (i_ >= t_.size() || t_[i_] != ':') return false;
        ++i_;
        ws();
        v->o.emplace_back(std::move(k), JVal());
        if (!value(&v->o.back().second, depth + 1)) return false;
        ws();
        if (i_ < t_.size() && t_[i_] == ',') { ++i_; continue; }
        if (i_ < t_.size() && t_[i_] == '}') { ++i_; return true; }
        return false;
      }
    }
    if (c == '[') {
      v->type = JVal::Arr;
      ++i_;
      ws();
      if (i_ < t_.size() && t_[i_] == ']') { ++i_; return true; }
      for (;;) {
        ws();
        v->a.emplace_back();
        if (!value(&v->a.back(), depth + 1)) return false;
        ws();
        if (i_ < t_.size() && t_[i_] == ',') { ++i_; continue; }
        if (i_ < t_.size() && t_[i_] == ']') { ++i_; return true; }
        return false;
      }
    }
    if (c == '"') { v->type = JVal::Str; return string(&v->s); }
    if (lit("true")) { v->type = JVal::Bool; v->b = true; return true; }
    if (lit("false")) { v->type = JVal::Bool; v->b = false; return true; }
    if (lit("null")) { v->type = JVal::Null; return true; }
    const char* p = t_.c_str() + i_;
    char* end = nullptr;
    v->n = std::strtod(p, &end);
    if (end == p) return false;
    v->type = JVal::Num;
    i_ += size_t(end - p);
    return true;
  }
  const std::string& t_;
  size_t i_ = 0;
};

}  // namespace desk
