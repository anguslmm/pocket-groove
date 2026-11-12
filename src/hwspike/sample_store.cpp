// Sample storage in the unused OTA slot. See sample_store.h.
//
// ESP-IDF v5.5.5 esp_partition.h (read in this session):
//  * esp_partition_find_first(type, subtype, label); app partitions are
//    writable (readonly = false) -- that is how OTA updates write them.
//  * esp_partition_erase_range: offset and size multiples of erase_size (4 KB).
//  * esp_partition_write: flash must be erased first; bounds-checked against
//    the partition (ESP_ERR_INVALID_SIZE).
//  * esp_partition_mmap(part, off, size, ESP_PARTITION_MMAP_DATA, &ptr, &h):
//    maps through the MMU (64 KB pages) into the data cache's address space.
// To stay clear of any stale-cache question the regions are unmapped before
// every erase/write and mapped again afterwards.
#include "sample_store.h"

#include <Arduino.h>

#include <esp_heap_caps.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_timer.h>
#include <cstring>

namespace hws {
namespace store {

namespace {

const esp_partition_t* s_part = nullptr;
Info s_info = {false, "not started", 0, 0, 0, 0, 0};
esp_partition_mmap_handle_t s_hHdr = 0, s_hRec = 0, s_hBench = 0, s_hPoll = 0;
const uint8_t* s_hdr = nullptr;
const int16_t* s_rec = nullptr;
const int16_t* s_bench = nullptr;
const uint8_t* s_poll = nullptr;
bool s_mapped = false;
volatile bool s_benchReady = false;   // cached at map time: the UI never dereferences the mapping

bool inRange(uint32_t off, uint32_t bytes) {
  return s_info.ok && off < kUsedEnd && bytes <= kUsedEnd - off && off + bytes <= s_info.partSize;
}

uint32_t crc32(const uint8_t* p, size_t n) {
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; ++i) {
    c ^= p[i];
    for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
  }
  return ~c;
}

}  // namespace

uint32_t headerCrc(const RecHeader& h) {
  return crc32(reinterpret_cast<const uint8_t*>(&h), offsetof(RecHeader, crc));
}

bool begin() {
  s_info = {false, "", 0, 0, 0, 0, 0};
  s_part = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, nullptr);
  const esp_partition_t* fs = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, nullptr);
  const esp_partition_t* run = esp_ota_get_running_partition();
  if (fs) { s_info.fsAddr = fs->address; s_info.fsSize = fs->size; }
  if (run) s_info.runningAddr = run->address;
  if (!s_part) { s_info.why = "no ota_1 slot"; return false; }
  s_info.partAddr = s_part->address;
  s_info.partSize = s_part->size;
  if (s_part->address != kExpectAddr || s_part->size != kExpectSize) { s_info.why = "ota_1 not at 0x340000+0x330000"; return false; }
  if (!fs || fs->address != kExpectFsAddr || fs->size != kExpectFsSize) { s_info.why = "spiffs not at 0x670000+0x180000"; return false; }
  if (s_part->address + s_part->size > fs->address) { s_info.why = "ota_1 overlaps spiffs"; return false; }
  if (!run || run->address == s_part->address) { s_info.why = "running from ota_1"; return false; }
  if (s_part->readonly || s_part->encrypted) { s_info.why = "ota_1 read-only/encrypted"; return false; }
  if (kUsedEnd > s_part->size) { s_info.why = "layout too big"; return false; }
  s_info.ok = true;
  s_info.why = "";
  if (!mapAll()) {
    s_info.why = "mmap failed";
    // still ok for writing; playback refuses without a mapping
  }
  return true;
}

const Info& info() { return s_info; }

bool mapAll() {
  if (!s_info.ok) return false;
  if (s_mapped) return true;
  const void* p = nullptr;
  bool ok = true;
  ok = esp_partition_mmap(s_part, kRecHdrOff, 0x10000, ESP_PARTITION_MMAP_DATA, &p, &s_hHdr) == ESP_OK && ok;
  s_hdr = static_cast<const uint8_t*>(p);
  ok = esp_partition_mmap(s_part, kRecOff, kRecBytes, ESP_PARTITION_MMAP_DATA, &p, &s_hRec) == ESP_OK && ok;
  s_rec = static_cast<const int16_t*>(p);
  ok = esp_partition_mmap(s_part, kBenchOff, kBenchBytes, ESP_PARTITION_MMAP_DATA, &p, &s_hBench) == ESP_OK && ok;
  s_bench = static_cast<const int16_t*>(p);
  ok = esp_partition_mmap(s_part, kPollOff, kPollBytes, ESP_PARTITION_MMAP_DATA, &p, &s_hPoll) == ESP_OK && ok;
  s_poll = static_cast<const uint8_t*>(p);
  s_mapped = true;
  if (!ok) {
    unmapAll();
    return false;
  }
  uint32_t m[2];
  memcpy(m, s_hdr + kBenchHdrOff, sizeof(m));
  s_benchReady = (m[0] == kBenchMagic && m[1] == kBenchBytes);
  return true;
}

void unmapAll() {
  if (!s_mapped) return;
  s_hdr = nullptr;
  s_rec = nullptr;
  s_bench = nullptr;
  s_poll = nullptr;
  if (s_hHdr) esp_partition_munmap(s_hHdr);
  if (s_hRec) esp_partition_munmap(s_hRec);
  if (s_hBench) esp_partition_munmap(s_hBench);
  if (s_hPoll) esp_partition_munmap(s_hPoll);
  s_hHdr = s_hRec = s_hBench = s_hPoll = 0;
  s_hdr = nullptr;
  s_rec = nullptr;
  s_bench = nullptr;
  s_poll = nullptr;
  s_mapped = false;
}

bool mapped() { return s_mapped; }
const int16_t* recData() { return s_rec; }
const int16_t* benchData() { return s_bench; }
const uint8_t* pollData() { return s_poll; }

bool readRecHeader(RecHeader* h) {
  if (!s_hdr || !h) return false;
  memcpy(h, s_hdr + kRecHdrOff, sizeof(RecHeader));
  return h->magic == kRecMagic && h->crc == headerCrc(*h) && h->samples >= 2 && h->samples <= kRecBytes / 2;
}

bool benchReady() { return s_benchReady; }

bool erase(uint32_t off, uint32_t bytes, uint32_t* ms) {
  if (!inRange(off, bytes) || (off & 0xFFF) || (bytes & 0xFFF)) return false;
  const bool wasMapped = s_mapped;
  unmapAll();
  const int64_t t0 = esp_timer_get_time();
  const bool ok = esp_partition_erase_range(s_part, off, bytes) == ESP_OK;
  if (ms) *ms = static_cast<uint32_t>((esp_timer_get_time() - t0) / 1000);
  if (wasMapped) mapAll();
  return ok;
}

// Recording writes stream into an erased area: called many times, so it does
// NOT touch the mapping (the caller unmaps once around the whole recording).
bool write(uint32_t off, const void* src, uint32_t bytes, uint32_t* us) {
  if (!inRange(off, bytes) || s_mapped) return false;
  const int64_t t0 = esp_timer_get_time();
  const bool ok = esp_partition_write(s_part, off, src, bytes) == ESP_OK;
  if (us) *us = static_cast<uint32_t>(esp_timer_get_time() - t0);
  return ok;
}

bool writeRecHeader(const RecHeader& hIn) {
  RecHeader h = hIn;
  h.magic = kRecMagic;
  h.crc = headerCrc(h);
  const bool wasMapped = s_mapped;
  unmapAll();
  bool ok = inRange(kRecHdrOff, 0x1000) && esp_partition_erase_range(s_part, kRecHdrOff, 0x1000) == ESP_OK;
  ok = ok && esp_partition_write(s_part, kRecHdrOff, &h, sizeof(h)) == ESP_OK;
  if (wasMapped) mapAll();
  return ok;
}

bool prepareBench(void (*progress)(int, void*), void* user) {
  if (!s_info.ok) return false;
  const bool wasMapped = s_mapped;
  unmapAll();
  bool ok = esp_partition_erase_range(s_part, kBenchHdrOff, 0x1000) == ESP_OK;
  ok = ok && esp_partition_erase_range(s_part, kBenchOff, kBenchBytes) == ESP_OK;
  constexpr uint32_t kChunk = 4096;
  int16_t* buf = static_cast<int16_t*>(heap_caps_malloc(kChunk, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (!buf) ok = false;
  uint32_t x = 0x12345678u;
  for (uint32_t off = 0; ok && off < kBenchBytes; off += kChunk) {
    for (uint32_t i = 0; i < kChunk / 2; ++i) {
      x ^= x << 13; x ^= x >> 17; x ^= x << 5;      // xorshift32
      buf[i] = static_cast<int16_t>((static_cast<int32_t>(x >> 16) - 32768) / 4);   // -12 dBFS noise
    }
    ok = esp_partition_write(s_part, kBenchOff + off, buf, kChunk) == ESP_OK;
    if (progress && (off % (64 * 1024)) == 0) progress(static_cast<int>(off * 100 / kBenchBytes), user);
  }
  heap_caps_free(buf);
  const uint32_t m[2] = {kBenchMagic, kBenchBytes};
  ok = ok && esp_partition_write(s_part, kBenchHdrOff, m, sizeof(m)) == ESP_OK;
  if (wasMapped || ok) mapAll();
  return ok;
}

}  // namespace store
}  // namespace hws
