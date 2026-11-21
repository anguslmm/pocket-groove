// A region of the Seed's QSPI flash (IS25LP064A) as a pg::sampler::FlashDriver, for
// /int's snapshots (daisy/core/fs_persist.h) and the sample pool's copy
// (daisy/core/pool_mirror.h). Offsets are relative to the region's base address.
//
// libDaisy 1289a00 (src/per/qspi.cpp), what this relies on:
//   EraseSector(addr)      4 KB, waits for the chip, then back to memory-mapped mode
//   Erase(start, end)      64 KB block erases where aligned (4 KB otherwise), waits;
//                          does NOT return to memory-mapped mode after a block erase
//   Write(addr, n, buf)    page programs, waits, then back to memory-mapped mode
//   both refuse to run while the program executes from QSPI (CheckProgramMemory):
//   this firmware runs from SRAM, so they work, and since nothing reads the QSPI
//   while it is in indirect mode (the ROM part is copied to SDRAM at boot), the
//   audio interrupt never notices.
// read() copies from the memory-mapped window (0x90000000 + address) after
// invalidating the D-cache lines over it; after a block erase it first restores the
// mapping (re-running the QSPI init) if no Write came in between.
// Main loop only. Every call waits for the chip (a 64 KB block erase: ~0.15 s
// typical, 1 s at most per the datasheet figures libDaisy's timeouts allow for).
#pragma once
#include <cstdint>

#include "pg/sampler/flash_pool.h"

namespace daisy {
class QSPIHandle;
}

namespace pgdev {

class QspiRegion final : public pg::sampler::FlashDriver {
 public:
  void attach(daisy::QSPIHandle* q, uint32_t baseAddr, uint32_t bytes);
  bool erase(uint32_t offset, uint32_t bytes) override;
  bool write(uint32_t offset, const void* data, uint32_t bytes) override;
  bool read(uint32_t offset, void* data, uint32_t bytes) override;

  struct Stats {
    uint32_t erases, writes, reads, failures, remaps, maxEraseUs, maxWriteUs;
  };
  const Stats& stats() const { return st_; }

 private:
  bool inRange(uint32_t off, uint32_t n) const { return off <= bytes_ && n <= bytes_ - off; }
  daisy::QSPIHandle* q_ = nullptr;
  uint32_t base_ = 0, bytes_ = 0;
  Stats st_{};
};

// The QSPI's memory-mapped mode is shared by all regions: a block erase in one
// leaves it off for all.
void qspiMarkUnmapped();
bool qspiMapped();
// An erase or a write is under way (the main loop is in it): the audio interrupt's records.
bool qspiBusy();

}  // namespace pgdev
