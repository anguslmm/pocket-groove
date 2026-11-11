// Platform layer: storage. See storage.h.
//
// Checked against arduino-esp32 3.3.12 libraries/LittleFS (begin(formatOnFail,
// basePath, maxOpenFiles, partitionLabel = "spiffs")) and libraries/SD
// (begin(ssPin, SPIClass&, frequency, mountpoint, max_files, format_if_empty)).
// With board m5stack-stamps3 the default SPI pins are -1, so they are passed
// explicitly. Arduino's SPI object is SPI2 (FSPI); M5GFX drives the LCD on
// SPI3, so the two never share a controller.
#include "storage.h"

#include <Arduino.h>
#include <FS.h>
#include <LittleFS.h>
#include <SD.h>
#include <SPI.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cstring>

namespace plat {
namespace storage {

namespace {

constexpr int kSdSck = 40, kSdMiso = 39, kSdMosi = 14, kSdCs = 12;
constexpr int kExtLoraCs = 5;                    // EXT header / LoRa cap NSS, shares the SD bus
constexpr uint32_t kSdHz = 20000000;

bool     s_intMounted = false, s_intFormatted = false;
uint32_t s_intMountMs = 0;

uint32_t s_intTotal = 0, s_intUsed = 0;

// Cached SD facts, filled by the mount task. totalBytes()/usedBytes() are NOT
// cheap (FAT free-cluster scan on big cards, flash reads for LittleFS with the
// cache off, which stalls the audio core), so nothing calls them per frame.
const char* s_sdType = "none";
uint64_t s_sdCard = 0, s_sdTotal = 0, s_sdUsed = 0;

volatile SdState s_sd = SdState::Idle;
volatile uint32_t s_sdMountMs = 0;
TaskHandle_t s_sdTask = nullptr;
bool s_spiStarted = false;

void sdMountTask(void*) {
  const uint32_t t0 = millis();
  if (!s_spiStarted) {
    // A LoRa cap (or anything else) on the EXT header would see SD traffic on
    // its own lines; keep its chip-select weakly high. Harmless when empty.
    pinMode(kExtLoraCs, INPUT_PULLUP);
    pinMode(kSdCs, OUTPUT);
    digitalWrite(kSdCs, HIGH);
    SPI.begin(kSdSck, kSdMiso, kSdMosi, kSdCs);
    s_spiStarted = true;
  }
  SD.end();
  bool ok = SD.begin(kSdCs, SPI, kSdHz, "/sd", 5, false);
  SdState st;
  if (!ok) st = SdState::Failed;
  else if (SD.cardType() == CARD_NONE) { SD.end(); st = SdState::NoCard; }
  else st = SdState::Mounted;
  if (st == SdState::Mounted) {
    switch (SD.cardType()) {
      case CARD_MMC:  s_sdType = "MMC";  break;
      case CARD_SD:   s_sdType = "SDSC"; break;
      case CARD_SDHC: s_sdType = "SDHC"; break;
      default:        s_sdType = "?";    break;
    }
    s_sdCard = SD.cardSize();
    s_sdTotal = SD.totalBytes();
    s_sdUsed = SD.usedBytes();
  } else {
    s_sdType = "none";
    s_sdCard = s_sdTotal = s_sdUsed = 0;
  }
  s_sdMountMs = millis() - t0;
  s_sd = st;
  s_sdTask = nullptr;
  vTaskDelete(nullptr);
}

// Splits "/int/a/b" -> (&LittleFS, "/a/b"). Returns nullptr if the mount is
// unknown or not mounted.
fs::FS* resolve(const char* path, const char** rest) {
  if (!path) return nullptr;
  if (strncmp(path, "/int", 4) == 0 && (path[4] == '/' || path[4] == 0)) {
    if (!s_intMounted) return nullptr;
    *rest = path[4] ? path + 4 : "/";
    return &LittleFS;
  }
  if (strncmp(path, "/sd", 3) == 0 && (path[3] == '/' || path[3] == 0)) {
    if (s_sd != SdState::Mounted) return nullptr;
    *rest = path[3] ? path + 3 : "/";
    return &SD;
  }
  return nullptr;
}

class ArduinoFs final : public pg::FileSystem {
 public:
  bool mounted(const char* mount) override {
    if (!mount) return false;
    if (strcmp(mount, "/int") == 0) return s_intMounted;
    if (strcmp(mount, "/sd") == 0) return s_sd == SdState::Mounted;
    return false;
  }
  bool read(const char* path, uint8_t* buf, size_t cap, size_t* len) override {
    const char* p = nullptr;
    fs::FS* f = resolve(path, &p);
    if (len) *len = 0;
    if (!f) return false;
    File file = f->open(p, FILE_READ);
    if (!file || file.isDirectory()) return false;
    size_t got = 0;
    while (got < cap) {
      const size_t n = file.read(buf + got, cap - got);
      if (n == 0) break;
      got += n;
    }
    file.close();
    if (len) *len = got;
    return true;
  }
  bool write(const char* path, const uint8_t* buf, size_t len) override {
    const char* p = nullptr;
    fs::FS* f = resolve(path, &p);
    if (!f) return false;
    File file = f->open(p, FILE_WRITE);
    if (!file) return false;
    size_t done = 0;
    while (done < len) {
      const size_t n = file.write(buf + done, len - done);
      if (n == 0) break;
      done += n;
    }
    file.close();
    return done == len;
  }
  bool remove(const char* path) override {
    const char* p = nullptr;
    fs::FS* f = resolve(path, &p);
    return f && f->remove(p);
  }
  bool exists(const char* path) override {
    const char* p = nullptr;
    fs::FS* f = resolve(path, &p);
    return f && f->exists(p);
  }
  int list(const char* dir, void (*cb)(const char*, size_t, void*), void* user) override {
    const char* p = nullptr;
    fs::FS* f = resolve(dir, &p);
    if (!f) return -1;
    File d = f->open(p);
    if (!d || !d.isDirectory()) return -1;
    int n = 0;
    for (File e = d.openNextFile(); e; e = d.openNextFile()) {
      if (!e.isDirectory()) {
        if (cb) cb(e.name(), e.size(), user);
        ++n;
      }
      e.close();
    }
    d.close();
    return n;
  }
};

ArduinoFs s_fs;

}  // namespace

const char* sdStateName(SdState s) {
  switch (s) {
    case SdState::Idle:     return "not tried";
    case SdState::Mounting: return "mounting";
    case SdState::Mounted:  return "mounted";
    case SdState::NoCard:   return "no card";
    case SdState::Failed:   return "no card/failed";
  }
  return "?";
}

bool beginInternal() {
  if (s_intMounted) return true;
  const uint32_t t0 = millis();
  s_intFormatted = false;
  // Try without formatting first so we can report whether a format happened.
  if (!LittleFS.begin(false, "/littlefs", 4, "spiffs")) {
    s_intFormatted = true;
    s_intMounted = LittleFS.begin(true, "/littlefs", 4, "spiffs");
  } else {
    s_intMounted = true;
  }
  s_intMountMs = millis() - t0;
  refreshUsage();
  return s_intMounted;
}

void refreshUsage() {
  if (s_intMounted) {
    s_intTotal = static_cast<uint32_t>(LittleFS.totalBytes());
    s_intUsed = static_cast<uint32_t>(LittleFS.usedBytes());
  }
}

void sdMountAsync() {
  if (s_sdTask || s_sd == SdState::Mounting) return;
  s_sd = SdState::Mounting;
  // Core 1 (with the UI), priority just above the UI loop so it finishes
  // promptly, far below keys (10) and audio (20). The SD driver's waits are
  // blocking delays, so the UI keeps running in between.
  // 8 KB: SD.begin + f_getfree through VFS/FATFS, and this build keeps FATFS
  // long-file-name buffers on the stack (CONFIG_FATFS_LFN_STACK, 255 chars).
  // Transient: the task deletes itself.
  if (xTaskCreatePinnedToCore(sdMountTask, "pg_sdmount", 8192, nullptr, 2, &s_sdTask, 1) != pdPASS) {
    s_sdTask = nullptr;
    s_sd = SdState::Failed;
  }
}

void sdUnmount() {
  if (s_sd == SdState::Mounting) return;
  if (s_sd == SdState::Mounted) SD.end();
  s_sd = SdState::Idle;
}

SdState sdState() { return s_sd; }

Info info() {
  Info in;
  in.intMounted = s_intMounted;
  in.intFormatted = s_intFormatted;
  in.intMountMs = s_intMountMs;
  in.intTotalBytes = s_intTotal;
  in.intUsedBytes = s_intUsed;
  in.sd = s_sd;
  in.sdMountMs = s_sdMountMs;
  const bool m = (s_sd == SdState::Mounted);
  in.sdType = m ? s_sdType : "none";
  in.sdCardBytes = m ? s_sdCard : 0;
  in.sdTotalBytes = m ? s_sdTotal : 0;
  in.sdUsedBytes = m ? s_sdUsed : 0;
  return in;
}

pg::FileSystem& fs() { return s_fs; }

}  // namespace storage
}  // namespace plat
