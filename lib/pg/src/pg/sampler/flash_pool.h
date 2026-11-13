// The sample pool in one flash partition (sample_format.h): its read side for the
// voice (FlashPool) and its write side for the USB transfer (SampleWriter).
// Portable: the partition is reached through FlashDriver (the device: ESP-IDF's
// esp_partition_* on `app1`, src/platform/sample_flash.*; the tests and the
// transfer simulator: a RAM image with NOR-flash semantics, RamFlash below).
//
// READ SIDE. `base` is the partition's first byte as the CPU reads it (the
// device: one esp_partition_mmap of the whole 3.19 MB, made once at boot and kept,
// so a chop's pointer never moves). Slot validity is decided on the UI thread
// (rescan: header magic, version, CRC, counts) and cached, so the voice's note-on
// costs a bit test and a few loads, never a CRC.
//
// WRITE SIDE. A slot is written as: begin (the slot is hidden from the voice, which
// chokes any chop of it at its next quantum, and its header sector is erased, so
// an interrupted write leaves an EMPTY slot, never a half one) -> write the audio
// bytes in order (each 4 KB sector is erased when the first byte of it arrives) ->
// commit (the audio read back through the driver must match the CRC the host sent;
// the header is built from the audio as the CPU reads it and must give that CRC
// too (a stale cache would fail here); the header is written and read back; the
// slot is shown again). Erasing and writing flash stalls both cores: the transfer
// refuses to start while the transport plays (pg/io/xfer.h).
#pragma once
#include <atomic>
#include <cstdint>

#include "sample_format.h"
#include "sample_pool.h"

namespace pg {
namespace sampler {

class FlashDriver {
 public:
  // Offsets and sizes inside the partition (kPoolBytes). erase: 4 KB aligned.
  virtual bool erase(uint32_t offset, uint32_t bytes) = 0;
  virtual bool write(uint32_t offset, const void* data, uint32_t bytes) = 0;
  virtual bool read(uint32_t offset, void* data, uint32_t bytes) = 0;

 protected:
  ~FlashDriver() = default;
};

class FlashPool final : public SamplePool {
 public:
  // nullptr: no partition (every slot empty). Rescans.
  void attach(const uint8_t* base);
  bool attached() const { return base_ != nullptr; }
  // UI thread: re-reads the headers (all, or one slot) and bumps the generation.
  void rescan();
  void rescan(int slot);
  // A slot being written reads as empty (and its chops stop): until rescan(slot).
  void hide(int slot);
  bool user(int slot, SampleRef* out) const override;
  // The slot's header as the CPU reads it, nullptr when empty or hidden.
  const SlotHeader* header(int slot) const;
  const uint8_t* base() const { return base_; }

 private:
  const uint8_t* base_ = nullptr;
  std::atomic<uint32_t> valid_{0};   // bit = slot holds a valid sample (and is not hidden)
};

class SampleWriter {
 public:
  enum Result : uint8_t { kOk = 0, kNoPool, kBadSlot, kBadSize, kFlash, kCrc, kVerify, kNoSession };
  static const char* resultName(Result r);   // "OK", "NO_POOL", ... (one protocol token)

  void attach(FlashDriver* driver, FlashPool* pool);
  bool ready() const { return drv_ && pool_ && pool_->attached(); }
  FlashPool* pool() const { return pool_; }

  Result begin(int slot, uint32_t samples);
  Result write(uint32_t offset, const uint8_t* data, uint32_t bytes);   // audio bytes, in order
  Result commit(uint32_t crc, const char* name);
  void cancel();                      // the slot stays empty
  bool active() const { return slot_ >= 0; }
  int slot() const { return slot_; }
  uint32_t written() const { return written_; }
  uint32_t total() const { return bytes_; }

  // The slot becomes empty: EVERY sector of it is erased (header and audio), so a
  // microphone recording can go straight in later without an erase (stage F2). One
  // driver call of kSlotBytes (the device: about 1-2 s, both cores stalled).
  Result remove(int slot);
  // The audio of a valid slot (offset in bytes), through the driver.
  Result read(int slot, uint32_t offset, uint8_t* data, uint32_t bytes);

  // ---- MICROPHONE RECORDING (stage F2, docs/STATE.md) ----------------------------
  // A recording streams into a slot while the transport may play, so it must never
  // erase (an erase stalls both cores for tens of ms; a 512-byte write ~0.35 ms):
  //   erased(slot)        every byte of the slot reads 0xFF through the mapping (the
  //                       CPU's view; a 4-byte compare per word, 272 KB, no driver call)
  //   remove(slot)        makes it so (above), with the output faded by the caller
  //   beginRecording      the slot must be empty and erased (checked again here); its
  //                       length is not known yet: write() takes up to kSlotAudioBytes
  //   write()             as for a put, in order, never erasing
  //   commitRecording     the first `samples` samples are the sample (the tail after
  //                       them is left as written: a filled slot's tail is never read);
  //                       CRC through the driver vs the mapping as for commit(), then the
  //                       header with the playback gain (setPlaybackGain), read back
  //   cancel()            nothing was committed: the slot stays empty (and dirty: the
  //                       next recording finds it not erased and erases it first)
  bool erased(int slot) const;
  Result beginRecording(int slot);
  Result commitRecording(uint32_t samples, const char* name);
  bool recording() const { return slot_ >= 0 && rec_; }

 private:
  FlashDriver* drv_ = nullptr;
  FlashPool* pool_ = nullptr;
  int slot_ = -1;
  bool rec_ = false;
  uint32_t bytes_ = 0, written_ = 0, erasedTo_ = 0;
};

// A RAM image of the partition with NOR flash rules (erase sets 0xFF, a write can
// only clear bits; a write over unerased bytes that would need to set one fails).
// Host tests and the transfer simulator; `image` must hold kPoolBytes.
class RamFlash final : public FlashDriver {
 public:
  explicit RamFlash(uint8_t* image) : img_(image) {}
  bool erase(uint32_t offset, uint32_t bytes) override;
  bool write(uint32_t offset, const void* data, uint32_t bytes) override;
  bool read(uint32_t offset, void* data, uint32_t bytes) override;
  uint32_t erases = 0, writes = 0;

 private:
  uint8_t* img_;
};

}  // namespace sampler
}  // namespace pg
