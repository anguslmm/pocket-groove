// A NOR flash region in RAM for the host tests and the host simulation of the
// firmware: erase (4 KB aligned) sets 0xFF, a write can only clear bits (a write
// that would need to set one fails), reads are plain. Power cuts: with cutAfter set,
// the flash dies at that operation (counted from 0): an erase leaves its first half
// erased, a write its first half written, and every later operation fails, so a
// test can check what a reboot finds. Portable, header-only, heap-free (the caller
// owns the image).
#pragma once
#include <cstdint>
#include <cstring>

#include "pg/sampler/flash_pool.h"

namespace pgd {

class NorSim final : public pg::sampler::FlashDriver {
 public:
  static constexpr uint32_t kSector = 4096;
  NorSim(uint8_t* image, uint32_t bytes) : img_(image), n_(bytes) {}
  void wipe(uint8_t v = 0xFF) { std::memset(img_, v, n_); }

  bool erase(uint32_t offset, uint32_t bytes) override {
    if ((offset % kSector) || (bytes % kSector) || offset > n_ || bytes > n_ - offset) return false;
    if (dead_) return false;
    if (cutNow()) {
      std::memset(img_ + offset, 0xFF, bytes / 2);
      dead_ = true;
      return false;
    }
    std::memset(img_ + offset, 0xFF, bytes);
    ++erases;
    return true;
  }
  bool write(uint32_t offset, const void* data, uint32_t bytes) override {
    if (offset > n_ || bytes > n_ - offset || dead_) return false;
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (uint32_t i = 0; i < bytes; ++i)
      if ((img_[offset + i] & p[i]) != p[i]) {
        ++badWrites;
        return false;
      }
    if (cutNow()) {
      for (uint32_t i = 0; i < bytes / 2; ++i) img_[offset + i] &= p[i];
      dead_ = true;
      return false;
    }
    for (uint32_t i = 0; i < bytes; ++i) img_[offset + i] &= p[i];
    ++writes;
    return true;
  }
  bool read(uint32_t offset, void* data, uint32_t bytes) override {
    if (offset > n_ || bytes > n_ - offset || dead_) return false;
    std::memcpy(data, img_ + offset, bytes);
    return true;
  }

  bool cutNow() { const bool c = cutAfter >= 0 && ops_ == static_cast<uint64_t>(cutAfter); ++ops_; return c; }
  // Power cut at operation k (0-based), -1 = never. revive(): power back on.
  int64_t cutAfter = -1;
  void revive() { dead_ = false; cutAfter = -1; }
  uint64_t ops() const { return ops_; }
  bool dead() const { return dead_; }
  uint32_t erases = 0, writes = 0, badWrites = 0;

 private:
  uint8_t* img_;
  uint32_t n_;
  uint64_t ops_ = 0;
  bool dead_ = false;
};

}  // namespace pgd
