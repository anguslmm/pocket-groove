// /int on the Daisy: a file system in RAM (SDRAM), kept on the QSPI flash by
// snapshots (fs_persist.h). The Seed has no LittleFS partition and libDaisy no
// flash file system; projects are small (one is at most ~69 KB) and SDRAM is 64 MB,
// so the live files sit in RAM, where reads and writes are instant and never
// touch the flash, and the persistence writes a consistent copy of the whole
// set in the background.
//
// Semantics follow what pg::io relies on (and host/common/memfs.h models): open for
// write creates / truncates the file at once and the data becomes the file's at
// close(); rename() replaces the target atomically; directories are implicit (list
// returns the files directly in a directory). No heap: a fixed table of kMaxFiles
// files, file i keeps its bytes in arena slot i (slotBytes each). The capacity is
// the smaller of the arena and the persistence budget: the snapshot of all files
// (serializedSize()) must fit one flash bank, so a write that would make it larger
// fails, as a full flash would, and freeBytes() reports what is left of it.
// Portable (host/daisy/test_daisy_fs.cpp).
#pragma once
#include <cstddef>
#include <cstdint>

#include "pg/io/project_io.h"
#include "pg/platform.h"

namespace pgd {

class RamFs final : public pg::FileSystem {
 public:
  static constexpr int kMaxFiles = 48;
  static constexpr int kMaxHandles = 2;
  static constexpr int kPathMax = pg::io::kPathMax;
  static constexpr uint32_t kFormatVersion = 1;

  // mount: "/int". arena: kMaxFiles * slotBytes bytes. budget: the largest
  // serializedSize() allowed (the snapshot bank's payload capacity).
  void init(const char* mount, uint8_t* arena, uint32_t slotBytes, uint32_t budget);
  void clear();

  // Every successful change bumps it (the persistence watches it).
  uint32_t revision() const { return rev_; }
  int fileCount() const;
  uint32_t usedBytes() const;          // file data
  uint32_t serializedSize() const;     // what serialize() writes now
  uint32_t budget() const { return budget_; }
  // The whole file set: "PGRF", version, count, then per file u16 path length, path,
  // u32 size, data. Returns the byte count, 0 if it does not fit cap.
  uint32_t serialize(uint8_t* out, uint32_t cap) const;
  // Replaces the content with a serialized set. false (and the FS left empty) if
  // it is malformed or does not fit.
  bool deserialize(const uint8_t* in, uint32_t n);

  // ---- pg::FileSystem ----
  bool mounted(const char* mount) override;
  bool read(const char* path, uint8_t* buf, size_t cap, size_t* len) override;
  bool write(const char* path, const uint8_t* buf, size_t len) override;
  bool remove(const char* path) override;
  bool exists(const char* path) override;
  int list(const char* dir, void (*cb)(const char* name, size_t size, void* user), void* user) override;
  int open(const char* path, bool forWrite) override;
  int readSome(int h, uint8_t* buf, size_t cap) override;
  bool writeSome(int h, const uint8_t* buf, size_t len) override;
  bool close(int h) override;
  bool rename(const char* from, const char* to) override;
  long freeBytes(const char* mount) override;

 private:
  struct File {
    bool used;
    bool writing;      // an open write handle: size is 0 until close
    char path[kPathMax];
    uint32_t size;
  };
  struct Handle {
    bool open, write, failed;
    int file;
    uint32_t pos;
  };
  bool ours(const char* path) const;
  int find(const char* path) const;
  int create(const char* path);
  uint8_t* data(int i) const { return arena_ + static_cast<size_t>(i) * slot_; }
  static uint32_t entryBytes(const char* path, uint32_t size);
  uint32_t serializedWith(int file, uint32_t newSize) const;   // serializedSize() if file had newSize

  char mount_[8] = {};
  size_t mountLen_ = 0;
  uint8_t* arena_ = nullptr;
  uint32_t slot_ = 0, budget_ = 0;
  uint32_t rev_ = 0;
  File f_[kMaxFiles] = {};
  Handle h_[kMaxHandles] = {};
};

// Routes "/int..." and "/sd..." to two file systems (sd may be nullptr: no card).
class MountFs final : public pg::FileSystem {
 public:
  void set(pg::FileSystem* intFs, pg::FileSystem* sdFs) {
    int_ = intFs;
    sd_ = sdFs;
  }
  pg::FileSystem* sd() const { return sd_; }
  bool mounted(const char* mount) override;
  bool read(const char* path, uint8_t* buf, size_t cap, size_t* len) override;
  bool write(const char* path, const uint8_t* buf, size_t len) override;
  bool remove(const char* path) override;
  bool exists(const char* path) override;
  int list(const char* dir, void (*cb)(const char* name, size_t size, void* user), void* user) override;
  int open(const char* path, bool forWrite) override;
  int readSome(int h, uint8_t* buf, size_t cap) override;
  bool writeSome(int h, const uint8_t* buf, size_t len) override;
  bool close(int h) override;
  bool rename(const char* from, const char* to) override;
  long freeBytes(const char* mount) override;

 private:
  static constexpr int kSdTag = 0x4000;
  pg::FileSystem* route(const char* path) const;
  pg::FileSystem* byHandle(int h, int* inner) const;
  pg::FileSystem* int_ = nullptr;
  pg::FileSystem* sd_ = nullptr;
};

}  // namespace pgd
