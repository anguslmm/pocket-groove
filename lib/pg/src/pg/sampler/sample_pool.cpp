#include "sample_pool.h"

#include <cstring>

#include "rom_samples.h"

namespace pg {
namespace sampler {

void resolve(const SamplePool* pool, int slot, SampleRef* out) {
  if (slot < 0) slot = 0;
  if (slot >= kPoolSlots) slot = kPoolSlots - 1;
  if (pool && pool->user(slot, out) && out->data && out->length >= kMinSamples) {
    out->builtin = false;
    return;
  }
  const RomSample& r = kRom[slot % kRomCount];
  out->data = r.data;
  out->length = r.length;
  out->rate = kPoolRate;
  out->overview = r.overview;
  std::memset(out->name, 0, sizeof(out->name));
  std::strncpy(out->name, r.name, kNameLen);
  out->peak = r.peak;
  out->builtin = true;
  out->rom = uint8_t(slot % kRomCount);
}

bool MemoryPool::user(int slot, SampleRef* out) const {
  if (slot < 0 || slot >= kPoolSlots) return false;
  const Slot& s = slots_[slot];
  if (!s.valid.load(std::memory_order_acquire)) return false;
  out->data = s.data;
  out->length = s.hdr.samples;
  out->rate = s.hdr.rate;
  out->overview = s.hdr.overview;
  headerName(s.hdr, out->name);
  out->peak = s.hdr.peak;
  out->builtin = false;
  out->rom = 0;
  return true;
}

bool MemoryPool::set(int slot, const int16_t* data, uint32_t samples, const char* name) {
  if (slot < 0 || slot >= kPoolSlots) return false;
  Slot& s = slots_[slot];
  s.valid.store(false, std::memory_order_release);
  bump();
  if (!data || samples < kMinSamples || samples > kSlotMaxSamples) return false;
  s.data = data;
  makeHeader(data, samples, name, &s.hdr);
  s.valid.store(true, std::memory_order_release);
  bump();
  return true;
}

void MemoryPool::clear(int slot) {
  if (slot < 0 || slot >= kPoolSlots) return;
  slots_[slot].valid.store(false, std::memory_order_release);
  bump();
}

const SlotHeader* MemoryPool::header(int slot) const {
  if (slot < 0 || slot >= kPoolSlots || !slots_[slot].valid.load(std::memory_order_acquire)) return nullptr;
  return &slots_[slot].hdr;
}

}  // namespace sampler
}  // namespace pg
