// Pocket Groove platform layer -- storage: pg::FileSystem over
//   "/int"  LittleFS on the 1.5 MB "spiffs" data partition of default_8MB.csv
//           (formatted automatically if it does not mount, e.g. first boot or
//           a partition left in another format by a previous firmware)
//   "/sd"   microSD over SPI2 (SCK 40, MISO 39, MOSI 14, CS 12), optional.
//
// The SD mount runs on its own low-priority task on core 1 (sdMountAsync), so
// a missing card or a slow card never blocks the UI loop, and it never touches
// the audio core. All file operations are blocking and belong to ONE thread
// (UI or a worker), never the audio task.
//
// HARDWARE NOTE: writing or erasing internal flash stalls BOTH cores (the
// cache is off while the flash chip is busy). The audio DMA keeps playing what
// is already queued; anything longer than the DMA ring is an audible gap. The
// smoke test measures this; the instrument should save only when it can afford
// a gap (or use a deeper DMA ring while saving).
//
// No Arduino / M5 / ESP-IDF types in this header.
#pragma once
#include <cstddef>
#include <cstdint>

#include "pg/platform.h"

namespace plat {
namespace storage {

enum class SdState : uint8_t { Idle = 0, Mounting, Mounted, NoCard, Failed };
const char* sdStateName(SdState s);

struct Info {
  bool     intMounted;
  bool     intFormatted;     // had to format at mount (first boot / foreign data)
  uint32_t intMountMs;
  uint32_t intTotalBytes, intUsedBytes;
  SdState  sd;
  uint32_t sdMountMs;        // duration of the last mount attempt
  const char* sdType;        // "MMC", "SDSC", "SDHC", "none", "?"
  uint64_t sdCardBytes, sdTotalBytes, sdUsedBytes;
};

// Mounts "/int". Blocking; can take seconds on the first boot (format).
bool beginInternal();
// Starts (or restarts) the SD mount in the background. Returns immediately.
void sdMountAsync();
void sdUnmount();
SdState sdState();
// Cheap (cached values only): safe to call every frame.
Info info();
// Re-reads /int used/total bytes (walks the filesystem; call after writes, not per frame).
void refreshUsage();

// The FileSystem the app uses (paths start with "/int/" or "/sd/").
pg::FileSystem& fs();

}  // namespace storage
}  // namespace plat
