// Four pots onto the selected track's four macros (stage 0 on the breadboard: the
// owner's pots on four ADC pins; no UI owns them yet). Pickup: after the track
// changes (or at boot, or when the value moved by other means: keys, motion, a
// project load) a pot does nothing until it is turned past the macro's current
// value, so it never makes the sound jump. Turned, it follows: the macro gets
// the pot's position (0..255). A dead band (kDeadband) keeps ADC noise from
// sending a stream of tiny changes. Switchable off ('p' on the serial port).
// Portable (host/daisy/test_daisy_core.cpp): the reads and the macro access are
// passed in.
#pragma once
#include <cstdint>

namespace pgd {

class PotMacros {
 public:
  static constexpr int kPots = 4;
  static constexpr int kDeadband = 2;   // of 255
  static constexpr int kCatch = 3;      // "crossed": within this of the value counts too

  // raw: 0..1 per pot (< 0: no reading). track: the selected one. current(m): the
  // macro's value now (0..255; < 0: the track has no instrument, nothing to do).
  // set(m, v): move macro m to v. Returns the number of macros moved.
  using Current = int (*)(void* user, int macro);
  using Set = void (*)(void* user, int macro, int value);
  int update(const float raw[kPots], int track, Current current, Set set, void* user);

  void setEnabled(bool on) {
    on_ = on;
    track_ = -1;
  }
  bool enabled() const { return on_; }
  bool engaged(int p) const { return engaged_[p]; }
  int position(int p) const { return pos_[p]; }   // last pot position 0..255, -1 none
  uint32_t moves() const { return moves_; }

 private:
  bool on_ = true;
  int track_ = -1;
  bool engaged_[kPots] = {};
  int pos_[kPots] = {-1, -1, -1, -1};
  int sent_[kPots] = {-1, -1, -1, -1};   // what we last set (to notice other changes)
  int prevSide_[kPots] = {};             // sign of (pot - value) when not engaged
  uint32_t moves_ = 0;
};

}  // namespace pgd
