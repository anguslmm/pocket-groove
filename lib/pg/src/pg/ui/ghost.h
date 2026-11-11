// Phantom-key ("ghost") filter for the Cardputer-Adv keyboard (UX.md 16.1,
// docs/ghosting-audit.md). Portable: the Ui runs every key event through it.
//
// The keyboard is a TCA8418 scanning a 7 x 8 matrix with no diodes. When three
// closed keys sit on three corners of a rectangle of that SCAN matrix (two share
// a scan row, two share a scan column), current flows round the loop and the
// controller reports the fourth corner closed too: a phantom down, in the same
// FIFO batch as the third real key (so with the same KeyEvent::timeUs), in no
// guaranteed order. Releasing any of the three real keys breaks the loop and the
// controller reports the phantom up. More generally a key is reported closed
// whenever its scan row and column are connected through closed keys.
//
// Scan coordinates are NOT the printed 4 x 14 layout: scanRow = col / 2 and
// scanCol = row + 4 * (col & 1) (M5Cardputer's TCA8418 remap, inverted). So
// a + s + d (one printed row) ghosts f, and q + w + e ghosts tab.
//
// What the filter does, per key-down:
//  1. If its scan row and column are already connected through settled closed
//     keys, it is a phantom: dropped, and its key-up is swallowed later.
//  2. If it would close a loop (some open key would become a ghost), its
//     partner is expected in the same batch. The four corners are electrically
//     interchangeable, so the two new downs can't be told apart by the hardware.
//     The down is held back until the partner arrives (normally microseconds
//     later), then the pair is resolved: the one that, with the keys already
//     held, forms a known gesture (the table in ghost.cpp) is real and the other
//     a phantom; if both or neither do, both are dropped. If no partner arrives
//     within kWindowUs (event time) or kFlushMs (UI clock, via tick()), the down
//     is delivered as real.
//  3. Otherwise it is delivered at once. Order is kept: while a down is held
//     back, later events queue behind it.
// A key-up of a dropped key is swallowed; any key-up while downs are held back
// first delivers those as real. A down for a key thought to be a phantom means
// its up was lost: it is treated as a fresh down, so no key can stay stuck.
// No heap, a few hundred bytes of state.
#pragma once
#include <cstdint>

#include "../platform.h"

namespace pg {
namespace ui {

// Scan-matrix coordinates of a matrix key id (0..55).
constexpr int kScanRows = 7, kScanCols = 8;
inline int scanRow(int k) { return (k % kKeyCols) / 2; }
inline int scanCol(int k) { return k / kKeyCols + 4 * ((k % kKeyCols) & 1); }
inline int keyAtScan(int r, int c) { return (c & 3) * kKeyCols + r * 2 + (c > 3 ? 1 : 0); }

// Keys the controller reports closed while the keys in `pressed` are physically
// down (bit n = key id n, matrix keys only): pressed plus every phantom.
uint64_t ghostClosure(uint64_t pressed);
// True if key k's scan row and column are connected through the keys in `closed`
// (k itself excluded), i.e. k would read as pressed even if it is not.
bool ghostCompletes(int k, uint64_t closed);
// True if key k, pressed last, together with two keys of `held` forms one of the
// known three-key gestures (ghost.cpp). Used to break phantom ties.
bool knownGesture(uint64_t held, int k);

class GhostFilter {
 public:
  static constexpr uint32_t kWindowUs = 3000;   // "same batch": phantom and real key share the FIFO read
  static constexpr uint32_t kFlushMs = 4;
  static constexpr int kQueue = 8;
  static constexpr int kMaxOut = kQueue + 1;
  struct Out {
    KeyEvent ev;
    uint32_t ms;   // the UI clock the event arrived with
  };

  void reset();
  // One event in, 0..kMaxOut events out (to deliver in order). nowMs = UI clock.
  int push(const KeyEvent& e, uint32_t nowMs, Out* out);
  // Delivers held-back downs whose partner never came. Call once per UI frame.
  int tick(uint32_t nowMs, Out* out);

  bool holding() const { return qn_ != 0; }
  uint64_t realMask() const { return real_; }        // delivered downs not yet released
  uint64_t phantomMask() const { return phantom_; }  // dropped downs whose up is still to come
  uint32_t droppedDowns() const { return dropped_; }
  uint32_t swallowedUps() const { return swallowed_; }

 private:
  enum : uint8_t { kUndecided = 0, kReal, kDrop };
  struct Entry {
    KeyEvent ev;
    uint32_t ms;
    uint64_t partners;   // keys that become ghosts if this one is real
    uint8_t state;
  };
  uint64_t queueMask() const;
  uint64_t heldForMatch(int exceptA, int exceptB) const;
  int flushAll(Out* out);       // undecided -> real, then release
  int release(Out* out);        // emits the queue if nothing is undecided
  void drop(int k);

  uint64_t real_ = 0, phantom_ = 0;
  Entry q_[kQueue] = {};
  uint8_t qn_ = 0;
  uint32_t dropped_ = 0, swallowed_ = 0;
};

}  // namespace ui
}  // namespace pg
