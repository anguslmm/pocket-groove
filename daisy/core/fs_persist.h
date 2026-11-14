// Keeps /int (ramfs.h, in SDRAM) on the QSPI flash: two banks, each one complete
// snapshot of the file set, written alternately. A bank is
//   offset 0       header: "PGSN", version, sequence, payload bytes, payload CRC32,
//                  header CRC32 (24 bytes, in its own 4 KB sector)
//   offset 4096    payload: RamFs::serialize()
// and is valid when both CRCs check. At boot load() takes the valid bank with the
// newest sequence (falling back to the other if the newest's payload is bad). A save
// writes the OTHER bank: its blocks are erased, the payload programmed, the header
// written last and the payload read back, so a power cut at any point leaves the
// previous snapshot intact (host/daisy/test_daisy_fs.cpp cuts the power at every
// flash operation of a save).
//
// When: some time (kQuietMs) after the file set last changed, whatever the transport
// does: the firmware runs from RAM (BOOT_SRAM), so erasing the QSPI stalls nothing
// but the main loop's step that waits for it, never the audio. The save is a state
// machine: one block erase (~0.1-1 s on the IS25LP064A, the longest wait) or
// kProgramChunk bytes per step(), so USB and keys are served in between. The file
// set is copied to `staging` when a save starts, so a change during the save is
// simply picked up by the next one.
//
// Flash access goes through pg::sampler::FlashDriver (erase / write / read inside
// the region; the Daisy: daisy/src/qspi_flash.h). Portable.
#pragma once
#include <cstdint>

#include "pg/sampler/flash_pool.h"
#include "ramfs.h"

namespace pgd {

class FsPersist {
 public:
  static constexpr uint32_t kHeaderSector = 4096;
  static constexpr uint32_t kEraseChunk = 65536;     // one QSPI block per step
  static constexpr uint32_t kProgramChunk = 8192;    // 32 pages per step
  static constexpr uint32_t kVerifyChunk = 32768;
  static constexpr uint32_t kQuietMs = 1500;         // after the last change
  static constexpr uint32_t kRetryMs = 30000;        // after a failed save

  // driver: a region of 2 * bankBytes (bank A at 0, bank B at bankBytes), bankBytes
  // a multiple of kEraseChunk. staging: bankBytes - kHeaderSector bytes.
  void init(RamFs* fs, pg::sampler::FlashDriver* driver, uint32_t bankBytes, uint8_t* staging);
  uint32_t payloadCapacity() const { return bank_ - kHeaderSector; }

  enum class Load : uint8_t { Loaded, Empty, Corrupt };
  // Boot: fills the RamFs from the newest valid bank. Empty: no valid bank (first
  // boot: nothing to load). Corrupt: a header was valid but no payload was.
  Load load();

  // Main loop. Returns true while a save is in progress.
  bool step(uint32_t nowMs);
  // Start a save at the next step if anything changed (no quiet wait).
  void requestNow() { now_ = true; }
  bool busy() const { return st_ != St::Idle; }
  bool dirty() const { return fs_ && fs_->revision() != savedRev_; }

  struct Stats {
    uint32_t saves, failures, seq, lastBytes, lastSaveMs, lastDurationMs;
    int bank;            // the bank holding the current snapshot: 0 = A, 1 = B, -1 = none
    const char* lastError;
  };
  const Stats& stats() const { return stats_; }

 private:
  enum class St : uint8_t { Idle, Erase, Program, Header, Verify };
  struct Header {
    char magic[4];
    uint32_t version, seq, bytes, crc, hcrc;
  };
  static uint32_t headerCrc(const Header& h);
  bool readHeader(int bank, Header* h);
  void fail(uint32_t nowMs, const char* why);

  RamFs* fs_ = nullptr;
  pg::sampler::FlashDriver* drv_ = nullptr;
  uint32_t bank_ = 0;
  uint8_t* stage_ = nullptr;

  St st_ = St::Idle;
  int target_ = 0;
  uint32_t bytes_ = 0, crc_ = 0, pos_ = 0, eraseTo_ = 0, verifyCrc_ = 0;
  uint32_t seq_ = 0, startMs_ = 0, savingRev_ = 0;
  uint32_t savedRev_ = 0, seenRev_ = 0, changedMs_ = 0, retryAfter_ = 0;
  bool now_ = false;
  Stats stats_{0, 0, 0, 0, 0, 0, -1, ""};
};

}  // namespace pgd
