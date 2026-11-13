#include "pg/io/xfer.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace pg::io {

namespace {

constexpr char kPrefix[] = "~PGX ";
constexpr int kPrefixLen = 5;
constexpr int kMaxTokens = 8;
constexpr int kReplyMax = 300;
constexpr int kListCap = 40;                 // per mount, per LS (stack: ~0.8 KB)
constexpr uint32_t kMaxGetBytes = 1u << 20;  // sanity bound for GET

const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int b64val(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

bool parseU32(const char* s, uint32_t* out, int base) {
  if (!s || !*s) return false;
  uint64_t v = 0;
  for (const char* p = s; *p; ++p) {
    int d;
    if (*p >= '0' && *p <= '9') d = *p - '0';
    else if (base == 16 && *p >= 'A' && *p <= 'F') d = *p - 'A' + 10;
    else if (base == 16 && *p >= 'a' && *p <= 'f') d = *p - 'a' + 10;
    else return false;
    v = v * uint64_t(base) + uint64_t(d);
    if (v > 0xFFFFFFFFull) return false;
  }
  *out = uint32_t(v);
  return true;
}

// "int" -> "/int", "sd" -> "/sd", else nullptr.
const char* mountFor(const char* tok) {
  if (!tok) return nullptr;
  if (std::strcmp(tok, "int") == 0) return kMountInt;
  if (std::strcmp(tok, "sd") == 0) return kMountSd;
  return nullptr;
}
const char* mountTok(const char* mount) { return std::strcmp(mount, kMountSd) == 0 ? "sd" : "int"; }

// statusName() with spaces as underscores: one protocol token.
void statusToken(Status s, char* out, size_t cap) {
  const char* n = statusName(s);
  size_t i = 0;
  for (; n[i] && i + 1 < cap; ++i) out[i] = n[i] == ' ' ? '_' : n[i];
  out[i] = 0;
}

}  // namespace

size_t base64Encode(const uint8_t* in, size_t n, char* out, size_t cap) {
  const size_t need = (n + 2) / 3 * 4;
  if (cap < need + 1) {
    if (cap) out[0] = 0;
    return 0;
  }
  size_t o = 0;
  for (size_t i = 0; i < n; i += 3) {
    const uint32_t a = in[i], b = i + 1 < n ? in[i + 1] : 0, c = i + 2 < n ? in[i + 2] : 0;
    const uint32_t v = (a << 16) | (b << 8) | c;
    out[o++] = kB64[(v >> 18) & 63];
    out[o++] = kB64[(v >> 12) & 63];
    out[o++] = i + 1 < n ? kB64[(v >> 6) & 63] : '=';
    out[o++] = i + 2 < n ? kB64[v & 63] : '=';
  }
  out[o] = 0;
  return o;
}

int base64Decode(const char* in, uint8_t* out, size_t cap) {
  const size_t len = std::strlen(in);
  if (len % 4 != 0) return -1;
  size_t o = 0;
  for (size_t i = 0; i < len; i += 4) {
    int v[4];
    int pad = 0;
    for (int k = 0; k < 4; ++k) {
      const char c = in[i + size_t(k)];
      if (c == '=' && i + 4 == len && k >= 2) { v[k] = 0; ++pad; continue; }
      if (pad) return -1;   // data after padding
      v[k] = b64val(c);
      if (v[k] < 0) return -1;
    }
    const uint32_t x = (uint32_t(v[0]) << 18) | (uint32_t(v[1]) << 12) | (uint32_t(v[2]) << 6) | uint32_t(v[3]);
    const int bytes = 3 - pad;
    if (o + size_t(bytes) > cap) return -1;
    out[o++] = uint8_t(x >> 16);
    if (bytes > 1) out[o++] = uint8_t(x >> 8);
    if (bytes > 2) out[o++] = uint8_t(x);
  }
  return int(o);
}

void Xfer::init(FileSystem* fs, const char* firmwareVersion) {
  abort();
  fs_ = fs;
  fw_ = firmwareVersion ? firmwareVersion : "?";
  len_ = 0;
  inLine_ = ready_ = overflow_ = false;
  doneId_[0] = 0;
  doneReply_[0] = 0;
  stats_ = Stats{};
}

bool Xfer::rx(uint8_t b, uint32_t nowMs) {
  if (!inLine_) {
    if (b != '~') return false;          // not ours: a single-letter command or noise
    if (ready_) { ++stats_.overflows; return true; }   // previous line not serviced yet: drop
    inLine_ = true;
    overflow_ = false;
    len_ = 0;
    lineStartMs_ = nowMs;
  }
  if (b == '\r') return true;
  if (b == '\n') {
    inLine_ = false;
    if (overflow_) { ++stats_.overflows; return true; }
    line_[len_] = 0;
    ready_ = true;
    return true;
  }
  if (len_ < kMaxHostLine) line_[len_++] = char(b);
  else overflow_ = true;
  return true;
}

bool Xfer::service(const XferEnv& env, XferOut& out) {
  if (inLine_ && env.nowMs - lineStartMs_ > kLineTimeoutMs) {   // a line that never ended
    inLine_ = false;
    ++stats_.badLines;
  }
  if (mode_ != Mode::None && env.nowMs - lastMs_ > kSessionTimeoutMs) {
    abort();
    ++stats_.timeouts;
  }
  if (!ready_) return false;
  ready_ = false;
  ++stats_.lines;
  if (len_ < kPrefixLen + 10 || std::memcmp(line_, kPrefix, kPrefixLen) != 0) { ++stats_.badLines; return true; }
  char* body = line_ + kPrefixLen;
  char* sp = std::strrchr(body, ' ');
  uint32_t want = 0;
  if (!sp || std::strlen(sp + 1) != 8 || !parseU32(sp + 1, &want, 16)) { ++stats_.badLines; return true; }
  *sp = 0;
  if (crc32(0, reinterpret_cast<const uint8_t*>(body), std::strlen(body)) != want) { ++stats_.badCrc; return true; }
  handle(body, env, out);
  return true;
}

void Xfer::handle(char* body, const XferEnv& env, XferOut& out) {
  const char* tok[kMaxTokens] = {};
  int n = 0;
  for (char* p = body; *p && n < kMaxTokens;) {
    while (*p == ' ') ++p;
    if (!*p) break;
    tok[n++] = p;
    while (*p && *p != ' ') ++p;
    if (*p) *p++ = 0;
  }
  if (n < 2 || std::strlen(tok[0]) >= sizeof(id_)) { ++stats_.badLines; return; }
  for (const char* c = tok[0]; *c; ++c)
    if (*c < '0' || *c > '9') { ++stats_.badLines; return; }
  std::strcpy(id_, tok[0]);
  const char* cmd = tok[1];

  if (std::strcmp(cmd, "HELLO") == 0) {
    reply(out, "HI PGX %d %s %d", kProtoVersion, fw_, env.playing ? 1 : 0);
  } else if (std::strcmp(cmd, "LS") == 0) {
    doList(out);
  } else if (std::strcmp(cmd, "GET") == 0 && n == 4) {
    abort();
    doGet(tok[2], tok[3], out);
  } else if (std::strcmp(cmd, "RD") == 0 && n == 3) {
    uint32_t off;
    if (!parseU32(tok[2], &off, 10)) err(out, "PROTO");
    else doRead(off, out);
  } else if (std::strcmp(cmd, "PUT") == 0 && n == 7) {
    abort();
    doPut(tok, n, env, out);
  } else if (std::strcmp(cmd, "WR") == 0 && n == 4) {
    uint32_t off;
    if (!parseU32(tok[2], &off, 10)) err(out, "PROTO");
    else doWrite(off, tok[3], env, out);
  } else if (std::strcmp(cmd, "DONE") == 0) {
    doDone(out);
  } else if (std::strcmp(cmd, "SLS") == 0) {
    doSampleList(out);
  } else if (std::strcmp(cmd, "SPUT") == 0 && n == 6) {
    abort();
    doSamplePut(tok, env, out);
  } else if (std::strcmp(cmd, "SGET") == 0 && n == 3) {
    abort();
    doSampleGet(tok[2], out);
  } else if (std::strcmp(cmd, "SRM") == 0 && n == 3) {
    abort();
    doSampleRemove(tok[2], env, out);
  } else if (std::strcmp(cmd, "END") == 0) {
    abort();
    reply(out, "OK");
  } else {
    err(out, "UNKNOWN", cmd);
  }
  if (mode_ != Mode::None) lastMs_ = env.nowMs;   // any request keeps the session alive
}

void Xfer::reply(XferOut& out, const char* fmt, ...) {
  char buf[kReplyMax];
  int n = std::snprintf(buf, sizeof(buf), "%s%s ", kPrefix, id_);
  va_list ap;
  va_start(ap, fmt);
  const int m = std::vsnprintf(buf + n, sizeof(buf) - size_t(n) - 12, fmt, ap);
  va_end(ap);
  if (m < 0) return;
  n += m;
  if (n > int(sizeof(buf)) - 12) n = int(sizeof(buf)) - 12;
  const uint32_t c = crc32(0, reinterpret_cast<const uint8_t*>(buf + kPrefixLen), size_t(n - kPrefixLen));
  n += std::snprintf(buf + n, sizeof(buf) - size_t(n), " %08lX\n", static_cast<unsigned long>(c));
  out.send(buf, size_t(n));
}

void Xfer::err(XferOut& out, const char* code, const char* detail) {
  ++stats_.errors;
  if (detail) reply(out, "ERR %s %.40s", code, detail);
  else reply(out, "ERR %s", code);
}

void Xfer::closeFile() {
  if (h_ >= 0 && fs_) fs_->close(h_);
  h_ = -1;
}

void Xfer::abort() {
  closeFile();
  if (mode_ == Mode::Put && fs_) fs_->remove(path_);
  if (mode_ == Mode::SamplePut && samples_) samples_->cancel();   // the slot stays empty
  mode_ = Mode::None;
}

bool Xfer::sampleProgress(int* slot, uint32_t* done, uint32_t* total) const {
  if (mode_ != Mode::SamplePut) return false;
  *slot = slot_;
  *done = pos_;
  *total = size_;
  return true;
}

namespace {
// "1".."12" -> 0..11, else -1
int slotFromTok(const char* t) {
  uint32_t v = 0;
  if (!parseU32(t, &v, 10) || v < 1 || v > uint32_t(sampler::kPoolSlots)) return -1;
  return int(v) - 1;
}
}  // namespace

void Xfer::doSampleList(XferOut& out) {
  if (!samples_ || !samples_->ready()) { err(out, "NO_POOL"); return; }
  int n = 0;
  for (int s = 0; s < sampler::kPoolSlots; ++s) {
    const sampler::SlotHeader* h = samples_->pool()->header(s);
    if (!h) continue;
    char nm[sampler::kNameLen + 1];
    sampler::headerName(*h, nm);
    reply(out, "SLOT %d %lu %08lX %s", s + 1, static_cast<unsigned long>(h->samples),
          static_cast<unsigned long>(h->dataCrc), nm[0] ? nm : "-");
    ++n;
  }
  reply(out, "SEND %d", n);
}

void Xfer::doSamplePut(const char* const* tok, const XferEnv& env, XferOut& out) {
  uint32_t samples = 0, crc = 0;
  if (!samples_ || !samples_->ready()) { err(out, "NO_POOL"); return; }
  const int slot = slotFromTok(tok[2]);
  if (slot < 0) { err(out, "BAD_SLOT", tok[2]); return; }
  if (!parseU32(tok[3], &samples, 10) || !parseU32(tok[4], &crc, 16)) { err(out, "PROTO"); return; }
  if (env.playing) { err(out, "PLAYING"); return; }
  if (samples < sampler::kMinSamples || samples > sampler::kSlotMaxSamples) { err(out, "BAD_SIZE"); return; }
  const sampler::SampleWriter::Result r = samples_->begin(slot, samples);
  if (r != sampler::SampleWriter::kOk) { err(out, sampler::SampleWriter::resultName(r)); return; }
  sampler::cleanName(std::strcmp(tok[5], "-") == 0 ? "" : tok[5], sname_);
  sname_[sampler::kNameLen] = 0;
  slot_ = slot;
  size_ = samples * 2u;
  crc_ = crc;
  pos_ = 0;
  runCrc_ = 0;
  force_ = false;
  mode_ = Mode::SamplePut;
  ++stats_.puts;
  reply(out, "READY %d", kChunk);
}

void Xfer::doSampleDone(XferOut& out) {
  std::strcpy(doneId_, id_);
  if (pos_ != size_ || runCrc_ != crc_) {
    abort();
    std::snprintf(doneReply_, sizeof(doneReply_), "ERR %s", pos_ != size_ ? "SHORT" : "CRC");
    ++stats_.errors;
    reply(out, "%s", doneReply_);
    return;
  }
  const sampler::SampleWriter::Result r = samples_->commit(crc_, sname_);
  if (r != sampler::SampleWriter::kOk) {
    abort();
    std::snprintf(doneReply_, sizeof(doneReply_), "ERR %s", sampler::SampleWriter::resultName(r));
    ++stats_.errors;
    reply(out, "%s", doneReply_);
    return;
  }
  mode_ = Mode::None;
  ++samplesChanged_;
  std::snprintf(doneReply_, sizeof(doneReply_), "SSAVED %d %s %lu", slot_ + 1, sname_[0] ? sname_ : "-",
                static_cast<unsigned long>(size_ / 2u));
  reply(out, "%s", doneReply_);
}

void Xfer::doSampleGet(const char* slotTok, XferOut& out) {
  if (!samples_ || !samples_->ready()) { err(out, "NO_POOL"); return; }
  const int slot = slotFromTok(slotTok);
  if (slot < 0) { err(out, "BAD_SLOT", slotTok); return; }
  const sampler::SlotHeader* h = samples_->pool()->header(slot);
  if (!h) { err(out, "EMPTY"); return; }
  char nm[sampler::kNameLen + 1];
  sampler::headerName(*h, nm);
  slot_ = slot;
  size_ = h->samples * 2u;
  crc_ = h->dataCrc;
  pos_ = 0;
  mode_ = Mode::SampleGet;
  ++stats_.gets;
  reply(out, "SFILE %lu %08lX %s", static_cast<unsigned long>(size_), static_cast<unsigned long>(crc_),
        nm[0] ? nm : "-");
}

void Xfer::doSampleRemove(const char* slotTok, const XferEnv& env, XferOut& out) {
  if (!samples_ || !samples_->ready()) { err(out, "NO_POOL"); return; }
  const int slot = slotFromTok(slotTok);
  if (slot < 0) { err(out, "BAD_SLOT", slotTok); return; }
  if (env.playing) { err(out, "PLAYING"); return; }
  const sampler::SampleWriter::Result r = samples_->remove(slot);
  if (r != sampler::SampleWriter::kOk) { err(out, sampler::SampleWriter::resultName(r)); return; }
  ++samplesChanged_;
  reply(out, "OK");
}

void Xfer::doList(XferOut& out) {
  if (!fs_) { err(out, "NO_STORAGE"); return; }
  ProjectEntry e[kListCap];
  int total = 0;
  bool truncated = false;
  const char* mounts[2] = {kMountInt, kMountSd};
  for (const char* m : mounts) {
    if (!fs_->mounted(m)) continue;
    const int n = listProjects(*fs_, m, e, kListCap);
    if (n >= kListCap) truncated = true;
    for (int i = 0; i < n; ++i, ++total)
      reply(out, "ENT %s %s %lu %u %d", mountTok(m), e[i].name, static_cast<unsigned long>(e[i].size),
            unsigned(e[i].tempo), e[i].valid ? 1 : 0);
  }
  if (fs_->mounted(kMountInt) && fs_->exists(kSessionPath)) {
    HeaderInfo h;
    const bool ok = readHeader(*fs_, kSessionPath, &h) == Status::Ok;
    reply(out, "ENT int _last %lu %u %d", static_cast<unsigned long>(ok ? h.fileSize() : 0u),
          unsigned(ok ? h.tempo : 0), ok ? 1 : 0);
    ++total;
  }
  reply(out, "END %d %d %d", total, fs_->mounted(kMountSd) ? 1 : 0, truncated ? 1 : 0);
}

bool Xfer::reopenAt(uint32_t offset) {
  closeFile();
  h_ = fs_->open(path_, false);
  if (h_ < 0) return false;
  uint8_t buf[64];
  pos_ = 0;
  while (pos_ < offset) {
    const uint32_t want = offset - pos_ < sizeof(buf) ? offset - pos_ : uint32_t(sizeof(buf));
    const int r = fs_->readSome(h_, buf, want);
    if (r <= 0) return false;
    pos_ += uint32_t(r);
  }
  return true;
}

void Xfer::doGet(const char* mountT, const char* name, XferOut& out) {
  const char* m = mountFor(mountT);
  if (!fs_) { err(out, "NO_STORAGE"); return; }
  if (!m) { err(out, "BAD_NAME", mountT); return; }
  if (std::strcmp(name, "_last") == 0 && m == kMountInt) {
    std::strncpy(path_, kSessionPath, sizeof(path_) - 1);
    path_[sizeof(path_) - 1] = 0;
  } else if (!isValidName(name) || !buildProjectPath(m, name, path_, sizeof(path_))) {
    err(out, "BAD_NAME", name);
    return;
  }
  if (!fs_->mounted(m)) { err(out, "NO_MOUNT", mountT); return; }
  const int h = fs_->open(path_, false);
  if (h < 0) { err(out, fs_->exists(path_) ? "OPEN_FAILED" : "NOT_FOUND", name); return; }
  uint8_t buf[64];
  uint32_t size = 0, crc = 0;
  for (;;) {
    const int r = fs_->readSome(h, buf, sizeof(buf));
    if (r < 0) { fs_->close(h); err(out, "READ_FAILED"); return; }
    if (r == 0) break;
    crc = crc32(crc, buf, size_t(r));
    size += uint32_t(r);
    if (size > kMaxGetBytes) { fs_->close(h); err(out, "TOO_BIG"); return; }
  }
  fs_->close(h);
  if (size == 0) { err(out, "EMPTY"); return; }
  if (!reopenAt(0)) { closeFile(); err(out, "OPEN_FAILED"); return; }
  std::strncpy(mount_, mountT, sizeof(mount_) - 1);
  mount_[sizeof(mount_) - 1] = 0;
  std::strncpy(name_, name, sizeof(name_) - 1);
  name_[sizeof(name_) - 1] = 0;
  size_ = size;
  crc_ = crc;
  mode_ = Mode::Get;
  ++stats_.gets;
  reply(out, "FILE %lu %08lX", static_cast<unsigned long>(size), static_cast<unsigned long>(crc));
}

void Xfer::doRead(uint32_t offset, XferOut& out) {
  if (mode_ == Mode::SampleGet) {
    if (offset >= size_) { err(out, "BAD_OFFSET"); return; }
    uint8_t data[kChunk];
    const uint32_t want = size_ - offset < uint32_t(kChunk) ? size_ - offset : uint32_t(kChunk);
    if (samples_->read(slot_, offset, data, want) != sampler::SampleWriter::kOk) { abort(); err(out, "READ_FAILED"); return; }
    char b64[(kChunk + 2) / 3 * 4 + 1];
    base64Encode(data, want, b64, sizeof(b64));
    reply(out, "DATA %lu %s", static_cast<unsigned long>(offset), b64);
    return;
  }
  if (mode_ != Mode::Get) { err(out, "NO_SESSION"); return; }
  if (offset >= size_) { err(out, "BAD_OFFSET"); return; }
  if (offset != pos_ && !reopenAt(offset)) { abort(); err(out, "READ_FAILED"); return; }
  uint8_t data[kChunk];
  const uint32_t want = size_ - offset < uint32_t(kChunk) ? size_ - offset : uint32_t(kChunk);
  uint32_t got = 0;
  while (got < want) {
    const int r = fs_->readSome(h_, data + got, want - got);
    if (r <= 0) { abort(); err(out, "READ_FAILED"); return; }
    got += uint32_t(r);
  }
  pos_ = offset + got;
  char b64[(kChunk + 2) / 3 * 4 + 1];
  base64Encode(data, got, b64, sizeof(b64));
  reply(out, "DATA %lu %s", static_cast<unsigned long>(offset), b64);
}

void Xfer::doPut(const char* const* tok, int n, const XferEnv& env, XferOut& out) {
  (void)n;
  const char* m = mountFor(tok[2]);
  const char* name = tok[3];
  uint32_t size = 0, crc = 0, force = 0;
  if (!fs_) { err(out, "NO_STORAGE"); return; }
  if (!parseU32(tok[4], &size, 10) || !parseU32(tok[5], &crc, 16) || !parseU32(tok[6], &force, 10)) {
    err(out, "PROTO");
    return;
  }
  if (env.playing && !force) { err(out, "PLAYING"); return; }
  if (!m) { err(out, "BAD_NAME", tok[2]); return; }
  if (!isValidName(name)) { err(out, "BAD_NAME", name); return; }
  if (size < uint32_t(kHeaderSize) || size > kMaxPutBytes) { err(out, "BAD_SIZE"); return; }
  if (!fs_->mounted(m)) { err(out, "NO_MOUNT", tok[2]); return; }
  const long fb = fs_->freeBytes(m);
  if (fb >= 0 && fb < long(size) + kSaveSpaceMargin) { err(out, "NO_SPACE"); return; }
  if (!xferTempPath(m, path_, sizeof(path_))) { err(out, "BAD_NAME"); return; }
  if (fs_->exists(path_)) fs_->remove(path_);
  h_ = fs_->open(path_, true);
  if (h_ < 0) { err(out, "OPEN_FAILED"); return; }
  std::strncpy(mount_, tok[2], sizeof(mount_) - 1);
  mount_[sizeof(mount_) - 1] = 0;
  std::strncpy(name_, name, sizeof(name_) - 1);
  name_[sizeof(name_) - 1] = 0;
  size_ = size;
  crc_ = crc;
  pos_ = 0;
  runCrc_ = 0;
  force_ = force != 0;
  mode_ = Mode::Put;
  ++stats_.puts;
  reply(out, "READY %d", kChunk);
}

void Xfer::doWrite(uint32_t offset, const char* b64, const XferEnv& env, XferOut& out) {
  if (mode_ != Mode::Put && mode_ != Mode::SamplePut) { err(out, "NO_SESSION"); return; }
  if (env.playing && !force_) { abort(); err(out, "PLAYING"); return; }
  uint8_t data[kChunk];
  const int n = base64Decode(b64, data, sizeof(data));
  if (n <= 0) { abort(); err(out, "BAD_DATA"); return; }
  if (mode_ == Mode::SamplePut && offset == pos_) {
    if (pos_ + uint32_t(n) > size_) { abort(); err(out, "BAD_SIZE"); return; }
    const sampler::SampleWriter::Result r = samples_->write(offset, data, uint32_t(n));
    if (r != sampler::SampleWriter::kOk) { abort(); err(out, sampler::SampleWriter::resultName(r)); return; }
    runCrc_ = crc32(runCrc_, data, size_t(n));
    pos_ += uint32_t(n);
    reply(out, "ACK %lu", static_cast<unsigned long>(pos_));
  } else if (offset == pos_) {
    if (pos_ + uint32_t(n) > size_) { abort(); err(out, "BAD_SIZE"); return; }
    if (!fs_->writeSome(h_, data, size_t(n))) {
      const long fb = fs_->freeBytes(mountFor(mount_));
      abort();
      err(out, fb >= 0 && fb < kSaveSpaceMargin ? "NO_SPACE" : "WRITE_FAILED");
      return;
    }
    runCrc_ = crc32(runCrc_, data, size_t(n));
    pos_ += uint32_t(n);
    reply(out, "ACK %lu", static_cast<unsigned long>(pos_));
  } else if (offset + uint32_t(n) == pos_) {
    reply(out, "ACK %lu", static_cast<unsigned long>(pos_));   // a resend: its ACK was lost
  } else {
    reply(out, "NAK %lu", static_cast<unsigned long>(pos_));
  }
}

void Xfer::doDone(XferOut& out) {
  if (mode_ == Mode::SamplePut) { doSampleDone(out); return; }
  if (mode_ != Mode::Put) {
    if (doneId_[0] && std::strcmp(doneId_, id_) == 0) reply(out, "%s", doneReply_);   // resent DONE
    else err(out, "NO_SESSION");
    return;
  }
  std::strcpy(doneId_, id_);
  if (pos_ != size_ || runCrc_ != crc_) {
    abort();
    std::snprintf(doneReply_, sizeof(doneReply_), "ERR %s", pos_ != size_ ? "SHORT" : "CRC");
    ++stats_.errors;
    reply(out, "%s", doneReply_);
    return;
  }
  const bool closed = fs_->close(h_);
  h_ = -1;
  if (!closed) {
    abort();
    std::snprintf(doneReply_, sizeof(doneReply_), "ERR WRITE_FAILED");
    ++stats_.errors;
    reply(out, "%s", doneReply_);
    return;
  }
  const Status s = installReceived(*fs_, path_, mountFor(mount_), name_, size_, crc_);
  mode_ = Mode::None;   // installReceived removed or renamed the temp
  if (s == Status::Ok) {
    std::snprintf(doneReply_, sizeof(doneReply_), "SAVED %s %s", mount_, name_);
  } else {
    char t[32];
    statusToken(s, t, sizeof(t));
    std::snprintf(doneReply_, sizeof(doneReply_), "ERR %s", t);
    ++stats_.errors;
  }
  reply(out, "%s", doneReply_);
}

}  // namespace pg::io
