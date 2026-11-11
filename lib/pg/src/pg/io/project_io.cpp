#include "pg/io/project_io.h"

#include <cstring>

#include "pg/io/codec.h"
#include "pg/params.h"

namespace pg::io {

using namespace detail;

const char* statusName(Status s) {
  switch (s) {
    case Status::Ok: return "OK";
    case Status::BadName: return "BAD NAME";
    case Status::NoMount: return "NO CARD";
    case Status::NotFound: return "NOT FOUND";
    case Status::NoSpace: return "DISK FULL";
    case Status::OpenFailed: return "OPEN FAILED";
    case Status::WriteFailed: return "WRITE FAILED";
    case Status::VerifyFailed: return "VERIFY FAILED";
    case Status::RenameFailed: return "RENAME FAILED";
    case Status::RemoveFailed: return "DELETE FAILED";
    case Status::ReadFailed: return "READ FAILED";
    case Status::NotPgFile: return "NOT A POCKET GROOVE FILE";
    case Status::NewerFormat: return "NEEDS NEWER FIRMWARE";
    case Status::ProjectVersion: return "UNSUPPORTED PROJECT VERSION";
    case Status::Corrupt: return "FILE CORRUPT";
    case Status::Truncated: return "FILE TRUNCATED";
    case Status::BadProject: return "BAD PROJECT";
    case Status::Busy: return "BUSY, TRY AGAIN";
    case Status::DestClobbered: return "LOAD FAILED, PROJECT CLEARED";
    case Status::BufferTooSmall: return "BUFFER TOO SMALL";
  }
  return "?";
}

uint32_t crc32(uint32_t crc, const uint8_t* data, size_t len) {
  uint32_t r = ~crc;
  for (size_t i = 0; i < len; ++i) r = crcReg(r, data[i]);
  return ~r;
}

namespace {

constexpr uint8_t kFileMagic[4] = {'P', 'G', 'V', 'F'};
constexpr uint8_t kCodecPglz = 1;
constexpr uint32_t kRawSize = uint32_t(sizeof(Project));
constexpr const char* kTmpName = "_save.tmp";
constexpr const char* kSessionStem = "_last";
constexpr int kEncodeAttempts = 4;
constexpr int kPrefixLen = 5;  // Project::magic + version, checked in pass 1

// ---- sources and sinks -------------------------------------------------------
class FsSource final : public Source {
 public:
  FsSource(FileSystem& fs, int h) : fs_(fs), h_(h) {}
  int readSome(uint8_t* buf, size_t cap) override { return fs_.readSome(h_, buf, cap); }
 private:
  FileSystem& fs_;
  int h_;
};
class FsSink final : public Sink {
 public:
  FsSink(FileSystem& fs, int h) : fs_(fs), h_(h) {}
  bool writeSome(const uint8_t* buf, size_t len) override { return fs_.writeSome(h_, buf, len); }
 private:
  FileSystem& fs_;
  int h_;
};
class MemSource final : public Source {
 public:
  MemSource(const uint8_t* p, size_t n) : p_(p), n_(n) {}
  int readSome(uint8_t* buf, size_t cap) override {
    size_t k = n_ - pos_;
    if (k > cap) k = cap;
    if (k) std::memcpy(buf, p_ + pos_, k);
    pos_ += k;
    return int(k);
  }
 private:
  const uint8_t* p_;
  size_t n_, pos_ = 0;
};
class MemSink final : public Sink {
 public:
  MemSink(uint8_t* p, size_t cap) : p_(p), cap_(cap) {}
  bool writeSome(const uint8_t* buf, size_t len) override {
    if (len > cap_ - n_) return false;
    std::memcpy(p_ + n_, buf, len);
    n_ += len;
    return true;
  }
  size_t size() const { return n_; }
 private:
  uint8_t* p_;
  size_t cap_, n_ = 0;
};

// ---- little helpers ----------------------------------------------------------
void put16(uint8_t* p, uint32_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); }
void put32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i)); }
uint32_t get16(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8); }
uint32_t get32(const uint8_t* p) { return get16(p) | (get16(p + 2) << 16); }

const uint8_t* bytesOf(const Project& p) { return reinterpret_cast<const uint8_t*>(&p); }
uint8_t* bytesOf(Project& p) { return reinterpret_cast<uint8_t*>(&p); }

size_t readChunkOf(const IoOptions* o) { return o ? o->readChunk : kIoBuf; }
size_t writeChunkOf(const IoOptions* o) { return o ? o->writeChunk : kIoBuf; }

bool validStem(const char* s) {
  if (!s) return false;
  size_t n = 0;
  for (; s[n]; ++n) {
    const char c = s[n];
    const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    if (!ok || n >= size_t(kStemMax)) return false;
  }
  return n > 0;
}

// "/int/..." -> "/int", "/sd/..." -> "/sd"; nullptr otherwise.
const char* mountOf(const char* path) {
  if (!path) return nullptr;
  if (std::strncmp(path, "/int/", 5) == 0) return kMountInt;
  if (std::strncmp(path, "/sd/", 4) == 0) return kMountSd;
  return nullptr;
}

bool joinPath(char* out, size_t cap, const char* a, const char* b, const char* c) {
  const size_t la = std::strlen(a), lb = std::strlen(b), lc = std::strlen(c);
  if (la + lb + lc + 1 > cap) return false;
  std::memcpy(out, a, la);
  std::memcpy(out + la, b, lb);
  std::memcpy(out + la + lb, c, lc + 1);
  return true;
}

// Same directory as path, file kTmpName.
bool tmpPathFor(const char* path, char* out, size_t cap) {
  const char* slash = std::strrchr(path, '/');
  if (!slash) return false;
  const size_t dirLen = size_t(slash - path) + 1;
  const size_t tl = std::strlen(kTmpName);
  if (dirLen + tl + 1 > cap) return false;
  std::memcpy(out, path, dirLen);
  std::memcpy(out + dirLen, kTmpName, tl + 1);
  return true;
}

Status mapDecode(DecodeResult r) {
  switch (r) {
    case DecodeResult::Ok: return Status::Ok;
    case DecodeResult::Truncated: return Status::Truncated;
    case DecodeResult::ReadError: return Status::ReadFailed;
    default: return Status::Corrupt;
  }
}

// ---- header -----------------------------------------------------------------
void packHeader(const HeaderInfo& h, uint8_t out[kHeaderSize]) {
  std::memset(out, 0, kHeaderSize);
  std::memcpy(out, kFileMagic, 4);
  out[4] = h.formatVersion;
  out[5] = h.compatVersion;
  put16(out + 6, h.headerSize);
  out[8] = h.projectVersion;
  out[9] = h.codec;
  out[10] = h.tempo;
  out[11] = h.songMode;
  put32(out + 12, h.rawSize);
  put32(out + 16, h.bodySize);
  put32(out + 20, h.rawCrc);
  put32(out + 24, h.bodyCrc);
  std::memcpy(out + 28, h.stem, kStemMax);  // stem[15] NUL implied by memset
  out[44] = h.swing;
  out[45] = h.scaleRoot;
  out[46] = h.scaleType;
  put32(out + kHeaderSize - 4, crc32(0, out, kHeaderSize - 4));
}

// Reads and validates the header (also of newer, longer headers). On Ok the
// reader is positioned at the first body byte.
Status readHeaderFrom(Reader& r, HeaderInfo& h) {
  std::memset(&h, 0, sizeof(h));
  uint8_t b[kHeaderSize];
  r.begin(0, false);
  auto fail = [&r]() { return r.fail() == Reader::kError ? Status::ReadFailed : Status::Truncated; };
  for (int i = 0; i < 8; ++i)
    if (!r.get(b[i])) return (i < 4 && r.fail() == Reader::kEof) ? Status::NotPgFile : fail();
  if (std::memcmp(b, kFileMagic, 4) != 0) return Status::NotPgFile;
  h.formatVersion = b[4];
  h.compatVersion = b[5];
  h.headerSize = uint16_t(get16(b + 6));
  if (h.compatVersion > kFormatVersion || h.compatVersion == 0) return Status::NewerFormat;
  if (h.headerSize < kHeaderSize || h.headerSize > kMaxHeaderSize) return Status::Corrupt;
  const uint32_t crcEnd = uint32_t(h.headerSize) - 4u;
  uint32_t computed = 0;
  uint8_t stored[4] = {};
  for (uint32_t i = 8; i < h.headerSize; ++i) {
    if (i == crcEnd) computed = r.crc();
    uint8_t x;
    if (!r.get(x)) return fail();
    if (i < uint32_t(kHeaderSize) - 4u) b[i] = x;  // later fields are CRC'd and skipped
    if (i >= crcEnd) stored[i - crcEnd] = x;
  }
  h.headerCrc = get32(stored);
  if (computed != h.headerCrc) return Status::Corrupt;
  h.projectVersion = b[8];
  h.codec = b[9];
  h.tempo = b[10];
  h.songMode = b[11];
  h.rawSize = get32(b + 12);
  h.bodySize = get32(b + 16);
  h.rawCrc = get32(b + 20);
  h.bodyCrc = get32(b + 24);
  std::memcpy(h.stem, b + 28, kStemMax);
  h.stem[kStemMax] = 0;
  h.swing = b[44];
  h.scaleRoot = b[45];
  h.scaleType = b[46];
  if (h.codec != kCodecPglz) return Status::Corrupt;  // a new codec must bump compatVersion
  if (h.projectVersion != kProjectVersion) return Status::ProjectVersion;
  if (h.rawSize != kRawSize) return Status::Corrupt;
  if (h.bodySize == 0 || h.bodySize > lzMaxBody(h.rawSize)) return Status::Corrupt;
  return Status::Ok;
}

bool sameFile(const HeaderInfo& a, const HeaderInfo& b) {
  return a.headerCrc == b.headerCrc && a.headerSize == b.headerSize && a.bodySize == b.bodySize &&
         a.bodyCrc == b.bodyCrc && a.rawCrc == b.rawCrc && a.rawSize == b.rawSize;
}

// Pass 1: header, body CRC, full token structure, EOF, Project magic/version.
Status validateStream(Source& src, size_t chunk, HeaderInfo& h) {
  Reader r(src, chunk);
  Status s = readHeaderFrom(r, h);
  if (s != Status::Ok) return s;
  uint8_t prefix[kPrefixLen] = {};
  r.begin(h.bodySize, true);
  s = mapDecode(lzDecode(r, nullptr, h.rawSize, prefix, kPrefixLen, nullptr));
  if (s != Status::Ok) return s;
  if (r.consumed() != h.bodySize || r.crc() != h.bodyCrc) return Status::Corrupt;
  const int eof = r.atEof();
  if (eof < 0) return Status::ReadFailed;
  if (eof == 0) return Status::Corrupt;  // trailing bytes
  if (prefix[0] != 'P' || prefix[1] != 'G' || prefix[2] != 'R' || prefix[3] != 'V' || prefix[4] != h.projectVersion)
    return Status::Corrupt;
  return Status::Ok;
}

// Pass 2: decode into dest. Ok, Corrupt (file changed before dest was touched)
// or DestClobbered (dest zeroed).
Status decodeStream(Source& src, size_t chunk, const HeaderInfo& expect, Project& dest) {
  Reader r(src, chunk);
  HeaderInfo h;
  const Status s = readHeaderFrom(r, h);
  if (s != Status::Ok) return s == Status::ReadFailed ? s : Status::Corrupt;
  if (!sameFile(h, expect)) return Status::Corrupt;
  uint8_t* d = bytesOf(dest);
  uint32_t raw = 0;
  r.begin(h.bodySize, true);
  const DecodeResult dr = lzDecode(r, d, h.rawSize, nullptr, 0, &raw);
  if (dr != DecodeResult::Ok || r.consumed() != h.bodySize || r.crc() != h.bodyCrc || raw != h.rawCrc ||
      r.atEof() != 1) {
    std::memset(d, 0, sizeof(Project));
    return Status::DestClobbered;
  }
  return Status::Ok;
}

// ---- encode -----------------------------------------------------------------
struct EncStats {
  uint32_t bodySize, bodyCrc, rawCrc;
  bool operator==(const EncStats& o) const { return bodySize == o.bodySize && bodyCrc == o.bodyCrc && rawCrc == o.rawCrc; }
  bool operator!=(const EncStats& o) const { return !(*this == o); }
};

void fillHeader(const Project& p, const char* stem, const EncStats& st, HeaderInfo& h) {
  std::memset(&h, 0, sizeof(h));
  h.formatVersion = kFormatVersion;
  h.compatVersion = 1;
  h.headerSize = kHeaderSize;
  h.projectVersion = p.version;
  h.codec = kCodecPglz;
  h.tempo = p.globals[kGTempo];
  h.songMode = p.songMode;
  h.swing = p.globals[kGSwing];
  h.scaleRoot = p.globals[kGScaleRoot];
  h.scaleType = p.globals[kGScaleType];
  h.rawSize = kRawSize;
  h.bodySize = st.bodySize;
  h.rawCrc = st.rawCrc;
  h.bodyCrc = st.bodyCrc;
  std::strncpy(h.stem, stem, kStemMax);
  h.stem[kStemMax] = 0;
}

EncodeResult encodeBody(const Project& p, Writer& w, EncStats& st) {
  w.begin();
  uint32_t raw = 0;
  const EncodeResult r = lzEncode(bytesOf(p), kRawSize, w, &raw);
  st.bodySize = w.count();
  st.bodyCrc = w.crc();
  st.rawCrc = raw;
  return r;
}

// Dry run until two reads of the Project agree (no edit landed mid-encode).
#if defined(__GNUC__)
__attribute__((noinline))
#endif
Status encodeStable(const Project& p, EncStats& st) {
  for (int a = 0; a < kEncodeAttempts; ++a) {
    Writer dry(nullptr, kIoBuf);
    if (encodeBody(p, dry, st) != EncodeResult::Ok) continue;
    if (crc32(0, bytesOf(p), kRawSize) == st.rawCrc) return Status::Ok;
  }
  return Status::Busy;
}

// Separate frames keep the header bytes off the stack while the body encodes.
#if defined(__GNUC__)
#define PG_IO_NOINLINE __attribute__((noinline))
#else
#define PG_IO_NOINLINE
#endif
PG_IO_NOINLINE uint32_t packFor(const Project& p, const char* stem, const EncStats& st, uint8_t* hb) {
  HeaderInfo h;
  fillHeader(p, stem, st, h);
  packHeader(h, hb);
  return get32(hb + kHeaderSize - 4);
}
PG_IO_NOINLINE void putHeader(Writer& w, const Project& p, const char* stem, const EncStats& st, uint32_t* hcrc) {
  uint8_t hb[kHeaderSize];
  *hcrc = packFor(p, stem, st, hb);
  w.put(hb, kHeaderSize);
}
PG_IO_NOINLINE uint32_t headerCrcFor(const Project& p, const char* stem, const EncStats& st) {
  uint8_t hb[kHeaderSize];
  return packFor(p, stem, st, hb);
}

// Header + body into sink; Ok only if the result matches `expect` and the
// Project did not change meanwhile.
enum class WriteOutcome { Ok, WriteError, Changed };
WriteOutcome writeStream(const Project& p, const char* stem, const EncStats& expect, Sink& sink, size_t chunk) {
  Writer w(&sink, chunk);
  uint32_t hcrc = 0;
  putHeader(w, p, stem, expect, &hcrc);
  EncStats got;
  const EncodeResult r = encodeBody(p, w, got);
  if (r == EncodeResult::WriteError || !w.ok()) return WriteOutcome::WriteError;
  if (r == EncodeResult::Race || got != expect || crc32(0, bytesOf(p), kRawSize) != expect.rawCrc) return WriteOutcome::Changed;
  // The header fields (tempo...) were read from the live Project too.
  if (headerCrcFor(p, stem, expect) != hcrc) return WriteOutcome::Changed;
  return WriteOutcome::Ok;
}

bool projectLooksValid(const Project& p) {
  return p.magic[0] == 'P' && p.magic[1] == 'G' && p.magic[2] == 'R' && p.magic[3] == 'V' && p.version == kProjectVersion;
}

Status verifyWithChunk(FileSystem& fs, const char* path, HeaderInfo* info, size_t chunk) {
  if (info) std::memset(info, 0, sizeof(*info));
  const char* m = mountOf(path);
  if (!m) return Status::BadName;
  if (!fs.mounted(m)) return Status::NoMount;
  const int h = fs.open(path, false);
  if (h < 0) return fs.exists(path) ? Status::OpenFailed : Status::NotFound;
  HeaderInfo hi;
  FsSource src(fs, h);
  const Status s = validateStream(src, chunk, hi);
  fs.close(h);
  if (info) *info = hi;
  return s;
}

PG_IO_NOINLINE bool verifyTemp(FileSystem& fs, const char* tmp, const EncStats& st, size_t chunk) {
  HeaderInfo v;
  return verifyWithChunk(fs, tmp, &v, chunk) == Status::Ok && v.bodySize == st.bodySize && v.bodyCrc == st.bodyCrc &&
         v.rawCrc == st.rawCrc;
}

Status failCause(FileSystem& fs, const char* mount, Status fallback) {
  if (!fs.mounted(mount)) return Status::NoMount;
  if (fallback == Status::WriteFailed) {
    const long fb = fs.freeBytes(mount);
    if (fb >= 0 && fb < kSaveSpaceMargin) return Status::NoSpace;
  }
  return fallback;
}

// Moves a verified temp file over its target (saveFile, installReceived).
Status commitTemp(FileSystem& fs, const char* m, const char* tmp, const char* path) {
  if (!fs.rename(tmp, path)) {
    // A non-atomic rename (remove + move) may have failed after removing
    // the target: then the verified temp is the only copy. Retry once;
    // if that fails too, keep the temp for recoverAfterPowerLoss().
    if (!fs.exists(path) && fs.exists(tmp)) {
      if (fs.rename(tmp, path)) return Status::Ok;
      return failCause(fs, m, Status::RenameFailed);
    }
    fs.remove(tmp);
    return failCause(fs, m, Status::RenameFailed);
  }
  return Status::Ok;
}

// ---- UI settings blob ----------------------------------------------------------
constexpr uint8_t kUiMagic[4] = {'P', 'G', 'U', 'I'};
constexpr int kUiHeader = 12;
constexpr const char* kUiTmpPath = "/int/pg/_ui.tmp";

// Reads + validates a UI blob. buf may be nullptr (validate only).
Status readUiBlob(FileSystem& fs, const char* path, uint8_t* version, uint8_t* buf, size_t cap, size_t* len) {
  if (!fs.mounted(kMountInt)) return Status::NoMount;
  const int h = fs.open(path, false);
  if (h < 0) return fs.exists(path) ? Status::OpenFailed : Status::NotFound;
  FsSource src(fs, h);
  Reader r(src, kIoBuf);
  r.begin(0, false);
  uint8_t hd[kUiHeader] = {};
  Status s = Status::Ok;
  for (int i = 0; i < kUiHeader && s == Status::Ok; ++i)
    if (!r.get(hd[i])) s = (r.fail() == Reader::kError) ? Status::ReadFailed : Status::Truncated;
  if (s == Status::Ok && std::memcmp(hd, kUiMagic, 4) != 0) s = Status::NotPgFile;
  const size_t n = hd[5];
  if (s == Status::Ok && (n > kUiSettingsMax || hd[6] || hd[7])) s = Status::Corrupt;
  if (s == Status::Ok && buf && n > cap) s = Status::BufferTooSmall;
  uint32_t c = ~crc32(0, hd, 8);
  for (size_t i = 0; i < n && s == Status::Ok; ++i) {
    uint8_t x;
    if (!r.get(x)) { s = (r.fail() == Reader::kError) ? Status::ReadFailed : Status::Truncated; break; }
    c = crcReg(c, x);
    if (buf) buf[i] = x;  // i < n <= cap
  }
  if (s == Status::Ok && ~c != get32(hd + 8)) s = Status::Corrupt;
  if (s == Status::Ok) {
    const int e = r.atEof();
    if (e < 0) s = Status::ReadFailed;
    else if (e == 0) s = Status::Corrupt;
  }
  fs.close(h);
  if (s == Status::Ok) {
    if (version) *version = hd[4];
    if (len) *len = n;
  }
  return s;
}

}  // namespace

// ---- names and paths ------------------------------------------------------------
bool isValidName(const char* name) {
  if (!name) return false;
  size_t n = 0;
  for (; name[n]; ++n) {
    const char c = name[n];
    if (n >= size_t(kNameMax)) return false;
    if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
  }
  return n > 0;
}

bool normalizeName(const char* in, char out[kNameBuf]) {
  int n = 0;
  if (in) {
    for (; *in && n < kNameMax; ++in) {
      char c = *in;
      if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
      if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_') out[n++] = c;
    }
  }
  out[n] = 0;
  return n > 0;
}

bool isMount(const char* m) { return m && (std::strcmp(m, kMountInt) == 0 || std::strcmp(m, kMountSd) == 0); }

bool buildProjectPath(const char* mount, const char* name, char* out, size_t cap) {
  if (!isMount(mount) || !isValidName(name) || !out) return false;
  char dir[16];
  if (!joinPath(dir, sizeof(dir), mount, "/pg/", "")) return false;
  return joinPath(out, cap, dir, name, ".pgv");
}

// ---- any path ------------------------------------------------------------------
Status readHeader(FileSystem& fs, const char* path, HeaderInfo* info) {
  if (info) std::memset(info, 0, sizeof(*info));
  const char* m = mountOf(path);
  if (!m) return Status::BadName;
  if (!fs.mounted(m)) return Status::NoMount;
  const int h = fs.open(path, false);
  if (h < 0) return fs.exists(path) ? Status::OpenFailed : Status::NotFound;
  FsSource src(fs, h);
  Reader r(src, kIoBuf);
  HeaderInfo hi;
  const Status s = readHeaderFrom(r, hi);
  fs.close(h);
  if (info) *info = hi;
  return s;
}

Status verifyFile(FileSystem& fs, const char* path, HeaderInfo* info) { return verifyWithChunk(fs, path, info, kIoBuf); }

Status saveFile(FileSystem& fs, const char* path, const char* stem, const Project& p, const IoOptions* o) {
  const char* m = mountOf(path);
  char tmp[kPathMax];
  if (!m || !validStem(stem) || std::strlen(path) >= size_t(kPathMax) || !tmpPathFor(path, tmp, sizeof(tmp)) ||
      std::strcmp(tmp, path) == 0)
    return Status::BadName;
  if (!fs.mounted(m)) return Status::NoMount;
  if (!projectLooksValid(p)) return Status::BadProject;

  for (int attempt = 0; attempt < kEncodeAttempts; ++attempt) {
    EncStats st;
    Status s = encodeStable(p, st);
    if (s != Status::Ok) return s;
    // A stale temp from an interrupted save would also eat free space.
    if (fs.exists(tmp) && !fs.remove(tmp)) return failCause(fs, m, Status::WriteFailed);
    const long need = long(kHeaderSize) + long(st.bodySize);
    const long fb = fs.freeBytes(m);
    if (fb >= 0 && fb < need + kSaveSpaceMargin) return Status::NoSpace;

    const int h = fs.open(tmp, true);
    if (h < 0) return failCause(fs, m, Status::OpenFailed);
    FsSink sink(fs, h);
    const WriteOutcome wo = writeStream(p, stem, st, sink, writeChunkOf(o));
    const bool closed = fs.close(h);
    if (wo == WriteOutcome::WriteError || !closed) {
      s = failCause(fs, m, Status::WriteFailed);
      fs.remove(tmp);
      return s;
    }
    if (wo == WriteOutcome::Changed) { fs.remove(tmp); continue; }

    if (!verifyTemp(fs, tmp, st, readChunkOf(o))) {
      fs.remove(tmp);
      return failCause(fs, m, Status::VerifyFailed);
    }
    return commitTemp(fs, m, tmp, path);
  }
  return Status::Busy;
}

Status installReceived(FileSystem& fs, const char* tmpPath, const char* mount, const char* name, uint32_t size,
                       uint32_t crc, HeaderInfo* info) {
  if (info) std::memset(info, 0, sizeof(*info));
  char path[kPathMax];
  if (!isMount(mount) || !buildProjectPath(mount, name, path, sizeof(path)) || !tmpPath ||
      std::strcmp(mountOf(tmpPath) ? mountOf(tmpPath) : "", mount) != 0 || std::strcmp(tmpPath, path) == 0)
    return Status::BadName;
  if (!fs.mounted(mount)) return Status::NoMount;
  // 1. The bytes on disk are the bytes that were sent (size + whole-file CRC32).
  {
    const int h = fs.open(tmpPath, false);
    if (h < 0) return fs.exists(tmpPath) ? Status::OpenFailed : Status::NotFound;
    uint8_t buf[kIoBuf];
    uint32_t n = 0, c = 0;
    bool ok = true;
    for (;;) {
      const int r = fs.readSome(h, buf, sizeof(buf));
      if (r < 0) { ok = false; break; }
      if (r == 0) break;
      c = crc32(c, buf, size_t(r));
      n += uint32_t(r);
      if (n > size) break;
    }
    fs.close(h);
    if (!ok || n != size || c != crc) {
      fs.remove(tmpPath);
      return ok ? Status::VerifyFailed : Status::ReadFailed;
    }
  }
  // 2. The same full check as the first pass of a load, and the stem the
  //    power-loss recovery would install it under.
  HeaderInfo h;
  const Status v = verifyFile(fs, tmpPath, &h);
  if (info) *info = h;
  if (v != Status::Ok || std::strcmp(h.stem, name) != 0) {
    fs.remove(tmpPath);
    return v != Status::Ok ? v : Status::BadName;
  }
  // 3. Into place exactly as saveFile does.
  return commitTemp(fs, mount, tmpPath, path);
}

bool xferTempPath(const char* mount, char* out, size_t cap) {
  return isMount(mount) && joinPath(out, cap, mount, "/pg/", kXferTmpName);
}

Status loadFile(FileSystem& fs, const char* path, Project& dest, HeaderInfo* info, const IoOptions* o) {
  HeaderInfo h1 = {};
  Status s = verifyWithChunk(fs, path, &h1, readChunkOf(o));
  if (info) *info = h1;
  if (s != Status::Ok) return s;
  const int h = fs.open(path, false);
  if (h < 0) return Status::OpenFailed;
  FsSource src(fs, h);
  s = decodeStream(src, readChunkOf(o), h1, dest);
  fs.close(h);
  return s;
}

// ---- projects by mount + name ---------------------------------------------------
Status saveProject(FileSystem& fs, const char* mount, const char* name, const Project& p, const IoOptions* o) {
  char path[kPathMax];
  if (!buildProjectPath(mount, name, path, sizeof(path))) return Status::BadName;
  return saveFile(fs, path, name, p, o);
}

Status loadProject(FileSystem& fs, const char* mount, const char* name, Project& dest, HeaderInfo* info, const IoOptions* o) {
  char path[kPathMax];
  if (!buildProjectPath(mount, name, path, sizeof(path))) return Status::BadName;
  return loadFile(fs, path, dest, info, o);
}

bool projectExists(FileSystem& fs, const char* mount, const char* name) {
  char path[kPathMax];
  if (!buildProjectPath(mount, name, path, sizeof(path))) return false;
  return fs.mounted(mount) && fs.exists(path);
}

Status deleteProject(FileSystem& fs, const char* mount, const char* name) {
  char path[kPathMax];
  if (!buildProjectPath(mount, name, path, sizeof(path))) return Status::BadName;
  if (!fs.mounted(mount)) return Status::NoMount;
  if (!fs.exists(path)) return Status::NotFound;
  return fs.remove(path) ? Status::Ok : failCause(fs, mount, Status::RemoveFailed);
}

bool nextFreeName(FileSystem& fs, char out[kNameBuf]) {
  for (int n = 1; n <= 99; ++n) {
    char name[kNameBuf] = {'G', 'R', 'O', 'O', 'V', 'E', char('0' + n / 10), char('0' + n % 10), 0};
    if (!projectExists(fs, kMountInt, name) && !projectExists(fs, kMountSd, name)) {
      std::memcpy(out, name, kNameBuf);
      return true;
    }
  }
  out[0] = 0;
  return false;
}

namespace {
struct ListCtx {
  ProjectEntry* out;
  int cap, n;
};
void listCb(const char* name, size_t size, void* user) {
  ListCtx& c = *static_cast<ListCtx*>(user);
  if (!name || c.n >= c.cap) return;
  const size_t len = std::strlen(name);
  if (len < 5 || len > size_t(kNameMax) + 4 || std::strcmp(name + len - 4, ".pgv") != 0) return;
  char stem[kNameBuf];
  std::memcpy(stem, name, len - 4);
  stem[len - 4] = 0;
  if (!isValidName(stem)) return;  // excludes _last.pgv (lower case) and foreign files
  ProjectEntry& e = c.out[c.n++];
  std::memcpy(e.name, stem, kNameBuf);
  e.tempo = 0;
  e.valid = false;
  e.status = Status::NotFound;
  e.size = uint32_t(size);
}
}  // namespace

int listProjects(FileSystem& fs, const char* mount, ProjectEntry* out, int cap) {
  if (!isMount(mount)) return -1;
  if (!fs.mounted(mount)) return -1;
  char dir[16];
  joinPath(dir, sizeof(dir), mount, "/pg", "");
  ListCtx c{out, out ? cap : 0, 0};
  if (fs.list(dir, listCb, &c) < 0) return 0;  // no pg/ directory yet
  for (int i = 1; i < c.n; ++i) {  // insertion sort A-Z
    ProjectEntry e = out[i];
    int j = i - 1;
    for (; j >= 0 && std::strcmp(out[j].name, e.name) > 0; --j) out[j + 1] = out[j];
    out[j + 1] = e;
  }
  for (int i = 0; i < c.n; ++i) {
    char path[kPathMax];
    buildProjectPath(mount, out[i].name, path, sizeof(path));
    HeaderInfo h;
    out[i].status = readHeader(fs, path, &h);
    out[i].valid = out[i].status == Status::Ok;
    out[i].tempo = out[i].valid ? h.tempo : 0;
  }
  return c.n;
}

// ---- session and UI ---------------------------------------------------------------
Status saveSession(FileSystem& fs, const Project& p) { return saveFile(fs, kSessionPath, kSessionStem, p); }
Status loadSession(FileSystem& fs, Project& dest) { return loadFile(fs, kSessionPath, dest); }

Status saveUiSettings(FileSystem& fs, uint8_t version, const uint8_t* data, size_t len) {
  if (len > kUiSettingsMax || (len && !data)) return Status::BufferTooSmall;
  if (!fs.mounted(kMountInt)) return Status::NoMount;
  uint8_t hd[kUiHeader] = {kUiMagic[0], kUiMagic[1], kUiMagic[2], kUiMagic[3], version, uint8_t(len), 0, 0};
  put32(hd + 8, crc32(crc32(0, hd, 8), data, len));
  if (fs.exists(kUiTmpPath) && !fs.remove(kUiTmpPath)) return failCause(fs, kMountInt, Status::WriteFailed);
  const long fb = fs.freeBytes(kMountInt);
  if (fb >= 0 && fb < long(kUiHeader + len) + kSaveSpaceMargin) return Status::NoSpace;
  const int h = fs.open(kUiTmpPath, true);
  if (h < 0) return failCause(fs, kMountInt, Status::OpenFailed);
  FsSink sink(fs, h);
  Writer w(&sink, kIoBuf);
  w.put(hd, kUiHeader);
  w.put(data, len);
  const bool ok = w.flush();
  if (!fs.close(h) || !ok) {
    const Status s = failCause(fs, kMountInt, Status::WriteFailed);
    fs.remove(kUiTmpPath);
    return s;
  }
  uint8_t v = 0;
  size_t n = 0;
  if (readUiBlob(fs, kUiTmpPath, &v, nullptr, 0, &n) != Status::Ok || v != version || n != len) {
    fs.remove(kUiTmpPath);
    return failCause(fs, kMountInt, Status::VerifyFailed);
  }
  if (!fs.rename(kUiTmpPath, kUiSettingsPath)) {
    fs.remove(kUiTmpPath);
    return failCause(fs, kMountInt, Status::RenameFailed);
  }
  return Status::Ok;
}

Status loadUiSettings(FileSystem& fs, uint8_t* version, uint8_t* buf, size_t cap, size_t* len) {
  if (!buf && cap) return Status::BufferTooSmall;
  uint8_t dummy;
  return readUiBlob(fs, kUiSettingsPath, version, buf ? buf : &dummy, buf ? cap : 0, len);
}

Status recoverAfterPowerLoss(FileSystem& fs, const char* mount) {
  if (!isMount(mount)) return Status::BadName;
  if (!fs.mounted(mount)) return Status::NoMount;
  Status result = Status::Ok;
  // The save temp and the USB-transfer temp (installReceived) are treated alike.
  const char* const tmpNames[2] = {kTmpName, kXferTmpName};
  for (const char* tmpName : tmpNames) {
    char tmp[kPathMax];
    joinPath(tmp, sizeof(tmp), mount, "/pg/", tmpName);
    if (!fs.exists(tmp)) continue;
    HeaderInfo h;
    char target[kPathMax];
    bool haveTarget = false;
    if (verifyFile(fs, tmp, &h) == Status::Ok) {
      if (isValidName(h.stem)) haveTarget = buildProjectPath(mount, h.stem, target, sizeof(target));
      else if (tmpName == kTmpName && std::strcmp(h.stem, kSessionStem) == 0 && std::strcmp(mount, kMountInt) == 0)
        haveTarget = joinPath(target, sizeof(target), kSessionPath, "", "");
    }
    if (haveTarget && !fs.exists(target)) {
      if (!fs.rename(tmp, target)) result = Status::RenameFailed;
    } else if (!fs.remove(tmp)) {
      result = Status::RemoveFailed;
    }
  }
  if (std::strcmp(mount, kMountInt) == 0 && fs.exists(kUiTmpPath)) {
    uint8_t v;
    size_t n;
    const bool valid = readUiBlob(fs, kUiTmpPath, &v, nullptr, 0, &n) == Status::Ok;
    if (valid && !fs.exists(kUiSettingsPath)) {
      if (!fs.rename(kUiTmpPath, kUiSettingsPath)) result = Status::RenameFailed;
    } else if (!fs.remove(kUiTmpPath)) {
      result = Status::RemoveFailed;
    }
  }
  return result;
}

// ---- in-memory ------------------------------------------------------------------
Status encodeToBuffer(const Project& p, const char* stem, uint8_t* out, size_t cap, size_t* len) {
  if (!validStem(stem)) return Status::BadName;
  if (!projectLooksValid(p)) return Status::BadProject;
  for (int a = 0; a < kEncodeAttempts; ++a) {
    EncStats st;
    const Status s = encodeStable(p, st);
    if (s != Status::Ok) return s;
    if (size_t(kHeaderSize) + st.bodySize > cap) return Status::BufferTooSmall;
    MemSink sink(out, cap);
    const WriteOutcome wo = writeStream(p, stem, st, sink, kIoBuf);
    if (wo == WriteOutcome::WriteError) return Status::BufferTooSmall;
    if (wo == WriteOutcome::Ok) {
      if (len) *len = sink.size();
      return Status::Ok;
    }
  }
  return Status::Busy;
}

Status decodeFromBuffer(const uint8_t* in, size_t len, Project& dest, HeaderInfo* info) {
  if (!in) return Status::NotPgFile;
  HeaderInfo h;
  MemSource s1(in, len);
  Status s = validateStream(s1, kIoBuf, h);
  if (info) *info = h;
  if (s != Status::Ok) return s;
  MemSource s2(in, len);
  return decodeStream(s2, kIoBuf, h, dest);
}

}  // namespace pg::io
