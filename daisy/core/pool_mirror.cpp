#include "pool_mirror.h"

#include <cstring>

namespace pgd {

namespace {
constexpr uint32_t kSlotSectors = pg::sampler::kSlotSectors;
}

void MirrorFlash::init(uint8_t* image, pg::sampler::FlashDriver* backing, uint8_t* staging) {
  img_ = image;
  back_ = backing;
  stage_ = staging;
  std::memset(dirty_, 0, sizeof(dirty_));
  stats_ = Stats{};
}

bool MirrorFlash::loadFrom() {
  if (!img_) return false;
  std::memset(dirty_, 0, sizeof(dirty_));
  if (!back_) {
    std::memset(img_, 0xFF, kBytes);
    return false;
  }
  constexpr uint32_t kChunk = 64 * 1024;
  for (uint32_t off = 0; off < kBytes; off += kChunk) {
    const uint32_t n = kBytes - off < kChunk ? kBytes - off : kChunk;
    if (!back_->read(off, img_ + off, n)) {
      std::memset(img_, 0xFF, kBytes);
      return false;
    }
  }
  return true;
}

void MirrorFlash::mark(uint32_t offset, uint32_t bytes) {
  if (!bytes) return;
  const uint32_t a = offset / kSector, b = (offset + bytes - 1) / kSector;
  for (uint32_t s = a; s <= b && s < kSectors; ++s) dirty_[s >> 5] |= 1u << (s & 31);
}

int MirrorFlash::dirtySectors() const {
  int n = 0;
  for (uint32_t s = 0; s < kSectors; ++s) n += isDirty(s) ? 1 : 0;
  return n;
}

// The lowest dirty sector, except that a slot's header sector waits for the rest of
// the slot's dirty sectors.
int MirrorFlash::pickSector() const {
  int header = -1;
  for (uint32_t s = 0; s < kSectors; ++s) {
    if (!isDirty(s)) continue;
    if (s % kSlotSectors != 0) return int(s);
    // a header sector: only if no other sector of its slot is dirty
    bool others = false;
    for (uint32_t k = s + 1; k < s + kSlotSectors && k < kSectors; ++k)
      if (isDirty(k)) {
        others = true;
        break;
      }
    if (!others) return int(s);
    if (header < 0) header = int(s);
  }
  return header;   // unreachable in practice (its slot's other dirty sector is returned first)
}

bool MirrorFlash::flushStep() {
  if (!img_ || !back_ || !stage_ || hold_) return false;
  const int s = pickSector();
  if (s < 0) return false;
  const uint32_t off = uint32_t(s) * kSector;
  std::memcpy(stage_, img_ + off, kSector);
  clearDirty(uint32_t(s));
  // Program only what is not erased: an all-0xFF sector needs the erase alone.
  bool blank = true;
  for (uint32_t i = 0; i < kSector; ++i)
    if (stage_[i] != 0xFF) {
      blank = false;
      break;
    }
  if (!back_->erase(off, kSector) || (!blank && !back_->write(off, stage_, kSector))) {
    ++stats_.flushFailures;
    dirty_[uint32_t(s) >> 5] |= 1u << (uint32_t(s) & 31);   // try again later
    return true;
  }
  ++stats_.flushed;
  return true;
}

bool MirrorFlash::erase(uint32_t offset, uint32_t bytes) {
  if (!img_ || (offset % kSector) || (bytes % kSector) || offset > kBytes || bytes > kBytes - offset) return false;
  std::memset(img_ + offset, 0xFF, bytes);
  mark(offset, bytes);
  ++stats_.erases;
  return true;
}

bool MirrorFlash::write(uint32_t offset, const void* data, uint32_t bytes) {
  if (!img_ || offset > kBytes || bytes > kBytes - offset) return false;
  const uint8_t* p = static_cast<const uint8_t*>(data);
  for (uint32_t i = 0; i < bytes; ++i)
    if ((img_[offset + i] & p[i]) != p[i]) {   // would need to set a bit: NOR cannot
      ++stats_.writeFailures;
      return false;
    }
  for (uint32_t i = 0; i < bytes; ++i) img_[offset + i] &= p[i];
  mark(offset, bytes);
  ++stats_.writes;
  return true;
}

bool MirrorFlash::read(uint32_t offset, void* data, uint32_t bytes) {
  if (!img_ || offset > kBytes || bytes > kBytes - offset) return false;
  std::memcpy(data, img_ + offset, bytes);
  return true;
}

}  // namespace pgd
