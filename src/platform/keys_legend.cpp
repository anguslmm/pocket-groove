// Key legends. Portable (no hardware includes) so the desktop mock can reuse it.
#include "keys.h"

namespace plat {
namespace keys {

const char* legend(uint8_t key) {
  // Same order as platform.h's diagram and M5Cardputer's _key_value_map.
  static const char* const kLegend[pg::kNumKeys] = {
    "`",   "1",   "2",   "3", "4", "5", "6", "7", "8", "9", "0", "-", "=",  "del",
    "tab", "q",   "w",   "e", "r", "t", "y", "u", "i", "o", "p", "[", "]",  "\\",
    "fn",  "sft", "a",   "s", "d", "f", "g", "h", "j", "k", "l", ";", "'",  "ent",
    "ctl", "opt", "alt", "z", "x", "c", "v", "b", "n", "m", ",", ".", "/",  "spc",
  };
  if (key < pg::kNumKeys) return kLegend[key];
  if (key == kKeyG0) return "G0";
  return "?";
}

}  // namespace keys
}  // namespace plat
