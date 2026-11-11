// pg::FileSystem backed by a real directory, for host tools and tests:
// "/int/..." maps to <root>/int/..., "/sd/..." to <root>/sd/...
// A mount is present when its directory exists (createMounts() makes them).
// open(forWrite) creates parent directories. rename() is POSIX rename
// (atomic replace). freeBytes() uses statvfs.
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

namespace pgtest {

class HostFs : public pg::FileSystem {
 public:
  explicit HostFs(std::string root) : root_(std::move(root)) {}
  ~HostFs() override {
    for (int& fd : fds_)
      if (fd >= 0) ::close(fd);
  }
  bool createMounts(bool withSd = true) {
    return mkdirs(root_ + "/int/pg") && (!withSd || mkdirs(root_ + "/sd/pg"));
  }
  std::string real(const char* path) const { return root_ + path; }

  bool mounted(const char* m) override {
    if (!m || (std::strcmp(m, "/int") != 0 && std::strcmp(m, "/sd") != 0)) return false;
    struct stat st;
    return ::stat(real(m).c_str(), &st) == 0 && S_ISDIR(st.st_mode);
  }
  bool read(const char* path, uint8_t* buf, size_t cap, size_t* len) override {
    if (!ok(path)) return false;
    FILE* f = std::fopen(real(path).c_str(), "rb");
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
  bool remove(const char* path) override { return ok(path) && ::unlink(real(path).c_str()) == 0; }
  bool exists(const char* path) override {
    struct stat st;
    return ok(path) && ::stat(real(path).c_str(), &st) == 0 && S_ISREG(st.st_mode);
  }
  int list(const char* dir, void (*cb)(const char*, size_t, void*), void* user) override {
    if (!ok(dir)) return -1;
    const std::string d = real(dir);
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
    if (!ok(path)) return -1;
    const std::string p = real(path);
    if (forWrite) {
      if (!mounted(std::strncmp(path, "/sd", 3) == 0 ? "/sd" : "/int")) return -1;  // never create a mount
      const size_t slash = p.rfind('/');
      if (slash != std::string::npos && !mkdirs(p.substr(0, slash))) return -1;
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
    return ok(from) && ok(to) && ::rename(real(from).c_str(), real(to).c_str()) == 0;
  }
  long freeBytes(const char* m) override {
    if (!mounted(m)) return -1;
    struct statvfs sv;
    if (::statvfs(real(m).c_str(), &sv) != 0) return -1;
    const unsigned long long b = (unsigned long long)sv.f_bavail * sv.f_frsize;
    return b > 0x7FFFFFFFull ? 0x7FFFFFFFL : long(b);
  }

 private:
  static constexpr int kMaxHandles = 4;
  std::string root_;
  int fds_[kMaxHandles] = {-1, -1, -1, -1};
  bool err_[kMaxHandles] = {};
  bool write_[kMaxHandles] = {};

  bool valid(int h) const { return h >= 0 && h < kMaxHandles && fds_[h] >= 0; }
  // Absolute, under a mount, no ".." components.
  static bool ok(const char* path) {
    if (!path) return false;
    if (std::strncmp(path, "/int", 4) != 0 && std::strncmp(path, "/sd", 3) != 0) return false;
    return std::strstr(path, "..") == nullptr;
  }
  static bool mkdirs(const std::string& dir) {
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
    return true;
  }
};

}  // namespace pgtest
