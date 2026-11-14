// The sample pool on the Daisy (stage F1's twelve slots, pg/sampler/flash_pool.h):
// the pool's partition image lives in SDRAM, where the voice reads it (pg's
// FlashPool over the image's base, unchanged) and where every write lands at once
// (pg's SampleWriter, unchanged: the USB transfer's SPUT / SRM and the microphone
// recorder), and the QSPI flash keeps a copy in the background.
//
//   MirrorFlash   the FlashDriver SampleWriter writes through: NOR rules over the
//                 RAM image (erase sets 0xFF, a write only clears bits; a write that
//                 would need to set one fails, as on the device's flash), and a dirty
//                 bit per 4 KB sector.
//   flushStep()   one dirty sector per call, main loop: copied to a staging buffer
//                 first (the RAM may change meanwhile: the sector is then dirty again
//                 and flushed again), erased and programmed on the backing flash. A
//                 slot's header sector (its first, pg's layout) goes last of the
//                 slot's dirty sectors, so a power cut mid-flush leaves on the flash a
//                 slot whose header does not match its audio: the pool's own CRC
//                 check (FlashPool::rescan) then reads it as empty, never as a wrong
//                 sample.
//   loadFrom()    boot: the backing flash's copy into the RAM image (one pass,
//                 ~3.2 MB).
// So a sample put or a recording never waits for the flash, the transport may
// play, and a flush never stalls the audio (the firmware runs from RAM).
// Portable (host/daisy/test_daisy_pool.cpp).
#pragma once
#include <cstdint>

#include "pg/sampler/flash_pool.h"
#include "pg/sampler/sample_format.h"

namespace pgd {

class MirrorFlash final : public pg::sampler::FlashDriver {
 public:
  static constexpr uint32_t kBytes = pg::sampler::kPoolBytes;
  static constexpr uint32_t kSector = pg::sampler::kSectorBytes;
  static constexpr uint32_t kSectors = kBytes / kSector;

  // image: kBytes of RAM (the pool's base). backing: the flash region of kBytes.
  void init(uint8_t* image, pg::sampler::FlashDriver* backing, uint8_t* staging /* kSector bytes */);
  const uint8_t* image() const { return img_; }

  // Boot: the flash copy into RAM (no dirty bits). false if the backing read failed
  // (the image is then all 0xFF: an empty pool).
  bool loadFrom();
  // Main loop: flushes one dirty sector. Returns true if it did some work.
  bool flushStep();
  // Do not flush while true (e.g. a slot is being written right now and the caller
  // wants it written in one go; optional).
  void hold(bool on) { hold_ = on; }
  int dirtySectors() const;
  bool dirty() const { return dirtySectors() > 0; }

  // ---- FlashDriver (what SampleWriter calls) ----
  bool erase(uint32_t offset, uint32_t bytes) override;
  bool write(uint32_t offset, const void* data, uint32_t bytes) override;
  bool read(uint32_t offset, void* data, uint32_t bytes) override;

  struct Stats {
    uint32_t erases, writes, writeFailures;    // SampleWriter's calls
    uint32_t flushed, flushFailures;           // sectors to the backing flash
  };
  const Stats& stats() const { return stats_; }

 private:
  void mark(uint32_t offset, uint32_t bytes);
  bool isDirty(uint32_t s) const { return (dirty_[s >> 5] >> (s & 31)) & 1u; }
  void clearDirty(uint32_t s) { dirty_[s >> 5] &= ~(1u << (s & 31)); }
  int pickSector() const;

  uint8_t* img_ = nullptr;
  uint8_t* stage_ = nullptr;
  pg::sampler::FlashDriver* back_ = nullptr;
  uint32_t dirty_[(kSectors + 31) / 32] = {};
  bool hold_ = false;
  Stats stats_{};
};

}  // namespace pgd
