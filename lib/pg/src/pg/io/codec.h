// Internal to pg/io: CRC32, small buffered byte reader/writer over an abstract
// source/sink, and the PGLZ codec (format described in project_io.h).
#pragma once
#include <cstddef>
#include <cstdint>

namespace pg::io::detail {

constexpr size_t kIoBuf = 64;  // the only I/O buffer, per Reader / Writer

extern const uint32_t kCrcNibble[16];
inline uint32_t crcReg(uint32_t reg, uint8_t b) {  // raw register update (init ~0, final ~)
  reg ^= b;
  reg = (reg >> 4) ^ kCrcNibble[reg & 15u];
  reg = (reg >> 4) ^ kCrcNibble[reg & 15u];
  return reg;
}

class Source {
 public:
  virtual int readSome(uint8_t* buf, size_t cap) = 0;  // >0 bytes, 0 EOF, <0 error
 protected:
  ~Source() = default;
};
class Sink {
 public:
  virtual bool writeSome(const uint8_t* buf, size_t len) = 0;
 protected:
  ~Sink() = default;
};

class Reader {
 public:
  enum Fail : uint8_t { kNone = 0, kEof, kError, kLimit };
  Reader(Source& src, size_t chunk) : src_(src), chunk_(clampChunk(chunk)) {}

  bool get(uint8_t& b) {
    if (limited_ && consumed_ >= limit_) { fail_ = kLimit; return false; }
    if (pos_ == len_ && !fill()) return false;
    b = buf_[pos_++];
    ++consumed_;
    crc_ = crcReg(crc_, b);
    return true;
  }
  // Start counting/CRCing a new section; limit = max bytes get() may return.
  void begin(uint32_t limit, bool limited) { consumed_ = 0; crc_ = 0xFFFFFFFFu; limit_ = limit; limited_ = limited; fail_ = kNone; }
  uint32_t consumed() const { return consumed_; }
  uint32_t crc() const { return ~crc_; }
  Fail fail() const { return fail_; }
  // 1 = at EOF, 0 = more data follows, -1 = read error.
  int atEof();

 private:
  static size_t clampChunk(size_t c) { return c < 1 ? 1 : (c > kIoBuf ? kIoBuf : c); }
  bool fill();
  Source& src_;
  size_t chunk_;
  uint8_t buf_[kIoBuf];
  uint8_t pos_ = 0, len_ = 0;
  Fail fail_ = kNone;
  bool limited_ = false;
  uint32_t consumed_ = 0, limit_ = 0, crc_ = 0xFFFFFFFFu;
};

class Writer {
 public:
  Writer(Sink* sink, size_t chunk) : sink_(sink), chunk_(chunk < 1 ? 1 : (chunk > kIoBuf ? kIoBuf : chunk)) {}
  void put(uint8_t b) {
    crc_ = crcReg(crc_, b);
    ++count_;
    if (!sink_) return;
    buf_[n_++] = b;
    if (n_ >= chunk_) flush();
  }
  void put(const uint8_t* p, size_t n) { for (size_t i = 0; i < n; ++i) put(p[i]); }
  bool flush();
  void begin() { count_ = 0; crc_ = 0xFFFFFFFFu; }
  uint32_t count() const { return count_; }
  uint32_t crc() const { return ~crc_; }
  bool ok() const { return ok_; }

 private:
  Sink* sink_;  // nullptr = dry run (count + CRC only)
  size_t chunk_;
  uint8_t buf_[kIoBuf];
  size_t n_ = 0;
  bool ok_ = true;
  uint32_t count_ = 0, crc_ = 0xFFFFFFFFu;
};

enum class EncodeResult : uint8_t { Ok, Race, WriteError };
// Encodes src[0..n) into w. *rawCrc receives the CRC of the bytes as encoded.
// Race = the source changed under the encoder (detected inside a match).
EncodeResult lzEncode(const uint8_t* src, uint32_t n, Writer& w, uint32_t* rawCrc);

enum class DecodeResult : uint8_t { Ok, Corrupt, Truncated, ReadError };
// Decodes exactly n bytes. dest may be nullptr (structure check only). The
// first prefixLen output bytes are also reconstructed into prefix (if given),
// so magic/version can be checked without a destination. *rawCrc (if given)
// is only meaningful when dest != nullptr.
DecodeResult lzDecode(Reader& r, uint8_t* dest, uint32_t n, uint8_t* prefix, uint32_t prefixLen, uint32_t* rawCrc);

// Upper bound of the compressed size for n input bytes.
constexpr uint32_t lzMaxBody(uint32_t n) { return n + n / 128u + 16u; }

}  // namespace pg::io::detail
