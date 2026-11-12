#include "keymap.h"

#include <SDL_keyboard.h>
#include <SDL_scancode.h>

#include <cctype>
#include <cstdio>
#include <cstring>

#include "pg/ui/keys.h"

namespace desk {

namespace {

struct Bind {
  SDL_Scancode sc;
  int16_t b;
};

using namespace pg::ui;

// The Cardputer rows, left to right, and the Mac key in the same place.
// Letters, digits and punctuation one-to-one; the modifiers are the decision
// in docs/DESKTOP.md (SHIFT = left Shift, FN = right Shift, OPT = left Option,
// ALT = right Option, CTRL = either Control). Command is never bound: macOS
// and SDL act on Cmd+Q/H/W/M/Tab.
const Bind kDefaultBindings[] = {
    // row 0:  ` 1 2 3 4 5 6 7 8 9 0 - = del
    {SDL_SCANCODE_GRAVE, kKBacktick},
    {SDL_SCANCODE_1, 1}, {SDL_SCANCODE_2, 2}, {SDL_SCANCODE_3, 3}, {SDL_SCANCODE_4, 4}, {SDL_SCANCODE_5, 5},
    {SDL_SCANCODE_6, 6}, {SDL_SCANCODE_7, 7}, {SDL_SCANCODE_8, 8}, {SDL_SCANCODE_9, kK9}, {SDL_SCANCODE_0, kK0},
    {SDL_SCANCODE_MINUS, kKMinus}, {SDL_SCANCODE_EQUALS, kKEquals}, {SDL_SCANCODE_BACKSPACE, kKDel},
    // row 1:  tab q w e r t y u i o p [ ] backslash
    {SDL_SCANCODE_TAB, kKTab},
    {SDL_SCANCODE_Q, 15}, {SDL_SCANCODE_W, 16}, {SDL_SCANCODE_E, 17}, {SDL_SCANCODE_R, 18}, {SDL_SCANCODE_T, 19},
    {SDL_SCANCODE_Y, 20}, {SDL_SCANCODE_U, 21}, {SDL_SCANCODE_I, 22}, {SDL_SCANCODE_O, kKO}, {SDL_SCANCODE_P, kKP},
    {SDL_SCANCODE_LEFTBRACKET, kKLBracket}, {SDL_SCANCODE_RIGHTBRACKET, kKRBracket},
    {SDL_SCANCODE_BACKSLASH, kKBackslash},
    // row 2:  fn shift a s d f g h j k l ; ' enter
    {SDL_SCANCODE_RSHIFT, kKFn}, {SDL_SCANCODE_LSHIFT, kKShift},
    {SDL_SCANCODE_A, 30}, {SDL_SCANCODE_S, 31}, {SDL_SCANCODE_D, 32}, {SDL_SCANCODE_F, 33}, {SDL_SCANCODE_G, 34},
    {SDL_SCANCODE_H, 35}, {SDL_SCANCODE_J, 36}, {SDL_SCANCODE_K, 37}, {SDL_SCANCODE_L, kKL},
    {SDL_SCANCODE_SEMICOLON, kKSemicolon}, {SDL_SCANCODE_APOSTROPHE, kKQuote}, {SDL_SCANCODE_RETURN, kKEnter},
    // row 3:  ctrl opt alt z x c v b n m , . / space
    {SDL_SCANCODE_LCTRL, kKCtrl}, {SDL_SCANCODE_LALT, kKOpt}, {SDL_SCANCODE_RALT, kKAlt},
    {SDL_SCANCODE_Z, 45}, {SDL_SCANCODE_X, 46}, {SDL_SCANCODE_C, 47}, {SDL_SCANCODE_V, 48}, {SDL_SCANCODE_B, 49},
    {SDL_SCANCODE_N, 50}, {SDL_SCANCODE_M, 51}, {SDL_SCANCODE_COMMA, 52}, {SDL_SCANCODE_PERIOD, kKDot},
    {SDL_SCANCODE_SLASH, kKSlash}, {SDL_SCANCODE_SPACE, kKSpace},
    // the side button and the desktop's own keys
    {SDL_SCANCODE_F1, kKG0},
    {SDL_SCANCODE_ESCAPE, kKG0},   // G0 without fn on Apple keyboards (F-keys are media keys there)
    {SDL_SCANCODE_F9, kActRecord},
    {SDL_SCANCODE_F2, kActStats},
    // Second keys for the same Cardputer key (after the canonical ones, so the
    // script player presses the canonical key). Right Control where a keyboard
    // has one; the keypad Enter (fn+Return on a MacBook); ISO keyboards: the
    // key left of Z and, on some layouts, the key left of Return (macOS swaps
    // the ISO section key and ` on some models, so both read as `).
    {SDL_SCANCODE_RCTRL, kKCtrl},
    {SDL_SCANCODE_KP_ENTER, kKEnter},
    {SDL_SCANCODE_NONUSBACKSLASH, kKBacktick},
    {SDL_SCANCODE_NONUSHASH, kKBackslash},
};

bool ieq(const char* a, const char* b) {
  for (; *a && *b; ++a, ++b)
    if (std::tolower(static_cast<unsigned char>(*a)) != std::tolower(static_cast<unsigned char>(*b))) return false;
  return *a == *b;
}

std::string trim(const std::string& s) {
  size_t a = 0, z = s.size();
  while (a < z && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
  while (z > a && std::isspace(static_cast<unsigned char>(s[z - 1]))) --z;
  return s.substr(a, z - a);
}

// SDL_GetScancodeFromName, case-insensitive (it already is in SDL 2), plus a
// fallback over every scancode's name so "left shift" and "LEFT SHIFT" both work.
int scancodeFromName(const std::string& name) {
  const SDL_Scancode sc = SDL_GetScancodeFromName(name.c_str());
  if (sc != SDL_SCANCODE_UNKNOWN) return sc;
  for (int i = 1; i < KeyMap::kScancodes; ++i) {
    const char* n = SDL_GetScancodeName(static_cast<SDL_Scancode>(i));
    if (n && *n && ieq(n, name.c_str())) return i;
  }
  return -1;
}

}  // namespace

// Command shortcuts (the Mac idiom for app commands, and they need no fn).
// Safe: pressing Command has already released every instrument key, and a
// Command shortcut never reaches the instrument, on its down or its up.
const Bind kDefaultCmdBindings[] = {
    {SDL_SCANCODE_R, kActRecord},   // Cmd+R = F9
    {SDL_SCANCODE_I, kActStats},    // Cmd+I = F2
    {SDL_SCANCODE_L, kActListen},   // Cmd+L: Listen
    {SDL_SCANCODE_B, kActStems},    // Cmd+B: Stems
};

KeyMap::KeyMap() {
  clear();
  for (const Bind& b : kDefaultBindings) bind[b.sc] = b.b;
  for (const Bind& b : kDefaultCmdBindings) cmdBind[b.sc] = b.b;
}

void KeyMap::clear() {
  for (int16_t& b : bind) b = kUnbound;
  for (int16_t& b : cmdBind) b = kUnbound;
}

int KeyMap::scancodeFor(int b) const {
  // Prefer the default table's order (canonical first), then any other binding.
  for (const Bind& d : kDefaultBindings)
    if (bind[d.sc] == b) return d.sc;
  for (int i = 0; i < kScancodes; ++i)
    if (bind[i] == b) return i;
  return -1;
}

const char* bindingName(int b) {
  if (b == kActRecord) return "record";
  if (b == kActStats) return "stats";
  if (b == kActListen) return "listen";
  if (b == kActStems) return "stems";
  if (b >= 0 && b < kNumKeyIds) return keyLegend(b);
  return "none";
}

int bindingFromName(const char* name) {
  if (ieq(name, "record")) return kActRecord;
  if (ieq(name, "stats")) return kActStats;
  if (ieq(name, "listen")) return kActListen;
  if (ieq(name, "stems")) return kActStems;
  if (ieq(name, "none")) return kUnbound;
  const int k = keyFromName(name);
  return k >= 0 ? k : -2;
}

bool KeyMap::parse(const char* text, std::string* err) {
  int lineNo = 0;
  const char* p = text;
  while (*p) {
    const char* e = std::strchr(p, '\n');
    std::string line = e ? std::string(p, size_t(e - p)) : std::string(p);
    p = e ? e + 1 : p + line.size();
    ++lineNo;
    // A comment is a '#' at the start of the line or after whitespace (so "#"
    // is not special inside a name; no key name contains one anyway).
    for (size_t i = 0; i < line.size(); ++i)
      if (line[i] == '#' && (i == 0 || std::isspace(static_cast<unsigned char>(line[i - 1])))) {
        line.resize(i);
        break;
      }
    line = trim(line);
    if (line.empty()) continue;
    // "<key> = <scancode name>": the key is the first token, so "=" and "-" work as keys.
    size_t sp = 0;
    while (sp < line.size() && !std::isspace(static_cast<unsigned char>(line[sp]))) ++sp;
    const std::string key = line.substr(0, sp);
    std::string rest = trim(line.substr(sp));
    if (rest.empty() || rest[0] != '=') {
      if (err) *err = "line " + std::to_string(lineNo) + ": expected '<cardputer key> = <scancode name>'";
      return false;
    }
    rest = trim(rest.substr(1));
    const int b = bindingFromName(key.c_str());
    if (b == -2) {
      if (err) *err = "line " + std::to_string(lineNo) + ": unknown Cardputer key '" + key + "'";
      return false;
    }
    bool cmd = false;
    if (rest.size() > 4 && (rest[0] == 'C' || rest[0] == 'c') && (rest[1] == 'm' || rest[1] == 'M') &&
        (rest[2] == 'd' || rest[2] == 'D') && rest[3] == '+') {
      cmd = true;
      rest = trim(rest.substr(4));
      if (b >= 0 && b < kNumKeyIds) {
        if (err) *err = "line " + std::to_string(lineNo) + ": Command shortcuts are for record / stats, not keys";
        return false;
      }
    }
    const int sc = rest.empty() ? -1 : scancodeFromName(rest);
    if (sc < 0) {
      if (err) *err = "line " + std::to_string(lineNo) + ": unknown scancode name '" + rest + "'";
      return false;
    }
    (cmd ? cmdBind : bind)[sc] = int16_t(b);
  }
  return true;
}

bool KeyMap::loadFile(const char* path, std::string* err) {
  FILE* f = std::fopen(path, "rb");
  if (!f) {
    if (err) *err = std::string("cannot open ") + path;
    return false;
  }
  std::string text;
  char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
  std::fclose(f);
  return parse(text.c_str(), err);
}

}  // namespace desk
