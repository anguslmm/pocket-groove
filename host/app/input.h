// The desktop's input layer.
//
// KeyboardInput turns scancode events into Cardputer key downs/ups (pure
// logic, unit-tested in tests/test_desktop_keymap.cpp):
//   - OS key repeat is ignored (the UI does its own repeat, as on the device);
//   - two scancodes bound to one Cardputer key (left/right Control) count as
//     one key: down on the first press, up on the last release;
//   - while Command is held nothing new goes down (only the Command
//     shortcuts, Cmd+R / Cmd+I, fire their app action), and pressing Command
//     releases every held key: Cmd+Q / Cmd+Tab never play a note, and the
//     key-ups that macOS swallows while Command is down cannot leave a key
//     stuck;
//   - releaseAll() (focus loss, quit) sends an up for every held key.
//
// Everything that reaches the Ui goes through one Sink on the main thread.
// A later MIDI source (a CoreMIDI thread) will post into EventQueue, a
// lock-free single-producer queue the main loop drains before each frame;
// the Note kind is reserved for it and is not routed anywhere yet.
#pragma once
#include <atomic>
#include <cstdint>

#include "keymap.h"
#include "pg/ui/keys.h"

namespace desk {

struct KeyOut {
  int16_t binding;   // a Cardputer key id (0..56) or an action (kActRecord, ...)
  bool down;
};

class KeyboardInput {
 public:
  static constexpr int kMaxOut = pg::ui::kNumKeyIds + 4;

  explicit KeyboardInput(const KeyMap* map) : map_(map) {}

  // One SDL key event. commandHeld = KMOD_GUI was set on it; isCommand = the
  // scancode itself is a Command key. Writes 0..kMaxOut events; returns the count.
  int onScancode(int scancode, bool down, bool repeat, bool commandHeld, bool isCommand, KeyOut* out);
  // An up for every held key, lowest id first.
  int releaseAll(KeyOut* out);

  uint64_t heldKeys() const { return held_; }   // bit = Cardputer key id (0..56)
  bool anyHeld() const { return held_ != 0; }

 private:
  const KeyMap* map_;
  uint64_t held_ = 0;                 // Cardputer keys down
  uint8_t count_[pg::ui::kNumKeyIds] = {};   // scancodes holding each key down
  uint8_t scDown_[KeyMap::kScancodes / 8] = {};   // scancodes we counted as down
  bool scIsDown(int sc) const { return (scDown_[sc >> 3] >> (sc & 7)) & 1; }
  void setSc(int sc, bool on) {
    if (on) scDown_[sc >> 3] = uint8_t(scDown_[sc >> 3] | (1u << (sc & 7)));
    else scDown_[sc >> 3] = uint8_t(scDown_[sc >> 3] & ~(1u << (sc & 7)));
  }
};

// Events from sources other than the computer keyboard (MIDI, later).
struct InputEvent {
  enum Kind : uint8_t { Key, Note } kind;
  uint8_t a;      // Key: Cardputer key id; Note: MIDI note
  uint8_t b;      // Note: velocity (0 = off)
  bool down;      // Key
};

// Single producer (one source thread), single consumer (the main loop).
class EventQueue {
 public:
  bool post(const InputEvent& e) {
    const uint32_t w = w_.load(std::memory_order_relaxed);
    if (w - r_.load(std::memory_order_acquire) >= kSize) return false;
    q_[w % kSize] = e;
    w_.store(w + 1, std::memory_order_release);
    return true;
  }
  bool pop(InputEvent* e) {
    const uint32_t r = r_.load(std::memory_order_relaxed);
    if (r == w_.load(std::memory_order_acquire)) return false;
    *e = q_[r % kSize];
    r_.store(r + 1, std::memory_order_release);
    return true;
  }

 private:
  static constexpr uint32_t kSize = 256;
  InputEvent q_[kSize] = {};
  std::atomic<uint32_t> w_{0}, r_{0};
};

}  // namespace desk
