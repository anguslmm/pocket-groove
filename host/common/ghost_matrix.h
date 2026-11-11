// Host model of the Cardputer-Adv key matrix WITHOUT diodes, for the UI tests.
// Takes intended physical presses and releases and returns the key events the
// TCA8418 would report: the key itself plus every phantom (a key whose scan row
// and column are connected through closed keys), downs and ups, in the same
// batch with the same timestamp. A physical press of a key that already reads
// closed (a phantom position) produces no event, and a physical release of a key
// that stays closed through a loop produces none either, exactly as on the device.
//
// Written independently of pg::ui::ghostClosure (a flood fill over the scan
// lines here, union-find there), with the scan coordinates taken from the
// TCA8418 remap in docs/hardware-notes.md 4.5 (tcaRow = x / 2,
// tcaCol = y + 4 * (x & 1)).
#pragma once
#include <cstdint>

#include "pg/platform.h"

namespace uih {

// Where the phantom events sit in their batch: the hardware gives no guarantee.
enum class GhostOrder : uint8_t { Off, PhantomLast, PhantomFirst };

struct GhostMatrix {
  GhostOrder order = GhostOrder::PhantomLast;
  uint64_t phys = 0;     // physically held
  uint64_t closed = 0;   // what the controller reports as held
  uint32_t phantomDowns = 0, phantomUps = 0;

  static int tcaRow(int k) { return (k % pg::kKeyCols) / 2; }
  static int tcaCol(int k) { return (k / pg::kKeyCols) + 4 * ((k % pg::kKeyCols) & 1); }

  static uint64_t closure(uint64_t p) {
    // Flood-fill the scan lines (rows 0..6 = nodes 0..6, columns 0..7 = nodes 7..14)
    // through the pressed keys; a key reads closed if its row and column meet.
    int comp[15];
    for (int i = 0; i < 15; ++i) comp[i] = -1;
    for (int start = 0; start < 15; ++start) {
      if (comp[start] >= 0) continue;
      int stack[15], sp = 0;
      stack[sp++] = start;
      comp[start] = start;
      while (sp) {
        const int node = stack[--sp];
        for (int k = 0; k < pg::kNumKeys; ++k) {
          if (!((p >> k) & 1)) continue;
          const int r = tcaRow(k), c = 7 + tcaCol(k);
          const int other = node == r ? c : node == c ? r : -1;
          if (other >= 0 && comp[other] < 0) { comp[other] = start; stack[sp++] = other; }
        }
      }
    }
    uint64_t out = p;
    for (int k = 0; k < pg::kNumKeys; ++k)
      if (comp[tcaRow(k)] == comp[7 + tcaCol(k)]) out |= 1ull << k;
    return out;
  }

  void reset() { phys = closed = 0; }

  // Physical change of key k (a matrix key). Writes the reported events to out
  // (room for kNumKeys) and returns their count.
  int change(int k, bool down, uint32_t timeUs, pg::KeyEvent* out) {
    const uint64_t b = 1ull << k;
    if (order == GhostOrder::Off) {
      if (((phys & b) != 0) == down) return 0;
      phys = down ? (phys | b) : (phys & ~b);
      closed = phys;
      out[0] = pg::KeyEvent{uint8_t(k), down, timeUs};
      return 1;
    }
    phys = down ? (phys | b) : (phys & ~b);
    const uint64_t now = closure(phys);
    const uint64_t diff = down ? (now & ~closed) : (closed & ~now);
    closed = now;
    int n = 0;
    auto emit = [&](uint64_t m) {
      for (int i = 0; i < pg::kNumKeys; ++i)
        if ((m >> i) & 1) out[n++] = pg::KeyEvent{uint8_t(i), down, timeUs};
    };
    const uint64_t self = diff & b, ghosts = diff & ~b;
    if (down) phantomDowns += uint32_t(__builtin_popcountll(ghosts));
    else phantomUps += uint32_t(__builtin_popcountll(ghosts));
    if (order == GhostOrder::PhantomFirst) { emit(ghosts); emit(self); }
    else { emit(self); emit(ghosts); }
    return n;
  }
};

inline const char* ghostOrderName(GhostOrder o) {
  return o == GhostOrder::Off ? "no ghosts" : o == GhostOrder::PhantomLast ? "phantom last" : "phantom first";
}

}  // namespace uih
