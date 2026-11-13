#include "input.h"

namespace desk {

int KeyboardInput::onScancode(int sc, bool down, bool repeat, bool commandHeld, bool isCommand, KeyOut* out) {
  if (sc <= 0 || sc >= KeyMap::kScancodes) return 0;
  if (isCommand) return down ? releaseAll(out) : 0;
  if (down) {
    if (commandHeld) {   // a Command shortcut (app action) or nothing; never an instrument key
      const int a = map_->lookupCmd(sc);
      if (a < kActRecord || (repeat && !actionRepeats(a))) return 0;
      out[0] = KeyOut{int16_t(a), true};
      return 1;
    }
    if (repeat || scIsDown(sc)) return 0;
    const int b = map_->lookup(sc);
    if (b == kUnbound) return 0;
    if (b >= pg::ui::kNumKeyIds) {          // an action: fires on the press, no up
      out[0] = KeyOut{int16_t(b), true};
      return 1;
    }
    setSc(sc, true);
    if (count_[b]++ > 0) return 0;          // the other scancode of this key is already down
    held_ |= 1ull << b;
    out[0] = KeyOut{int16_t(b), true};
    return 1;
  }
  if (!scIsDown(sc)) return 0;              // never counted (repeat, Command, action, released)
  setSc(sc, false);
  const int b = map_->lookup(sc);
  if (b < 0 || b >= pg::ui::kNumKeyIds || count_[b] == 0) return 0;   // the map changed underneath: ignore
  if (--count_[b] > 0) return 0;
  held_ &= ~(1ull << b);
  out[0] = KeyOut{int16_t(b), false};
  return 1;
}

int KeyboardInput::releaseAll(KeyOut* out) {
  int n = 0;
  for (int k = 0; k < pg::ui::kNumKeyIds; ++k)
    if ((held_ >> k) & 1) out[n++] = KeyOut{int16_t(k), false};
  held_ = 0;
  for (uint8_t& c : count_) c = 0;
  for (uint8_t& s : scDown_) s = 0;
  return n;
}

}  // namespace desk
