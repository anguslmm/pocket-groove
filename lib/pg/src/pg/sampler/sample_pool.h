// The sample pool the sampler voice reads through (docs/STATE.md stage F1).
//
// Twelve user slots (sample_format.h). The platform provides them: memory-mapped
// flash on the device (src/platform/sample_flash.*), RAM on the desktop and in the
// tests (MemoryPool, below). An EMPTY user slot plays a built-in sound: ROM sample
// (slot mod kRomCount, rom_samples.h), so the instrument makes sound out of the box.
//
// THREADING. The audio thread resolves a slot only at a note-on (one virtual call)
// and keeps the pointer for that chop; it checks generation() once per quantum and
// stops a chop whose pool changed under it. A writer (the desktop's reload, the
// device's transfer) changes a slot's content and then bumps the generation, so a
// sounding chop never reads a slot mid-change for more than a quantum, and never
// outside the memory it resolved (the pointers stay valid: the desktop's buffers are
// allocated once at full slot size, the device's mapping stays put).
#pragma once
#include <atomic>
#include <cstdint>

#include "sample_format.h"

namespace pg {
namespace sampler {

struct SampleRef {
  const int16_t* data = nullptr;
  uint32_t length = 0;             // samples (>= kMinSamples when resolved)
  uint32_t rate = kPoolRate;
  const int8_t (*overview)[2] = nullptr;   // kOverviewCols min/max pairs
  char name[kNameLen + 1] = {};
  uint16_t peak = 0;
  bool builtin = false;            // an empty user slot: the ROM sample plays
  uint8_t rom = 0;                 // which ROM sample (builtin only)
};

class SamplePool {
 public:
  virtual ~SamplePool() = default;
  // User slot 0..kPoolSlots-1: true and *out filled if it holds a valid sample.
  // Not real-time-unsafe: no locks, no heap (called at note-on on the audio thread).
  virtual bool user(int slot, SampleRef* out) const = 0;
  uint32_t generation() const { return gen_.load(std::memory_order_acquire); }

 protected:
  void bump() { gen_.fetch_add(1u, std::memory_order_acq_rel); }

 private:
  std::atomic<uint32_t> gen_{0};
};

// The slot's sample, or the built-in one an empty slot plays. pool may be nullptr
// (every slot empty). slot is clamped to 0..kPoolSlots-1. Always fills *out.
void resolve(const SamplePool* pool, int slot, SampleRef* out);

// A pool over caller-owned RAM (no heap): the desktop app, the host tools, tests.
// set() keeps the pointer (it must stay valid while the pool is in use and should be
// kSlotMaxSamples long, so a later set() of the same buffer never moves) and computes
// the header. Writer side: one thread; set()/clear() bump the generation.
class MemoryPool final : public SamplePool {
 public:
  bool user(int slot, SampleRef* out) const override;
  // false (and the slot is left empty) if the length is out of range or data is nullptr.
  bool set(int slot, const int16_t* data, uint32_t samples, const char* name);
  void clear(int slot);
  const SlotHeader* header(int slot) const;   // nullptr if empty

 private:
  struct Slot {
    const int16_t* data = nullptr;
    SlotHeader hdr{};
    std::atomic<bool> valid{false};
  };
  Slot slots_[kPoolSlots];
};

}  // namespace sampler
}  // namespace pg
