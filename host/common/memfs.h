// In-memory pg::FileSystem for host tests: "/int" and "/sd" (removable), a
// flat path -> bytes map, and fault injection.
//
// Semantics modelled on LittleFS:
//   * open(forWrite) creates/truncates the file immediately (empty);
//     written data becomes visible on close().
//   * rename() replaces the target atomically (renameAtomic = false models a
//     FAT-like remove-then-move that power loss can split).
//   * Directories are implicit: list(dir) returns the files directly in dir.
//
// Faults (all counted from arm(), over "mutating ops" = open-for-write,
// writeSome, close of a write handle, rename (2 ops if non-atomic), remove):
//   failOp = k        op k fails (returns false/-1), the device keeps working.
//   powerLossOp = k   power dies when op k starts: a writeSome persists
//                     `powerLossKeep` of its data according to lossMode, and
//                     every later op fails until powerCycle().
//   spaceLimit[m]     bytes per mount; writes beyond fail. freeBytes reports it
//                     unless reportFree = false.
//   shortReadMax      readSome returns at most this many bytes (0 = no limit);
//                     with shortReadRandom, a random 1..shortReadMax.
//   readFailCall = k  the k-th readSome call (from arm) returns -1.
#pragma once
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "pg/platform.h"

namespace pgtest {

class MemFs : public pg::FileSystem {
 public:
  enum class LossMode { Discard, KeepPrefix, KeepPrefixGarbage };

  std::map<std::string, std::vector<uint8_t>> files;
  bool intPresent = true, sdPresent = true;
  long spaceLimit[2] = {-1, -1};  // [0] = /int, [1] = /sd
  bool reportFree = true;
  bool renameAtomic = true;
  int failOp = -1, powerLossOp = -1;
  LossMode lossMode = LossMode::Discard;
  size_t shortReadMax = 0;
  bool shortReadRandom = false;
  int readFailCall = -1;
  uint32_t rng = 12345;

  // Statistics since arm()
  int ops = 0, readCalls = 0, writeCalls = 0;
  size_t maxWriteLen = 0, maxOpenHandles = 0;
  bool dead = false;

  void arm() { ops = 0; readCalls = 0; writeCalls = 0; maxWriteLen = 0; dead = false; }
  void clearFaults() { failOp = powerLossOp = readFailCall = -1; shortReadMax = 0; shortReadRandom = false; }
  // After power loss: reboot (handles lost, device works again).
  void powerCycle() { handles_.clear(); dead = false; clearFaults(); arm(); }

  bool has(const std::string& p) const { return files.count(p) != 0; }
  const std::vector<uint8_t>* get(const std::string& p) const {
    auto it = files.find(p);
    return it == files.end() ? nullptr : &it->second;
  }
  void corrupt(const std::string& p, size_t off, uint8_t xorMask) { files[p][off] ^= xorMask; }
  void truncate(const std::string& p, size_t len) { files[p].resize(len); }
  size_t openHandles() const { return handles_.size(); }

  // ---- pg::FileSystem ----
  bool mounted(const char* m) override {
    const int i = mountIndex(m, true);
    return i == 0 ? intPresent : (i == 1 ? sdPresent : false);
  }
  bool read(const char* path, uint8_t* buf, size_t cap, size_t* len) override {
    if (!present(path) || dead) return false;
    auto it = files.find(path);
    if (it == files.end()) return false;
    const size_t n = it->second.size() < cap ? it->second.size() : cap;
    if (n) std::memcpy(buf, it->second.data(), n);
    if (len) *len = n;
    return true;
  }
  bool write(const char* path, const uint8_t* buf, size_t len) override {
    if (!present(path) || !mutate()) return false;
    if (!fits(path, len, files.count(path) ? files[path].size() : 0)) return false;
    files[path].assign(buf, buf + len);
    return true;
  }
  bool remove(const char* path) override {
    if (!present(path) || !mutate()) return false;
    return files.erase(path) != 0;
  }
  bool exists(const char* path) override { return present(path) && !dead && files.count(path) != 0; }
  int list(const char* dir, void (*cb)(const char*, size_t, void*), void* user) override {
    if (!present(dir) || dead) return -1;
    std::string pre = std::string(dir) + "/";
    int n = 0;
    std::vector<std::pair<std::string, size_t>> found;
    for (auto& f : files)
      if (f.first.compare(0, pre.size(), pre) == 0 && f.first.find('/', pre.size()) == std::string::npos)
        found.emplace_back(f.first.substr(pre.size()), f.second.size());
    for (auto& f : found) { cb(f.first.c_str(), f.second, user); ++n; }
    return n;
  }
  int open(const char* path, bool forWrite) override {
    if (!present(path) || dead) return -1;
    if (forWrite) {
      if (!mutate()) return -1;
      files[path].clear();  // created/truncated at open, like LittleFS
    } else if (!files.count(path)) {
      return -1;
    }
    const int h = nextHandle_++;
    Handle& hd = handles_[h];
    hd.path = path;
    hd.write = forWrite;
    hd.pos = 0;
    if (handles_.size() > maxOpenHandles) maxOpenHandles = handles_.size();
    return h;
  }
  int readSome(int h, uint8_t* buf, size_t cap) override {
    auto it = handles_.find(h);
    if (it == handles_.end() || it->second.write || dead || !present(it->second.path.c_str())) return -1;
    const int call = readCalls++;
    if (call == readFailCall) return -1;
    auto f = files.find(it->second.path);
    if (f == files.end()) return -1;
    size_t n = f->second.size() - it->second.pos;
    if (n > cap) n = cap;
    if (shortReadMax && n > shortReadMax) n = shortReadRandom ? 1 + next() % shortReadMax : shortReadMax;
    if (n) std::memcpy(buf, f->second.data() + it->second.pos, n);
    it->second.pos += n;
    return int(n);
  }
  bool writeSome(int h, const uint8_t* buf, size_t len) override {
    auto it = handles_.find(h);
    if (it == handles_.end() || !it->second.write || !present(it->second.path.c_str())) return false;
    ++writeCalls;
    if (len > maxWriteLen) maxWriteLen = len;
    Handle& hd = it->second;
    if (!dead && ops == powerLossOp) {  // power dies during this write
      hd.pending.insert(hd.pending.end(), buf, buf + len / 2);
      powerLoss();
      return false;
    }
    if (!mutate()) { hd.failed = true; return false; }
    if (!fits(hd.path.c_str(), hd.pending.size() + len, 0)) { hd.failed = true; return false; }
    hd.pending.insert(hd.pending.end(), buf, buf + len);
    return true;
  }
  bool close(int h) override {
    auto it = handles_.find(h);
    if (it == handles_.end()) return false;
    Handle hd = it->second;
    handles_.erase(it);
    if (!hd.write) return !dead;
    if (!mutate()) return false;  // data never committed: file stays empty
    if (hd.failed) return false;
    files[hd.path] = hd.pending;
    return true;
  }
  bool rename(const char* from, const char* to) override {
    if (!present(from) || !present(to) || mountIndex(from, false) != mountIndex(to, false)) return false;
    if (!mutate()) return false;
    auto it = files.find(from);
    if (it == files.end()) return false;
    if (!renameAtomic) {
      files.erase(to);
      if (!mutate()) return false;  // power loss / fault between remove and move
      it = files.find(from);
    }
    std::vector<uint8_t> data = std::move(it->second);
    files.erase(it);
    files[to] = std::move(data);
    return true;
  }
  long freeBytes(const char* m) override {
    const int i = mountIndex(m, true);
    if (i < 0 || !reportFree || spaceLimit[i] < 0 || dead) return -1;
    return spaceLimit[i] - long(used(i));
  }

 private:
  struct Handle {
    std::string path;
    bool write = false, failed = false;
    size_t pos = 0;
    std::vector<uint8_t> pending;
  };
  std::map<int, Handle> handles_;
  int nextHandle_ = 1;

  uint32_t next() { rng = rng * 1664525u + 1013904223u; return rng >> 8; }

  static int mountIndex(const char* p, bool exact) {
    if (!p) return -1;
    if (exact) {
      if (std::strcmp(p, "/int") == 0) return 0;
      if (std::strcmp(p, "/sd") == 0) return 1;
    }
    if (std::strncmp(p, "/int/", 5) == 0 || std::strcmp(p, "/int") == 0) return 0;
    if (std::strncmp(p, "/sd/", 4) == 0 || std::strcmp(p, "/sd") == 0) return 1;
    return -1;
  }
  bool present(const char* path) {
    const int i = mountIndex(path, false);
    return i == 0 ? intPresent : (i == 1 ? sdPresent : false);
  }
  size_t used(int mount) const {
    size_t u = 0;
    for (auto& f : files)
      if (mountIndex(f.first.c_str(), false) == mount) u += f.second.size();
    for (auto& h : handles_)
      if (mountIndex(h.second.path.c_str(), false) == mount) u += h.second.pending.size();
    return u;
  }
  bool fits(const char* path, size_t newLen, size_t replaced) const {
    const int i = mountIndex(path, false);
    if (i < 0 || spaceLimit[i] < 0) return true;
    // newLen includes this handle's pending bytes, which used() also counts.
    size_t u = used(i) - replaced;
    for (auto& h : handles_)
      if (h.second.path == path) u -= h.second.pending.size();
    return u + newLen <= size_t(spaceLimit[i]);
  }
  // Counts a mutating op; false if it must fail.
  bool mutate() {
    if (dead) return false;
    const int op = ops++;
    if (op == powerLossOp) { powerLoss(); return false; }
    return op != failOp;
  }
  void powerLoss() {
    dead = true;
    for (auto& h : handles_) {
      if (!h.second.write) continue;
      std::vector<uint8_t>& f = files[h.second.path];
      switch (lossMode) {
        case LossMode::Discard: f.clear(); break;
        case LossMode::KeepPrefix: f = h.second.pending; break;
        case LossMode::KeepPrefixGarbage:
          f = h.second.pending;
          for (int i = 0; i < 37; ++i) f.push_back(uint8_t(next()));
          break;
      }
    }
  }
};

}  // namespace pgtest
