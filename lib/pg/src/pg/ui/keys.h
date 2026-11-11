// Key ids (pg::Key::id() = row*14 + col) of the Cardputer keyboard, named for the UI.
#pragma once
#include <cstdint>
#include "../platform.h"

namespace pg {
namespace ui {

enum KeyId : uint8_t {
  kKBacktick = 0, kKStep1 = 1,   // 1..8 = steps 1-8
  kK9 = 9, kK0 = 10, kKMinus = 11, kKEquals = 12, kKDel = 13,
  kKTab = 14, kKStep9 = 15,      // q..i = steps 9-16 (15..22)
  kKO = 23, kKP = 24, kKLBracket = 25, kKRBracket = 26, kKBackslash = 27,
  kKFn = 28, kKShift = 29, kKPad0 = 30,   // a..k = pads (30..37)
  kKL = 38, kKSemicolon = 39, kKQuote = 40, kKEnter = 41,
  kKCtrl = 42, kKOpt = 43, kKAlt = 44, kKNote0 = 45,   // z.. , = notes (45..52)
  kKDot = 53, kKSlash = 54, kKSpace = 55,
  kKG0 = 56,
  kNumKeyIds = 57,
};

// Step key index 0..15 for a key id, or -1.
inline int stepKeyIndex(int k) {
  if (k >= 1 && k <= 8) return k - 1;
  if (k >= 15 && k <= 22) return k - 15 + 8;
  return -1;
}
inline int padKeyIndex(int k) { return (k >= kKPad0 && k < kKPad0 + 8) ? k - kKPad0 : -1; }
inline int noteKeyIndex(int k) { return (k >= kKNote0 && k < kKNote0 + 8) ? k - kKNote0 : -1; }
inline bool isModifierKey(int k) {
  return k == kKFn || k == kKShift || k == kKCtrl || k == kKOpt || k == kKAlt;
}

// Lower-case legend of a key ("`", "1", "del", "tab", "q", "fn", "sft", "ent", "spc", "G0").
const char* keyLegend(int k);
// Inverse of keyLegend, also accepting aliases (space, enter, shift, backspace, ...). -1 if unknown.
int keyFromName(const char* name);

}  // namespace ui
}  // namespace pg
