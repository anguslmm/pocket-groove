#include "ramfs.h"

#include <cstring>

namespace pgd {

namespace {

constexpr uint8_t kMagic[4] = {'P', 'G', 'R', 'F'};
constexpr uint32_t kHeaderBytes = 12;   // magic, version, count

void put32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v);
  p[1] = uint8_t(v >> 8);
  p[2] = uint8_t(v >> 16);
  p[3] = uint8_t(v >> 24);
}
uint32_t get32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

}  // namespace

void RamFs::init(const char* mount, uint8_t* arena, uint32_t slotBytes, uint32_t budget) {
  std::strncpy(mount_, mount ? mount : "/int", sizeof(mount_) - 1);
  mount_[sizeof(mount_) - 1] = 0;
  mountLen_ = std::strlen(mount_);
  arena_ = arena;
  slot_ = slotBytes;
  budget_ = budget;
  clear();
}

void RamFs::clear() {
  for (File& f : f_) f = File{};
  for (Handle& h : h_) h = Handle{};
  ++rev_;
}

bool RamFs::ours(const char* path) const {
  return arena_ && path && std::strncmp(path, mount_, mountLen_) == 0 && path[mountLen_] == '/' &&
         path[mountLen_ + 1] != 0 && std::strlen(path) < size_t(kPathMax);
}

int RamFs::find(const char* path) const {
  for (int i = 0; i < kMaxFiles; ++i)
    if (f_[i].used && std::strcmp(f_[i].path, path) == 0) return i;
  return -1;
}

int RamFs::create(const char* path) {
  for (int i = 0; i < kMaxFiles; ++i) {
    if (f_[i].used) continue;
    f_[i] = File{};
    f_[i].used = true;
    std::memcpy(f_[i].path, path, std::strlen(path) + 1);   // ours(): shorter than kPathMax
    return i;
  }
  return -1;
}

uint32_t RamFs::entryBytes(const char* path, uint32_t size) { return 2u + uint32_t(std::strlen(path)) + 4u + size; }

int RamFs::fileCount() const {
  int n = 0;
  for (const File& f : f_) n += f.used ? 1 : 0;
  return n;
}

uint32_t RamFs::usedBytes() const {
  uint32_t n = 0;
  for (const File& f : f_)
    if (f.used) n += f.size;
  return n;
}

uint32_t RamFs::serializedSize() const {
  uint32_t n = kHeaderBytes;
  for (const File& f : f_)
    if (f.used) n += entryBytes(f.path, f.size);
  return n;
}

uint32_t RamFs::serializedWith(int file, uint32_t newSize) const {
  return serializedSize() - f_[file].size + newSize;
}

uint32_t RamFs::serialize(uint8_t* out, uint32_t cap) const {
  const uint32_t need = serializedSize();
  if (!out || need > cap) return 0;
  uint8_t* p = out;
  std::memcpy(p, kMagic, 4);
  put32(p + 4, kFormatVersion);
  put32(p + 8, uint32_t(fileCount()));
  p += kHeaderBytes;
  for (int i = 0; i < kMaxFiles; ++i) {
    const File& f = f_[i];
    if (!f.used) continue;
    const uint32_t len = uint32_t(std::strlen(f.path));
    p[0] = uint8_t(len);
    p[1] = uint8_t(len >> 8);
    std::memcpy(p + 2, f.path, len);
    p += 2 + len;
    put32(p, f.size);
    p += 4;
    if (f.size) std::memcpy(p, data(i), f.size);
    p += f.size;
  }
  return uint32_t(p - out);
}

bool RamFs::deserialize(const uint8_t* in, uint32_t n) {
  clear();
  if (!in || n < kHeaderBytes || std::memcmp(in, kMagic, 4) != 0 || get32(in + 4) != kFormatVersion) return false;
  const uint32_t count = get32(in + 8);
  if (count > uint32_t(kMaxFiles)) return false;
  uint32_t off = kHeaderBytes;
  for (uint32_t k = 0; k < count; ++k) {
    if (off + 2 > n) break;
    const uint32_t len = uint32_t(in[off]) | uint32_t(in[off + 1]) << 8;
    if (len == 0 || len >= uint32_t(kPathMax) || off + 2 + len + 4 > n) break;
    char path[kPathMax];
    std::memcpy(path, in + off + 2, len);
    path[len] = 0;
    off += 2 + len;
    const uint32_t size = get32(in + off);
    off += 4;
    if (size > slot_ || off + size > n || !ours(path) || find(path) >= 0) break;
    const int i = create(path);
    if (i < 0) break;
    if (size) std::memcpy(data(i), in + off, size);
    f_[i].size = size;
    off += size;
    if (k + 1 == count) {
      ++rev_;
      return off == n;
    }
  }
  if (count == 0 && off == n) return true;
  clear();
  return false;
}

bool RamFs::mounted(const char* mount) { return arena_ && mount && std::strcmp(mount, mount_) == 0; }

bool RamFs::read(const char* path, uint8_t* buf, size_t cap, size_t* len) {
  if (!ours(path)) return false;
  const int i = find(path);
  if (i < 0 || f_[i].writing) return false;
  const size_t n = f_[i].size < cap ? f_[i].size : cap;
  if (n) std::memcpy(buf, data(i), n);
  if (len) *len = n;
  return true;
}

bool RamFs::write(const char* path, const uint8_t* buf, size_t len) {
  if (!ours(path) || len > slot_) return false;
  int i = find(path);
  if (i >= 0 && f_[i].writing) return false;
  if (i < 0) {
    if (serializedSize() + entryBytes(path, uint32_t(len)) > budget_) return false;
    i = create(path);
    if (i < 0) return false;
  } else if (serializedWith(i, uint32_t(len)) > budget_) {
    return false;
  }
  if (len) std::memcpy(data(i), buf, len);
  f_[i].size = uint32_t(len);
  ++rev_;
  return true;
}

bool RamFs::remove(const char* path) {
  if (!ours(path)) return false;
  const int i = find(path);
  if (i < 0) return false;
  for (Handle& h : h_)
    if (h.open && h.file == i) h = Handle{};
  f_[i] = File{};
  ++rev_;
  return true;
}

bool RamFs::exists(const char* path) { return ours(path) && find(path) >= 0; }

int RamFs::list(const char* dir, void (*cb)(const char*, size_t, void*), void* user) {
  if (!dir || std::strncmp(dir, mount_, mountLen_) != 0 || (dir[mountLen_] != 0 && dir[mountLen_] != '/') || !arena_)
    return -1;
  const size_t dl = std::strlen(dir);
  int n = 0;
  for (const File& f : f_) {
    if (!f.used || f.writing) continue;
    if (std::strncmp(f.path, dir, dl) != 0 || f.path[dl] != '/') continue;
    const char* name = f.path + dl + 1;
    if (!*name || std::strchr(name, '/')) continue;
    if (cb) cb(name, f.size, user);
    ++n;
  }
  return n;
}

int RamFs::open(const char* path, bool forWrite) {
  if (!ours(path)) return -1;
  int hi = -1;
  for (int k = 0; k < kMaxHandles; ++k)
    if (!h_[k].open) {
      hi = k;
      break;
    }
  if (hi < 0) return -1;
  int i = find(path);
  if (forWrite) {
    if (i >= 0 && f_[i].writing) return -1;
    if (i < 0) {
      if (serializedSize() + entryBytes(path, 0) > budget_) return -1;
      i = create(path);
      if (i < 0) return -1;
    }
    for (Handle& h : h_)
      if (h.open && h.file == i) h = Handle{};   // a reader of the old content loses it
    f_[i].size = 0;
    f_[i].writing = true;
    ++rev_;
  } else if (i < 0 || f_[i].writing) {
    return -1;
  }
  h_[hi] = Handle{true, forWrite, false, i, 0};
  return hi;
}

int RamFs::readSome(int h, uint8_t* buf, size_t cap) {
  if (h < 0 || h >= kMaxHandles || !h_[h].open || h_[h].write) return -1;
  Handle& hd = h_[h];
  const File& f = f_[hd.file];
  uint32_t n = f.size - hd.pos;
  if (n > cap) n = uint32_t(cap);
  if (n) std::memcpy(buf, data(hd.file) + hd.pos, n);
  hd.pos += n;
  return int(n);
}

bool RamFs::writeSome(int h, const uint8_t* buf, size_t len) {
  if (h < 0 || h >= kMaxHandles || !h_[h].open || !h_[h].write) return false;
  Handle& hd = h_[h];
  if (hd.failed) return false;
  if (hd.pos + len > slot_ || serializedWith(hd.file, uint32_t(hd.pos + len)) > budget_) {
    hd.failed = true;
    return false;
  }
  if (len) std::memcpy(data(hd.file) + hd.pos, buf, len);
  hd.pos += uint32_t(len);
  return true;
}

bool RamFs::close(int h) {
  if (h < 0 || h >= kMaxHandles || !h_[h].open) return false;
  const Handle hd = h_[h];
  h_[h] = Handle{};
  if (!hd.write) return true;
  File& f = f_[hd.file];
  f.writing = false;
  if (hd.failed) {
    f.size = 0;   // as LittleFS would leave it: created, empty
    ++rev_;
    return false;
  }
  f.size = hd.pos;
  ++rev_;
  return true;
}

bool RamFs::rename(const char* from, const char* to) {
  if (!ours(from) || !ours(to)) return false;
  const int i = find(from);
  if (i < 0 || f_[i].writing) return false;
  if (std::strcmp(from, to) == 0) return true;
  const int j = find(to);
  if (j >= 0) {
    if (f_[j].writing) return false;
    for (Handle& hd : h_)
      if (hd.open && hd.file == j) hd = Handle{};
    f_[j] = File{};
  }
  // The path length changes the snapshot's size by a few bytes: allowed even at the
  // budget's edge (the target's entry, if any, just went away).
  std::memset(f_[i].path, 0, sizeof(f_[i].path));
  std::memcpy(f_[i].path, to, std::strlen(to) + 1);
  ++rev_;
  return true;
}

long RamFs::freeBytes(const char* mount) {
  if (!mounted(mount)) return -1;
  const uint32_t s = serializedSize();
  return s >= budget_ ? 0 : long(budget_ - s);
}

// ---- MountFs ----------------------------------------------------------------------

pg::FileSystem* MountFs::route(const char* path) const {
  if (!path) return nullptr;
  if (std::strncmp(path, "/int", 4) == 0 && (path[4] == 0 || path[4] == '/')) return int_;
  if (std::strncmp(path, "/sd", 3) == 0 && (path[3] == 0 || path[3] == '/')) return sd_;
  return nullptr;
}

pg::FileSystem* MountFs::byHandle(int h, int* inner) const {
  if (h < 0) return nullptr;
  if (h & kSdTag) {
    *inner = h & ~kSdTag;
    return sd_;
  }
  *inner = h;
  return int_;
}

bool MountFs::mounted(const char* m) {
  pg::FileSystem* fs = route(m);
  return fs && fs->mounted(m);
}
bool MountFs::read(const char* p, uint8_t* b, size_t c, size_t* l) {
  pg::FileSystem* fs = route(p);
  return fs && fs->read(p, b, c, l);
}
bool MountFs::write(const char* p, const uint8_t* b, size_t l) {
  pg::FileSystem* fs = route(p);
  return fs && fs->write(p, b, l);
}
bool MountFs::remove(const char* p) {
  pg::FileSystem* fs = route(p);
  return fs && fs->remove(p);
}
bool MountFs::exists(const char* p) {
  pg::FileSystem* fs = route(p);
  return fs && fs->exists(p);
}
int MountFs::list(const char* d, void (*cb)(const char*, size_t, void*), void* u) {
  pg::FileSystem* fs = route(d);
  return fs ? fs->list(d, cb, u) : -1;
}
int MountFs::open(const char* p, bool w) {
  pg::FileSystem* fs = route(p);
  if (!fs) return -1;
  const int h = fs->open(p, w);
  if (h < 0) return -1;
  return fs == sd_ && fs != int_ ? (h | kSdTag) : h;
}
int MountFs::readSome(int h, uint8_t* b, size_t c) {
  int in = 0;
  pg::FileSystem* fs = byHandle(h, &in);
  return fs ? fs->readSome(in, b, c) : -1;
}
bool MountFs::writeSome(int h, const uint8_t* b, size_t l) {
  int in = 0;
  pg::FileSystem* fs = byHandle(h, &in);
  return fs && fs->writeSome(in, b, l);
}
bool MountFs::close(int h) {
  int in = 0;
  pg::FileSystem* fs = byHandle(h, &in);
  return fs && fs->close(in);
}
bool MountFs::rename(const char* f, const char* t) {
  pg::FileSystem* a = route(f);
  return a && a == route(t) && a->rename(f, t);
}
long MountFs::freeBytes(const char* m) {
  pg::FileSystem* fs = route(m);
  return fs ? fs->freeBytes(m) : -1;
}

}  // namespace pgd
