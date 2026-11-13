// Pocket Groove platform layer -- the sample pool in internal flash (stage F1;
// pg/sampler/sample_format.h, flash_pool.h).
//
// WHERE: the second OTA app slot of the stock default_8MB.csv table ("app1",
// ota_1, 0x340000 + 0x330000 = exactly twelve 68-sector slots). Nothing in this
// project uses OTA. The partition TABLE is unchanged; begin() refuses the pool
// (every slot plays its built-in sound) unless app1 sits exactly there, ends at or
// before "spiffs" (the projects, never touched), and is not the running app.
// The hw-spike's recordings live in the same partition in its own layout: the two
// overwrite each other's samples, and the instrument reads anything without a
// valid slot header as empty (docs/hw-spike.md).
//
// READING: the whole partition is memory-mapped once (esp_partition_mmap, 51 MMU
// pages of 64 KB) and stays mapped, so a chop's pointer never moves; the voice
// reads it through the flash cache (hardware-notes 11.5: 44-46 cycles per voice
// sample). WRITING (the USB transfer, pg/io/xfer.h): esp_partition_erase_range /
// esp_partition_write, which stall both cores, never while the transport plays;
// ESP-IDF invalidates the cache over the written range, and SampleWriter::commit
// checks that the CPU reads what the flash holds before it shows the slot.
// No Arduino / ESP types in this header.
#pragma once
#include <cstdint>

#include "pg/sampler/flash_pool.h"

namespace plat {
namespace samples {

struct Info {
  bool ok;               // the pool is mapped and attached
  const char* why;       // "" when ok
  uint32_t partAddr, partSize, mapMs;
  int slotsUsed;
};

// Finds and checks app1, maps it, attaches the pool and the writer. Boot, before audio.
bool begin();
const Info& info();
pg::sampler::FlashPool& pool();       // nothing attached when begin() failed: every slot empty
pg::sampler::SampleWriter& writer();  // not ready() when begin() failed

}  // namespace samples
}  // namespace plat
