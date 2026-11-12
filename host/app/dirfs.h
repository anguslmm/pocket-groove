// pg::FileSystem over plain directories, laid out like tools/pg expects:
//
//   "/int/pg/<file>"  ->  <intDir>/<file>        (default ./projects: where
//                                                 `tools/pg pull` writes and
//                                                 `tools/pg push` reads)
//   "/sd/pg/<file>"   ->  <sdDir>/<file>         (only with --sd DIR; else no card)
//   "/int/<other>"    ->  <intDir>/.int/<other>  (pg::io uses only /int/pg today)
//
// So a project saved on the desktop is <dir>/NAME.pgv, byte for byte what the
// device writes for /int/pg/NAME.pgv (the same pg::io code), and the session
// and UI settings are <dir>/_last.pgv and <dir>/_ui.cfg. Writes go through
// pg::io's temp-file + verify + rename, as on the device; rename() is POSIX
// rename (atomic replace) and close() fsyncs.
#pragma once
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>

#include "pg/platform.h"

namespace desk {

inline bool makeDirs(const std::string& dir) {
  if (dir.empty()) return false;
  std::string cur;
  size_t i = 0;
  while (i <= dir.size()) {
    const size_t j = dir.find('/', i);
    const size_t end = j == std::string::npos ? dir.size() : j;
    cur = dir.substr(0, end);
    if (!cur.empty() && ::mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST) return false;
    if (j == std::string::npos) break;
    i = j + 1;
  }
  struct stat st;
  return ::stat(dir.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

class DirFs : public pg::FileSystem {
 public:
  // sdDir empty = no SD card.
  DirFs(std::string intDir, std::string sdDir) : int_(std::move(intDir)), sd_(std::move(sdDir)) {}
  ~DirFs() override {
    for (int& fd : fds_)
      if (fd >= 0) ::close(fd);
  }
  // Creates the directories (a mount is present when its directory exists).
  bool createMounts() { return makeDirs(int_) && (sd_.empty() || makeDirs(sd_)); }
  const std::string& intDir() const { return int_; }

  // The real path of a pg path, or "" if it is not under a mount.
  std::string real(const char* path) const {
    if (!path || std::strstr(path, "..")) return "";
    const char* rest = nullptr;
    const std::string* base = nullptr;
    if (std::strncmp(path, "/int", 4) == 0 && (path[4] == 0 || path[4] == '/')) {
      base = &int_;
      rest = path + 4;
    } else if (std::strncmp(path, "/sd", 3) == 0 && (path[3] == 0 || path[3] == '/')) {
      if (sd_.empty()) return "";
      base = &sd_;
      rest = path + 3;
    } else {
      return "";
    }
    if (*rest == 0) return *base;
    if (std::strcmp(rest, "/pg") == 0 || std::strcmp(rest, "/pg/") == 0) return *base;
    if (std::strncmp(rest, "/pg/", 4) == 0) return *base + "/" + (rest + 4);
    return *base + "/.int" + rest;
  }

  bool mounted(const char* m) override {
    if (!m) return false;
    if (std::strcmp(m, "/int") == 0) return isDir(int_);
    if (std::strcmp(m, "/sd") == 0) return !sd_.empty() && isDir(sd_);
    return false;
  }
  bool read(const char* path, uint8_t* buf, size_t cap, size_t* len) override {
    const std::string p = real(path);
    if (p.empty()) return false;
    FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) return false;
    const size_t n = std::fread(buf, 1, cap, f);
    std::fclose(f);
    if (len) *len = n;
    return true;
  }
  bool write(const char* path, const uint8_t* buf, size_t len) override {
    const int h = open(path, true);
    if (h < 0) return false;
    const bool w = writeSome(h, buf, len);
    return close(h) && w;
  }
  bool remove(const char* path) override {
    const std::string p = real(path);
    return !p.empty() && ::unlink(p.c_str()) == 0;
  }
  bool exists(const char* path) override {
    const std::string p = real(path);
    struct stat st;
    return !p.empty() && ::stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
  }
  int list(const char* dir, void (*cb)(const char*, size_t, void*), void* user) override {
    const std::string d = real(dir);
    if (d.empty()) return -1;
    DIR* dp = ::opendir(d.c_str());
    if (!dp) return -1;
    int n = 0;
    while (dirent* e = ::readdir(dp)) {
      struct stat st;
      const std::string p = d + "/" + e->d_name;
      if (::stat(p.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
      cb(e->d_name, size_t(st.st_size), user);
      ++n;
    }
    ::closedir(dp);
    return n;
  }
  int open(const char* path, bool forWrite) override {
    const std::string p = real(path);
    if (p.empty()) return -1;
    if (forWrite) {
      if (!mounted(std::strncmp(path, "/sd", 3) == 0 ? "/sd" : "/int")) return -1;   // never create a mount
      const size_t slash = p.rfind('/');
      if (slash != std::string::npos && !makeDirs(p.substr(0, slash))) return -1;
    }
    int slot = -1;
    for (int i = 0; i < kMaxHandles; ++i)
      if (fds_[i] < 0) { slot = i; break; }
    if (slot < 0) return -1;
    const int fd = forWrite ? ::open(p.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644) : ::open(p.c_str(), O_RDONLY);
    if (fd < 0) return -1;
    fds_[slot] = fd;
    err_[slot] = false;
    write_[slot] = forWrite;
    return slot;
  }
  int readSome(int h, uint8_t* buf, size_t cap) override {
    if (!valid(h) || write_[h]) return -1;
    for (;;) {
      const ssize_t n = ::read(fds_[h], buf, cap);
      if (n < 0 && errno == EINTR) continue;
      return n < 0 ? -1 : int(n);
    }
  }
  bool writeSome(int h, const uint8_t* buf, size_t len) override {
    if (!valid(h) || !write_[h]) return false;
    while (len) {
      const ssize_t n = ::write(fds_[h], buf, len);
      if (n < 0 && errno == EINTR) continue;
      if (n <= 0) { err_[h] = true; return false; }
      buf += n;
      len -= size_t(n);
    }
    return true;
  }
  bool close(int h) override {
    if (!valid(h)) return false;
    bool good = !err_[h];
    if (write_[h] && ::fsync(fds_[h]) != 0) good = false;
    if (::close(fds_[h]) != 0) good = false;
    fds_[h] = -1;
    return good;
  }
  bool rename(const char* from, const char* to) override {
    const std::string a = real(from), b = real(to);
    return !a.empty() && !b.empty() && ::rename(a.c_str(), b.c_str()) == 0;
  }
  long freeBytes(const char* m) override {
    if (!mounted(m)) return -1;
    struct statvfs sv;
    if (::statvfs(std::strcmp(m, "/sd") == 0 ? sd_.c_str() : int_.c_str(), &sv) != 0) return -1;
    const unsigned long long b = (unsigned long long)sv.f_bavail * sv.f_frsize;
    return b > 0x7FFFFFFFull ? 0x7FFFFFFFL : long(b);
  }

 private:
  static constexpr int kMaxHandles = 4;
  std::string int_, sd_;
  int fds_[kMaxHandles] = {-1, -1, -1, -1};
  bool err_[kMaxHandles] = {};
  bool write_[kMaxHandles] = {};

  bool valid(int h) const { return h >= 0 && h < kMaxHandles && fds_[h] >= 0; }
  static bool isDir(const std::string& d) {
    struct stat st;
    return ::stat(d.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
  }
};

}  // namespace desk
