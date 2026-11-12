// Pocket Groove hardware spike -- sample storage in internal flash, played
// back straight from the memory-mapped flash.
//
// WHERE: the second OTA app slot of the stock default_8MB.csv table
// ("app1", ota_1, 0x340000, 3.19 MB). Nothing in this project uses OTA, so the
// slot is never written by anything else; the partition TABLE is the stock
// one (this env builds byte-identical partitions.bin), so flashing this
// firmware and flashing the instrument back changes no offsets at all. The
// owner's projects live in the "spiffs" LittleFS partition at 0x670000, which
// this module never touches: begin() refuses to run unless app1 sits exactly
// at 0x340000 + 0x330000, ends at or before the start of "spiffs", and is not
// the running app; every erase/write is bounds-checked again against app1.
// See docs/hw-spike.md "What flashing this does".
//
// Layout inside app1 (all offsets 64 KB aligned except the headers):
//   0x000000  recording header (one 4 KB sector)
//   0x001000  benchmark header (one 4 KB sector)
//   0x010000  recording, mono int16, up to kRecBytes
//   0x090000  benchmark sample, 1 MB of deterministic noise (written once)
//   0x190000  cache polluter area, 256 KB (only read; content irrelevant)
//
// No Arduino / ESP types in this header.
#pragma once
#include <cstddef>
#include <cstdint>

namespace hws {
namespace store {

constexpr uint32_t kRecHdrOff = 0x000000;
constexpr uint32_t kBenchHdrOff = 0x001000;
constexpr uint32_t kRecOff = 0x010000;
constexpr uint32_t kRecBytes = 0x080000;     // 512 KB = 5.94 s at 44.1 kHz mono 16-bit
constexpr uint32_t kBenchOff = 0x090000;
constexpr uint32_t kBenchBytes = 0x100000;   // 1 MB = 524288 samples
constexpr uint32_t kPollOff = 0x190000;
constexpr uint32_t kPollBytes = 0x040000;    // 256 KB
constexpr uint32_t kUsedEnd = 0x1D0000;

// The only layout this firmware accepts (stock default_8MB.csv).
constexpr uint32_t kExpectAddr = 0x340000, kExpectSize = 0x330000;
constexpr uint32_t kExpectFsAddr = 0x670000, kExpectFsSize = 0x180000;

struct RecHeader {
  uint32_t magic;          // kRecMagic
  uint32_t samples;
  uint32_t sampleRate;
  uint8_t  gain, channel, reg14, reg16;
  int32_t  peak;
  uint32_t clips;
  float    rmsDb, dcLsb;
  uint32_t seq;            // recording number
  uint32_t crc;            // of the header bytes before this field
};
constexpr uint32_t kRecMagic = 0x50475352u;   // "PGSR"
constexpr uint32_t kBenchMagic = 0x50475342u; // "PGSB"

struct Info {
  bool ok;
  const char* why;         // "" when ok
  uint32_t partAddr, partSize, fsAddr, fsSize, runningAddr;
};

bool begin();              // find + verify the slot, map it (UI thread)
const Info& info();

// Mapped views (nullptr if not mapped). Valid until unmap().
const int16_t* recData();
const int16_t* benchData();
const uint8_t* pollData();
bool mapAll();
void unmapAll();
bool mapped();

bool readRecHeader(RecHeader* h);   // from the mapped header (false if none / bad crc)
bool benchReady();

// Erase / write (offsets inside app1). Both stall both cores while the flash
// is busy. Never from the audio task.
bool erase(uint32_t off, uint32_t bytes, uint32_t* ms);
bool write(uint32_t off, const void* src, uint32_t bytes, uint32_t* us);
bool writeRecHeader(const RecHeader& h);
// Writes the 1 MB benchmark sample + its header. progress(0..100) is called
// between chunks. Blocking for several seconds.
bool prepareBench(void (*progress)(int pct, void* user), void* user);

uint32_t headerCrc(const RecHeader& h);

}  // namespace store
}  // namespace hws
