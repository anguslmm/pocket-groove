// A debounced push button as key events (stage 1: G0 on a GPIO, as on the Cardputer:
// held = tilt, + '-' / '=' = output level). A change counts once the pin has read
// the new level kStable times in a row, kPollMs apart (the Cardputer's 3 x 5 ms).
// The input layer stays general: buttons, pads and touch all become events into the
// core (pg::KeyEvent today). Portable, header-only (host/daisy/test_daisy_core.cpp).
#pragma once
#include <cstdint>

#include "pg/platform.h"

namespace pgd {

class Button {
 public:
  static constexpr uint32_t kPollMs = 5;
  static constexpr int kStable = 3;
  explicit Button(uint8_t key = pg::kKeyG0) : key_(key) {}
  // pressed: the pin's level now (true = pressed). Returns true and fills *e on a change.
  bool poll(bool pressed, uint32_t nowMs, pg::KeyEvent* e) {
    if (nowMs - last_ < kPollMs && primed_) return false;
    last_ = nowMs;
    primed_ = true;
    if (pressed == down_) {
      count_ = 0;
      return false;
    }
    if (++count_ < kStable) return false;
    count_ = 0;
    down_ = pressed;
    e->key = key_;
    e->down = pressed;
    e->timeUs = nowMs * 1000u;
    return true;
  }
  bool down() const { return down_; }

 private:
  uint8_t key_;
  bool down_ = false, primed_ = false;
  int count_ = 0;
  uint32_t last_ = 0;
};

}  // namespace pgd
