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
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

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

bool mkdirsVfs(const char* vpath, bool lastIsFile);

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
    mkdirsVfs("/sd/pg", false);
  } else {
    s_sdType = "none";
    s_sdCard = s_sdTotal = s_sdUsed = 0;
  }
  s_sdMountMs = millis() - t0;
  s_sd = st;
  s_sdTask = nullptr;
  vTaskDelete(nullptr);
}

// ---- pg::FileSystem over the ESP-IDF VFS -------------------------------------
// Everything goes through POSIX calls on the VFS mount points ("/littlefs" for
// LittleFS, "/sd" for FAT) instead of Arduino's File class, because only the
// POSIX calls report errors: fs::File::close() and flush() return void, so a
// write that fails when LittleFS commits the file at close (disk full, flash
// error) would go unnoticed. pg::io relies on close() reporting that.
//
// Semantics pg::io relies on (lib/pg/src/pg/io/project_io.h, platform.h):
//  * open(path, forWrite): forWrite truncates/creates, creating missing parent
//    directories ("/int/pg", "/sd/pg"). Handle >= 0 or -1.
//  * readSome: bytes read, 0 = EOF, -1 = error. writeSome: all-or-error.
//  * close: false if ANY write on that handle failed, or the final sync failed.
//  * rename(from, to) replaces `to`. LittleFS does that atomically; FAT
//    refuses an existing target (FR_EXIST), so there the target is removed
//    first (the non-atomic window is what io::recoverAfterPowerLoss()
//    repairs; the app runs it again when the card mounts after the UI boot).
//  * list(dir): base names of regular files only, with sizes.
//  * freeBytes(mount): -1 if unknown.
constexpr int kMaxHandles = 3;      // pg::io needs one; settings + a listing leave headroom
constexpr size_t kVfsPathMax = 96;

struct Handle {
  int  fd = -1;
  bool write = false;
  bool err = false;
};
Handle s_h[kMaxHandles];

// "/int/a/b" -> "/littlefs/a/b", "/sd/a/b" -> "/sd/a/b". false if the mount is
// unknown, not mounted, or the path does not fit.
bool vfsPath(const char* path, char* out, size_t cap) {
  if (!path) return false;
  const char* rest = nullptr;
  const char* base = nullptr;
  if (strncmp(path, "/int", 4) == 0 && (path[4] == '/' || path[4] == 0)) {
    if (!s_intMounted) return false;
    base = "/littlefs";
    rest = path + 4;
  } else if (strncmp(path, "/sd", 3) == 0 && (path[3] == '/' || path[3] == 0)) {
    if (s_sd != SdState::Mounted) return false;
    base = "/sd";
    rest = path + 3;
  } else {
    return false;
  }
  const int n = snprintf(out, cap, "%s%s", base, rest);
  return n > 0 && static_cast<size_t>(n) < cap;
}

bool isDirVfs(const char* vpath) {
  struct stat st;
  return ::stat(vpath, &st) == 0 && S_ISDIR(st.st_mode);
}

// mkdir -p for every directory component of vpath below the mount point
// (`lastIsFile`: the final component is a file name and is not created).
bool mkdirsVfs(const char* vpath, bool lastIsFile) {
  char tmp[kVfsPathMax];
  strlcpy(tmp, vpath, sizeof(tmp));
  const size_t len = strlen(tmp);
  // skip the mount point itself ("/littlefs" or "/sd")
  size_t i = 1;
  while (i < len && tmp[i] != '/') ++i;
  for (++i; i <= len; ++i) {
    if (tmp[i] == '/' || tmp[i] == 0) {
      if (tmp[i] == 0 && lastIsFile) break;
      const char c = tmp[i];
      tmp[i] = 0;
      if (!isDirVfs(tmp) && ::mkdir(tmp, 0775) != 0 && !isDirVfs(tmp)) return false;
      tmp[i] = c;
    }
  }
  return true;
}

Handle* handle(int h) {
  if (h < 0 || h >= kMaxHandles || s_h[h].fd < 0) return nullptr;
  return &s_h[h];
}

class VfsFs final : public pg::FileSystem {
 public:
  bool mounted(const char* mount) override {
    if (!mount) return false;
    if (strcmp(mount, "/int") == 0) return s_intMounted;
    if (strcmp(mount, "/sd") == 0) return s_sd == SdState::Mounted;
    return false;
  }

  int open(const char* path, bool forWrite) override {
    char vp[kVfsPathMax];
    if (!vfsPath(path, vp, sizeof(vp))) return -1;
    int slot = -1;
    for (int i = 0; i < kMaxHandles; ++i) if (s_h[i].fd < 0) { slot = i; break; }
    if (slot < 0) return -1;
    int fd;
    if (forWrite) {
      if (!mkdirsVfs(vp, true)) return -1;
      fd = ::open(vp, O_WRONLY | O_CREAT | O_TRUNC, 0664);
    } else {
      if (isDirVfs(vp)) return -1;
      fd = ::open(vp, O_RDONLY);
    }
    if (fd < 0) return -1;
    s_h[slot].fd = fd;
    s_h[slot].write = forWrite;
    s_h[slot].err = false;
    return slot;
  }
  int readSome(int h, uint8_t* buf, size_t cap) override {
    Handle* f = handle(h);
    if (!f || f->write || !buf) return -1;
    if (cap == 0) return 0;
    const ssize_t n = ::read(f->fd, buf, cap);
    return n < 0 ? -1 : static_cast<int>(n);
  }
  bool writeSome(int h, const uint8_t* buf, size_t len) override {
    Handle* f = handle(h);
    if (!f || !f->write) return false;
    if (f->err) return false;
    size_t done = 0;
    while (done < len) {
      const ssize_t n = ::write(f->fd, buf + done, len - done);
      if (n <= 0) { f->err = true; return false; }
      done += static_cast<size_t>(n);
    }
    return true;
  }
  bool close(int h) override {
    Handle* f = handle(h);
    if (!f) return false;
    bool ok = !f->err;
    if (f->write && ::fsync(f->fd) != 0) ok = false;
    if (::close(f->fd) != 0) ok = false;
    f->fd = -1;
    f->write = false;
    f->err = false;
    return ok;
  }
  bool rename(const char* from, const char* to) override {
    char a[kVfsPathMax], b[kVfsPathMax];
    if (!vfsPath(from, a, sizeof(a)) || !vfsPath(to, b, sizeof(b))) return false;
    if (::rename(a, b) == 0) return true;
    // LittleFS (esp_littlefs 2.0.0 vfs_littlefs_rename -> lfs_rename) already
    // replaces an existing target atomically, so a failure there is real
    // (no space for the metadata commit, a file still open): deleting the
    // target and retrying would turn it into a non-atomic replace whose temp
    // file the next save then removes. Only FAT needs the remove + move.
    if (strncmp(to, "/sd/", 4) != 0) return false;
    struct stat st;
    if (::stat(a, &st) != 0 || ::stat(b, &st) != 0) return false;   // nothing to replace: a real failure
    if (::unlink(b) != 0) return false;                             // FAT: target must go first
    return ::rename(a, b) == 0;
  }
  long freeBytes(const char* mount) override {
    if (!mounted(mount)) return -1;
    uint64_t total = 0, used = 0;
    if (strcmp(mount, "/int") == 0) {
      total = LittleFS.totalBytes();
      used = LittleFS.usedBytes();
    } else {
      total = SD.totalBytes();
      used = SD.usedBytes();
    }
    if (total == 0 || used > total) return -1;
    const uint64_t fr = total - used;
    return fr > 0x7FFFFFFFull ? 0x7FFFFFFFL : static_cast<long>(fr);
  }

  bool read(const char* path, uint8_t* buf, size_t cap, size_t* len) override {
    if (len) *len = 0;
    const int h = open(path, false);
    if (h < 0) return false;
    size_t got = 0;
    bool ok = true;
    while (got < cap) {
      const int n = readSome(h, buf + got, cap - got);
      if (n < 0) { ok = false; break; }
      if (n == 0) break;
      got += static_cast<size_t>(n);
    }
    close(h);
    if (len) *len = got;
    return ok;
  }
  bool write(const char* path, const uint8_t* buf, size_t len) override {
    const int h = open(path, true);
    if (h < 0) return false;
    const bool w = writeSome(h, buf, len);
    const bool c = close(h);
    return w && c;
  }
  bool remove(const char* path) override {
    char vp[kVfsPathMax];
    return vfsPath(path, vp, sizeof(vp)) && ::unlink(vp) == 0;
  }
  bool exists(const char* path) override {
    char vp[kVfsPathMax];
    struct stat st;
    return vfsPath(path, vp, sizeof(vp)) && ::stat(vp, &st) == 0;
  }
  int list(const char* dir, void (*cb)(const char*, size_t, void*), void* user) override {
    char vp[kVfsPathMax];
    if (!vfsPath(dir, vp, sizeof(vp))) return -1;
    DIR* d = ::opendir(vp);
    if (!d) return -1;
    int n = 0;
    char full[kVfsPathMax + 260];   // + a full 255-char dirent name
    while (struct dirent* e = ::readdir(d)) {
      if (e->d_name[0] == '.' && (e->d_name[1] == 0 || (e->d_name[1] == '.' && e->d_name[2] == 0))) continue;
      // base name only (some VFS drivers have returned paths in the past)
      const char* name = strrchr(e->d_name, '/');
      name = name ? name + 1 : e->d_name;
      const int fl = snprintf(full, sizeof(full), "%s/%s", vp, name);
      if (fl <= 0 || static_cast<size_t>(fl) >= sizeof(full)) continue;
      struct stat st;
      if (::stat(full, &st) != 0 || !S_ISREG(st.st_mode)) continue;
      if (cb) cb(name, static_cast<size_t>(st.st_size), user);
      ++n;
    }
    ::closedir(d);
    return n;
  }
};

VfsFs s_fs;

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

bool beginInternal(void (*beforeFormat)(void*), void* user) {
  if (s_intMounted) return true;
  const uint32_t t0 = millis();
  s_intFormatted = false;
  // Try without formatting first so we can report whether a format happened,
  // and so the caller can put a "formatting" screen up before the long part.
  if (!LittleFS.begin(false, "/littlefs", 4, "spiffs")) {
    s_intFormatted = true;
    if (beforeFormat) beforeFormat(user);
    s_intMounted = LittleFS.begin(true, "/littlefs", 4, "spiffs");
  } else {
    s_intMounted = true;
  }
  if (s_intMounted) mkdirsVfs("/littlefs/pg", false);
  s_intMountMs = millis() - t0;
  refreshUsage();
  return s_intMounted;
}

bool formatInternal() {
  for (auto& h : s_h) {                          // no file may stay open across a format
    if (h.fd >= 0) { ::close(h.fd); h.fd = -1; }
  }
  const uint32_t t0 = millis();
  if (s_intMounted) LittleFS.end();
  s_intMounted = false;
  bool ok = LittleFS.format();
  s_intMounted = LittleFS.begin(!ok, "/littlefs", 4, "spiffs");
  if (s_intMounted) mkdirsVfs("/littlefs/pg", false);
  s_intFormatted = true;
  s_intMountMs = millis() - t0;
  refreshUsage();
  return ok && s_intMounted;
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
  // Core 1 (with the UI), at the UI loop's own priority (1), far below keys
  // (10) and audio (20). NOT above the UI: the Arduino SD driver busy-waits
  // without yielding (sdWait() up to 500 ms per command with MISO held low,
  // the ACMD41 loop up to 3 s while a card initialises), which at a higher
  // priority would freeze the UI loop for that long. At equal priority the
  // two time-slice every tick.
  // 8 KB: SD.begin + f_getfree through VFS/FATFS, and this build keeps FATFS
  // long-file-name buffers on the stack (CONFIG_FATFS_LFN_STACK, 255 chars).
  // Transient: the task deletes itself.
  if (xTaskCreatePinnedToCore(sdMountTask, "pg_sdmount", 8192, nullptr, 1, &s_sdTask, 1) != pdPASS) {
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
