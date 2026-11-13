#include "flash_pool.h"

#include <cstring>

namespace pg {
namespace sampler {

namespace {
constexpr uint32_t slotOffset(int slot) { return uint32_t(slot) * kSlotBytes; }
constexpr uint32_t audioOffset(int slot) { return slotOffset(slot) + kSectorBytes; }
}  // namespace

// ---- FlashPool ------------------------------------------------------------------

void FlashPool::attach(const uint8_t* base) {
  base_ = base;
  rescan();
}

void FlashPool::rescan() {
  uint32_t v = 0;
  for (int s = 0; s < kPoolSlots && base_; ++s) {
    SlotHeader h;
    std::memcpy(&h, base_ + slotOffset(s), sizeof h);
    if (headerValid(h)) v |= 1u << s;
  }
  valid_.store(v, std::memory_order_release);
  bump();
}

void FlashPool::rescan(int slot) {
  if (slot < 0 || slot >= kPoolSlots) return;
  bool ok = false;
  if (base_) {
    SlotHeader h;
    std::memcpy(&h, base_ + slotOffset(slot), sizeof h);
    ok = headerValid(h);
  }
  uint32_t v = valid_.load(std::memory_order_acquire);
  v = ok ? (v | (1u << slot)) : (v & ~(1u << slot));
  valid_.store(v, std::memory_order_release);
  bump();
}

void FlashPool::hide(int slot) {
  if (slot < 0 || slot >= kPoolSlots) return;
  valid_.fetch_and(~(1u << slot), std::memory_order_acq_rel);
  bump();
}

const SlotHeader* FlashPool::header(int slot) const {
  if (!base_ || slot < 0 || slot >= kPoolSlots || !((valid_.load(std::memory_order_acquire) >> slot) & 1u)) return nullptr;
  return reinterpret_cast<const SlotHeader*>(base_ + slotOffset(slot));
}

bool FlashPool::user(int slot, SampleRef* out) const {
  const SlotHeader* h = header(slot);
  if (!h) return false;
  out->data = reinterpret_cast<const int16_t*>(base_ + audioOffset(slot));
  out->length = h->samples;
  out->rate = h->rate;
  out->overview = h->overview;
  headerName(*h, out->name);
  out->peak = h->peak;
  out->builtin = false;
  out->rom = 0;
  return true;
}

// ---- SampleWriter ----------------------------------------------------------------

const char* SampleWriter::resultName(Result r) {
  switch (r) {
    case kOk: return "OK";
    case kNoPool: return "NO_POOL";
    case kBadSlot: return "BAD_SLOT";
    case kBadSize: return "BAD_SIZE";
    case kFlash: return "FLASH";
    case kCrc: return "CRC";
    case kVerify: return "VERIFY";
    case kNoSession: return "NO_SESSION";
  }
  return "?";
}

void SampleWriter::attach(FlashDriver* driver, FlashPool* pool) {
  cancel();
  drv_ = driver;
  pool_ = pool;
}

SampleWriter::Result SampleWriter::begin(int slot, uint32_t samples) {
  cancel();
  if (!ready()) return kNoPool;
  if (slot < 0 || slot >= kPoolSlots) return kBadSlot;
  if (samples < kMinSamples || samples > kSlotMaxSamples) return kBadSize;
  pool_->hide(slot);   // chops of it stop; new notes play its built-in sound meanwhile
  if (!drv_->erase(slotOffset(slot), kSectorBytes)) return kFlash;   // the header first: empty from here on
  slot_ = slot;
  bytes_ = samples * 2u;
  written_ = 0;
  erasedTo_ = 0;
  return kOk;
}

SampleWriter::Result SampleWriter::write(uint32_t offset, const uint8_t* data, uint32_t bytes) {
  if (slot_ < 0) return kNoSession;
  if (offset != written_ || bytes > bytes_ - written_) return kBadSize;
  const uint32_t end = offset + bytes;
  while (erasedTo_ < end) {   // erase each sector as the first byte of it arrives
    if (!drv_->erase(audioOffset(slot_) + erasedTo_, kSectorBytes)) return kFlash;
    erasedTo_ += kSectorBytes;
  }
  if (!drv_->write(audioOffset(slot_) + offset, data, bytes)) return kFlash;
  written_ = end;
  return kOk;
}

SampleWriter::Result SampleWriter::commit(uint32_t crc, const char* name) {
  if (slot_ < 0) return kNoSession;
  const int slot = slot_;
  if (written_ != bytes_) return kBadSize;
  // 1. the audio as the flash holds it
  uint8_t buf[256];
  uint32_t c = 0;
  for (uint32_t o = 0; o < bytes_;) {
    const uint32_t n = bytes_ - o < sizeof buf ? bytes_ - o : uint32_t(sizeof buf);
    if (!drv_->read(audioOffset(slot) + o, buf, n)) return kFlash;
    c = crc32(buf, n, c);
    o += n;
  }
  if (c != crc) return kCrc;
  // 2. the header, from the audio as the CPU reads it (the mapping): the same CRC
  SlotHeader h;
  makeHeader(reinterpret_cast<const int16_t*>(pool_->base() + audioOffset(slot)), bytes_ / 2u, name, &h);
  if (h.dataCrc != crc) return kVerify;
  if (!drv_->write(slotOffset(slot), &h, sizeof h)) return kFlash;
  SlotHeader back;
  if (!drv_->read(slotOffset(slot), &back, sizeof back) || std::memcmp(&back, &h, sizeof h) != 0) return kVerify;
  slot_ = -1;
  pool_->rescan(slot);
  return pool_->header(slot) ? kOk : kVerify;
}

void SampleWriter::cancel() {
  if (slot_ >= 0 && pool_) pool_->rescan(slot_);   // its header is erased: empty
  slot_ = -1;
}

SampleWriter::Result SampleWriter::remove(int slot) {
  cancel();
  if (!ready()) return kNoPool;
  if (slot < 0 || slot >= kPoolSlots) return kBadSlot;
  pool_->hide(slot);
  const bool ok = drv_->erase(slotOffset(slot), kSectorBytes);
  pool_->rescan(slot);
  return ok ? kOk : kFlash;
}

SampleWriter::Result SampleWriter::read(int slot, uint32_t offset, uint8_t* data, uint32_t bytes) {
  if (!ready()) return kNoPool;
  const SlotHeader* h = pool_->header(slot);
  if (!h) return kBadSlot;
  if (offset > h->samples * 2u || bytes > h->samples * 2u - offset) return kBadSize;
  return drv_->read(audioOffset(slot) + offset, data, bytes) ? kOk : kFlash;
}

// ---- RamFlash ------------------------------------------------------------------

bool RamFlash::erase(uint32_t offset, uint32_t bytes) {
  if ((offset % kSectorBytes) || (bytes % kSectorBytes) || offset > kPoolBytes || bytes > kPoolBytes - offset) return false;
  std::memset(img_ + offset, 0xFF, bytes);
  ++erases;
  return true;
}

bool RamFlash::write(uint32_t offset, const void* data, uint32_t bytes) {
  if (offset > kPoolBytes || bytes > kPoolBytes - offset) return false;
  const uint8_t* d = static_cast<const uint8_t*>(data);
  for (uint32_t i = 0; i < bytes; ++i)
    if ((img_[offset + i] & d[i]) != d[i]) return false;   // would need to set a bit: not erased
  for (uint32_t i = 0; i < bytes; ++i) img_[offset + i] &= d[i];
  ++writes;
  return true;
}

bool RamFlash::read(uint32_t offset, void* data, uint32_t bytes) {
  if (offset > kPoolBytes || bytes > kPoolBytes - offset) return false;
  std::memcpy(data, img_ + offset, bytes);
  return true;
}

}  // namespace sampler
}  // namespace pg
