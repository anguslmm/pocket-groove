// The desktop's sample pool (docs/DESKTOP.md "Samples"): the folder samples/
// next to projects/ holds one audio file per pool slot, named by the slot:
// "01-ohyeah.wav" .. "12-....wav" (01 = slot 1). Each is converted on load to the
// slot's format (mono int16 at 22,050 Hz, trimmed, normalised; host/common/
// sample_convert.h) into a buffer allocated once per slot at full size, and handed
// to the instrument through a pg::sampler::MemoryPool. The folder is polled (stat,
// as the macro lab does) and a changed, new or removed file reloads its slot; a
// slot without a file plays its built-in sound, as on the device.
//
// drop() puts a file into a slot: a WAV is copied in under the slot's name (any
// other file of that slot is removed), anything else goes through macOS's
// afconvert when it exists. No SDL here (host/app/tests/test_desktop_samples.cpp).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "pg/sampler/sample_pool.h"

namespace desk {

class Samples {
 public:
  static constexpr uint32_t kPollMs = 400;
  static constexpr int64_t kSettleNs = 700000000LL;   // a file changed this recently is still being written

  // Creates the folder if needed and loads it. false if it cannot be created.
  bool init(const std::string& dir);
  const pg::sampler::SamplePool* pool() const { return &pool_; }
  const pg::sampler::MemoryPool& memoryPool() const { return pool_; }
  const std::string& dir() const { return dir_; }

  // Every UI frame: polls the folder when due. Returns true if a slot changed;
  // messages() then holds one human line per change ("slot 3: OHYEAH 1.42 s").
  bool tick(uint32_t nowMs);
  bool scanNow();   // the same, now
  const std::vector<std::string>& messages() const { return msgs_; }

  // Puts `path` into slot 0..11 (see above). msg: what happened, for a toast.
  bool drop(const std::string& path, int slot, std::string* msg);

  std::string fileOf(int slot) const;   // "" when the slot has no file
  int reloads() const { return reloads_; }

 private:
  struct Slot {
    std::string file;               // the name in dir_ ("" = none)
    int64_t mtimeNs = 0, size = -1;
    std::vector<int16_t> buf;       // kSlotMaxSamples, allocated once (the pool keeps the pointer)
    bool loaded = false;
  };
  void load(int slot);

  std::string dir_;
  pg::sampler::MemoryPool pool_;
  Slot slots_[pg::sampler::kPoolSlots];
  std::vector<std::string> msgs_;
  uint32_t nextPoll_ = 0;
  int reloads_ = 0;
};

}  // namespace desk
