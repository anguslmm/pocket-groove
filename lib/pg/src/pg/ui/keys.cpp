#include "keys.h"

namespace pg {
namespace ui {

namespace {
const char* const kLegend[kNumKeys] = {
    "`",   "1",   "2",   "3", "4", "5", "6", "7", "8", "9", "0", "-", "=",  "del",
    "tab", "q",   "w",   "e", "r", "t", "y", "u", "i", "o", "p", "[", "]",  "\\",
    "fn",  "sft", "a",   "s", "d", "f", "g", "h", "j", "k", "l", ";", "'",  "ent",
    "ctl", "opt", "alt", "z", "x", "c", "v", "b", "n", "m", ",", ".", "/",  "spc",
};
struct Alias { const char* name; uint8_t key; };
const Alias kAliases[] = {
    {"backtick", 0}, {"home", 0}, {"esc", 0}, {"minus", 11}, {"equals", 12}, {"plus", 12},
    {"backspace", 13}, {"delete", 13}, {"lbracket", 25}, {"rbracket", 26}, {"backslash", 27},
    {"shift", 29}, {"semicolon", 39}, {"quote", 40}, {"enter", 41}, {"ctrl", 42}, {"comma", 52},
    {"dot", 53}, {"period", 53}, {"slash", 54}, {"space", 55}, {"g0", 56},
};
bool eq(const char* a, const char* b) {
  for (; *a && *b; ++a, ++b) {
    char x = *a, y = *b;
    if (x >= 'A' && x <= 'Z') x = char(x + 32);
    if (y >= 'A' && y <= 'Z') y = char(y + 32);
    if (x != y) return false;
  }
  return *a == *b;
}
}  // namespace

const char* keyLegend(int k) {
  if (k >= 0 && k < kNumKeys) return kLegend[k];
  if (k == kKG0) return "G0";
  return "?";
}

int keyFromName(const char* name) {
  for (int k = 0; k < kNumKeys; ++k)
    if (eq(name, kLegend[k])) return k;
  for (const Alias& a : kAliases)
    if (eq(name, a.name)) return a.key;
  return -1;
}

}  // namespace ui
}  // namespace pg
