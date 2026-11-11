// Pocket Groove platform layer -- keyboard (TCA8418 keypad controller on the
// internal I2C bus, address 0x34, INT on GPIO11) and the G0 button.
//
// A small task drains the controller's event FIFO whenever INT goes low and
// turns the raw codes into pg::KeyEvent (down AND up, modifiers included). It
// does not depend on the UI loop rate: a slow frame delays delivery to the UI
// but loses nothing (64-entry queue behind the chip's own 10-entry FIFO).
//
// Key ids follow lib/pg/src/pg/platform.h: id = row * 14 + col, row 0 = number
// row, col 0 = leftmost key. Mapping verified against M5Cardputer 1.1.1
// (utility/Keyboard/KeyboardReader/TCA8418.cpp remap() and Keyboard.h
// _key_value_map): raw code c -> k = (c & 0x7F) - 1, tcaRow = k / 10,
// tcaCol = k % 10, col = tcaRow * 2 + (tcaCol > 3), row = tcaCol % 4,
// bit 7 set = pressed.
//
// No Arduino / M5 / ESP-IDF types in this header.
#pragma once
#include <cstdint>

#include "pg/platform.h"

namespace plat {
namespace keys {

// The G0 button (GPIO0, active low) is not part of the matrix. It is reported
// with this id, one past the last matrix key. (Proposed addition to platform.h.)
constexpr uint8_t kKeyG0   = pg::kNumKeys;       // 56
constexpr int     kNumIds  = pg::kNumKeys + 1;   // 57

struct Event {
  pg::KeyEvent ev;
  uint8_t      raw;       // raw TCA8418 event code (0 for G0)
  uint32_t     tIntUs;    // micros() (low 32 bits) when INT fell for the batch this event came in
  uint32_t     tReadUs;   // micros() when the event was read from the chip
};

struct Stats {
  bool     controllerPresent;   // TCA8418 answered at begin()
  uint32_t events;              // key events produced (matrix + G0)
  uint32_t queueDrops;          // events lost because nobody called poll() (queue full)
  uint32_t unknownCodes;        // raw codes outside the 7x8 matrix (GPI events etc.)
  uint32_t fifoOverflows;       // the chip's 10-event FIFO overflowed
  uint32_t i2cErrors;
  uint32_t lastIntToReadUs;     // INT falling edge -> event read over I2C
  uint32_t maxIntToReadUs;
  uint8_t  lastRaw;
};

// Starts the reader task. Returns false if the TCA8418 does not answer; the
// G0 button still works then.
bool begin();

// Next event, non-blocking. UI thread.
bool poll(Event* out);

// Keys currently held as seen by the reader task (bit n = key id n, bit 56 = G0).
uint64_t heldMask();

// Optional: called on the reader task for every event, before it is queued.
// Keep it tiny (set a flag, push to a lock-free queue). This is the low-latency
// path for note triggers: it does not wait for the UI loop.
void setListener(void (*fn)(const Event& e, void* user), void* user);

Stats stats();

// Geometry helpers shared with the UI.
inline uint8_t rowOf(uint8_t key) { return static_cast<uint8_t>(key / pg::kKeyCols); }
inline uint8_t colOf(uint8_t key) { return static_cast<uint8_t>(key % pg::kKeyCols); }
// Controller matrix coordinates of a key (7 rows x 8 columns): keys sharing a
// tcaRow or tcaCol share a scan line, which is what decides ghosting.
inline uint8_t tcaRowOf(uint8_t key) { return static_cast<uint8_t>(colOf(key) / 2); }
inline uint8_t tcaColOf(uint8_t key) { return static_cast<uint8_t>(rowOf(key) + 4 * (colOf(key) & 1)); }
inline uint8_t keyFromTca(uint8_t tcaRow, uint8_t tcaCol) {
  return static_cast<uint8_t>((tcaCol & 3) * pg::kKeyCols + tcaRow * 2 + (tcaCol > 3 ? 1 : 0));
}

// Short keycap legend ("q", "ent", "fn", "G0"...), never null.
const char* legend(uint8_t key);

}  // namespace keys
}  // namespace plat
