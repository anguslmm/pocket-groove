// The sample pool in app1. See sample_flash.h.
//
// ESP-IDF v5.5 esp_partition.h (as the hw-spike's sample_store.cpp reads it):
//  * esp_partition_find_first(type, subtype, label); app partitions are writable.
//  * esp_partition_erase_range: offset and size multiples of erase_size (4 KB).
//  * esp_partition_write / esp_partition_read: bounds-checked against the partition.
//  * esp_partition_mmap(part, 0, size, ESP_PARTITION_MMAP_DATA, &ptr, &handle).
#include "sample_flash.h"

#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_timer.h>

namespace plat {
namespace samples {

namespace {

constexpr uint32_t kExpectAddr = 0x340000, kExpectSize = 0x330000;
constexpr uint32_t kExpectFsAddr = 0x670000;

const esp_partition_t* s_part = nullptr;
esp_partition_mmap_handle_t s_map = 0;
Info s_info = {false, "not started", 0, 0, 0, 0};
pg::sampler::FlashPool s_pool;
pg::sampler::SampleWriter s_writer;

class PartitionFlash final : public pg::sampler::FlashDriver {
 public:
  bool erase(uint32_t offset, uint32_t bytes) override {
    return s_part && esp_partition_erase_range(s_part, offset, bytes) == ESP_OK;
  }
  bool write(uint32_t offset, const void* data, uint32_t bytes) override {
    return s_part && esp_partition_write(s_part, offset, data, bytes) == ESP_OK;
  }
  bool read(uint32_t offset, void* data, uint32_t bytes) override {
    return s_part && esp_partition_read(s_part, offset, data, bytes) == ESP_OK;
  }
};
PartitionFlash s_flash;

}  // namespace

bool begin() {
  s_info = Info{false, "", 0, 0, 0, 0};
  s_part = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, nullptr);
  const esp_partition_t* fs = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, nullptr);
  const esp_partition_t* run = esp_ota_get_running_partition();
  if (!s_part) { s_info.why = "no ota_1 slot"; return false; }
  s_info.partAddr = s_part->address;
  s_info.partSize = s_part->size;
  const char* why = nullptr;
  if (s_part->address != kExpectAddr || s_part->size != kExpectSize) why = "ota_1 not at 0x340000+0x330000";
  else if (!fs || fs->address != kExpectFsAddr || s_part->address + s_part->size > fs->address) why = "spiffs not after ota_1";
  else if (!run || run->address == s_part->address) why = "running from ota_1";
  else if (s_part->readonly || s_part->encrypted) why = "ota_1 read-only or encrypted";
  else if (s_part->size < pg::sampler::kPoolBytes) why = "ota_1 too small";
  if (why) {
    s_info.why = why;
    s_part = nullptr;
    return false;
  }
  const int64_t t0 = esp_timer_get_time();
  const void* p = nullptr;
  if (esp_partition_mmap(s_part, 0, pg::sampler::kPoolBytes, ESP_PARTITION_MMAP_DATA, &p, &s_map) != ESP_OK || !p) {
    s_info.why = "mmap failed";
    s_part = nullptr;
    return false;
  }
  s_pool.attach(static_cast<const uint8_t*>(p));   // reads the twelve headers
  s_writer.attach(&s_flash, &s_pool);
  s_info.mapMs = static_cast<uint32_t>((esp_timer_get_time() - t0) / 1000);
  for (int i = 0; i < pg::sampler::kPoolSlots; ++i) s_info.slotsUsed += s_pool.header(i) ? 1 : 0;
  s_info.ok = true;
  return true;
}

const Info& info() { return s_info; }
pg::sampler::FlashPool& pool() { return s_pool; }
pg::sampler::SampleWriter& writer() { return s_writer; }

}  // namespace samples
}  // namespace plat
