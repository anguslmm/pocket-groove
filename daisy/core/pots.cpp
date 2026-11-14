#include "pots.h"

namespace pgd {

namespace {
int sign(int v) { return v > 0 ? 1 : (v < 0 ? -1 : 0); }
}  // namespace

int PotMacros::update(const float raw[kPots], int track, Current current, Set set, void* user) {
  if (!on_) return 0;
  if (track != track_) {
    track_ = track;
    for (int p = 0; p < kPots; ++p) {
      engaged_[p] = false;
      sent_[p] = -1;
      prevSide_[p] = 0;
    }
  }
  int moved = 0;
  for (int p = 0; p < kPots; ++p) {
    if (raw[p] < 0.0f) {
      pos_[p] = -1;
      continue;
    }
    const float r = raw[p] > 1.0f ? 1.0f : raw[p];
    const int v = static_cast<int>(r * 255.0f + 0.5f);
    const int cur = current(user, p);
    if (cur < 0) {
      engaged_[p] = false;
      pos_[p] = v;
      continue;
    }
    // Moved by something else (a key, motion, a load): pick up again.
    if (engaged_[p] && sent_[p] >= 0 && cur != sent_[p]) {
      engaged_[p] = false;
      prevSide_[p] = 0;
    }
    if (!engaged_[p]) {
      const int d = v - cur;
      const int s = sign(d);
      if ((d < 0 ? -d : d) <= kCatch || (prevSide_[p] != 0 && s != 0 && s != prevSide_[p])) engaged_[p] = true;
      else prevSide_[p] = s;
      pos_[p] = v;
      if (!engaged_[p]) continue;
      if (v == cur) {
        sent_[p] = cur;
        continue;
      }
    } else {
      const int d = v - pos_[p];
      if ((d < 0 ? -d : d) < kDeadband && v != 0 && v != 255) continue;
      pos_[p] = v;
      if (v == cur) continue;
    }
    pos_[p] = v;
    set(user, p, v);
    sent_[p] = v;
    ++moves_;
    ++moved;
  }
  return moved;
}

}  // namespace pgd
