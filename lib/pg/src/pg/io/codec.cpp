#include "pg/io/codec.h"

#include "pg/project.h"

namespace pg::io::detail {

const uint32_t kCrcNibble[16] = {
    0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu, 0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
    0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu, 0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu,
};

// ---- Reader / Writer --------------------------------------------------------
bool Reader::fill() {
  if (fail_ != kNone) return false;
  const int n = src_.readSome(buf_, chunk_);
  if (n < 0 || size_t(n) > chunk_) { fail_ = kError; return false; }  // n > cap: broken FS, treat as error
  if (n == 0) { fail_ = kEof; return false; }
  pos_ = 0;
  len_ = uint8_t(n);
  return true;
}

int Reader::atEof() {
  if (pos_ != len_) return 0;
  if (fail_ == kEof) return 1;
  if (fail_ == kError) return -1;
  const int n = src_.readSome(buf_, chunk_);
  if (n < 0) return -1;
  if (n == 0) { fail_ = kEof; return 1; }
  return 0;
}

bool Writer::flush() {
  if (!sink_ || n_ == 0) return ok_;
  if (ok_ && !sink_->writeSome(buf_, n_)) ok_ = false;
  n_ = 0;
  return ok_;
}

// ---- PGLZ encoder -----------------------------------------------------------
namespace {

constexpr uint32_t kMinMatch = 4;
constexpr int kHashBits = 6;  // 64 x u16 = 128 bytes of encoder state
constexpr uint32_t kMaxLiteralRun = 128;

constexpr uint32_t varintSize(uint32_t v) { return v < (1u << 7) ? 1u : (v < (1u << 14) ? 2u : 3u); }

void putVarint(Writer& w, uint32_t v) {
  while (v >= 0x80u) { w.put(uint8_t(v | 0x80u)); v >>= 7; }
  w.put(uint8_t(v));
}

// Fixed candidate distances that match the Project layout: neighbouring
// bytes/steps, the same step in the neighbouring tracks, the same byte in
// any earlier pattern. Plus one hash candidate per position.
struct Offsets {
  uint32_t v[13 + 7 + 15];
  int n = 0;
  constexpr Offsets() : v{} {
    const uint32_t small[13] = {1, 2, 3, 4, 8, 16, 24, 32, 40, 48, 64, 128, 256};
    for (uint32_t s : small) v[n++] = s;
    for (uint32_t k = 1; k <= 7; ++k) v[n++] = k * uint32_t(sizeof(TrackPattern));
    for (uint32_t k = 1; k < uint32_t(kNumPatterns); ++k) v[n++] = k * uint32_t(sizeof(Pattern));
  }
};
constexpr Offsets kOffsets;

inline uint32_t hash4(const uint8_t* p) {
  const uint32_t x = uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
  return (x * 2654435761u) >> (32 - kHashBits);
}

void putLiterals(Writer& w, const uint8_t* src, uint32_t from, uint32_t to, uint32_t& crc) {
  while (from < to) {
    uint32_t run = to - from;
    if (run > kMaxLiteralRun) run = kMaxLiteralRun;
    w.put(uint8_t(run - 1));
    for (uint32_t i = 0; i < run; ++i) {
      const uint8_t b = src[from + i];  // read once: the CRC sees exactly what the file gets
      w.put(b);
      crc = crcReg(crc, b);
    }
    from += run;
  }
}

}  // namespace

EncodeResult lzEncode(const uint8_t* src, uint32_t n, Writer& w, uint32_t* rawCrc) {
  uint16_t table[1u << kHashBits] = {};  // low 16 bits of the last position with that hash
  uint32_t crc = 0xFFFFFFFFu;
  uint32_t pos = 0, lit = 0;
  while (pos < n) {
    const uint32_t remain = n - pos;
    uint32_t bestLen = 0, bestOff = 0;
    int bestScore = 0;
    if (remain >= kMinMatch) {
      const uint8_t* a = src + pos;
      auto tryOff = [&](uint32_t off) {
        if (off == 0 || off > pos) return;
        const uint8_t* b = a - off;
        if (b[0] != a[0]) return;
        if (bestLen && bestLen < remain && b[bestLen] != a[bestLen]) return;  // can't beat best
        uint32_t l = 1;
        while (l < remain && a[l] == b[l]) ++l;
        const uint32_t need = varintSize(off) >= 3 ? kMinMatch + 1 : kMinMatch;
        if (l < need) return;
        const int score = int(l) - int(varintSize(off));
        if (score > bestScore) { bestScore = score; bestLen = l; bestOff = off; }
      };
      for (int i = 0; i < kOffsets.n; ++i) tryOff(kOffsets.v[i]);
      const uint32_t h = hash4(a);
      if (pos >= 0x10000u || table[h] < pos) {
        uint32_t cand = (pos & ~0xFFFFu) | table[h];
        if (cand >= pos) cand -= 0x10000u;  // pos >= 0x10000 here, no underflow
        tryOff(pos - cand);
      }
      table[h] = uint16_t(pos);
    }
    if (bestLen == 0) {
      ++pos;
      if (pos - lit == kMaxLiteralRun) { putLiterals(w, src, lit, pos, crc); lit = pos; }
      continue;
    }
    putLiterals(w, src, lit, pos, crc);
    // Re-read the match and CRC exactly the bytes compared: if the engine
    // changed one since the search, the file and the CRC would disagree.
    for (uint32_t i = 0; i < bestLen; ++i) {
      const uint8_t x = src[pos + i];
      if (x != src[pos + i - bestOff]) return EncodeResult::Race;
      crc = crcReg(crc, x);
    }
    const uint32_t l4 = bestLen - kMinMatch;
    if (l4 < 0x7Fu) {
      w.put(uint8_t(0x80u | l4));
    } else {
      w.put(0xFF);
      putVarint(w, l4 - 0x7Fu);
    }
    putVarint(w, bestOff);
    const uint32_t end = pos + bestLen;
    for (uint32_t q = pos + 1; q < end && q + kMinMatch <= n; ++q) table[hash4(src + q)] = uint16_t(q);
    pos = end;
    lit = pos;
  }
  putLiterals(w, src, lit, n, crc);
  if (!w.flush()) return EncodeResult::WriteError;
  *rawCrc = ~crc;
  return EncodeResult::Ok;
}

// ---- PGLZ decoder -----------------------------------------------------------
namespace {
DecodeResult failOf(const Reader& r) {
  switch (r.fail()) {
    case Reader::kEof: return DecodeResult::Truncated;
    case Reader::kError: return DecodeResult::ReadError;
    default: return DecodeResult::Corrupt;  // body limit reached before output complete
  }
}
// LEB128, at most 3 bytes (21 bits).
bool getVarint(Reader& r, uint32_t& v, DecodeResult& err) {
  v = 0;
  for (int i = 0; i < 3; ++i) {
    uint8_t b;
    if (!r.get(b)) { err = failOf(r); return false; }
    v |= uint32_t(b & 0x7Fu) << (7 * i);
    if (!(b & 0x80u)) return true;
  }
  err = DecodeResult::Corrupt;
  return false;
}
}  // namespace

DecodeResult lzDecode(Reader& r, uint8_t* dest, uint32_t n, uint8_t* prefix, uint32_t prefixLen, uint32_t* rawCrc) {
  uint32_t crc = 0xFFFFFFFFu;
  uint32_t pos = 0;
  DecodeResult err = DecodeResult::Corrupt;
  if (!prefix) prefixLen = 0;
  // Every iteration consumes >= 1 input byte and the reader is bounded, so
  // this terminates; every write is checked against n.
  while (pos < n) {
    uint8_t c;
    if (!r.get(c)) return failOf(r);
    if (c < 0x80u) {
      const uint32_t run = uint32_t(c) + 1u;
      if (run > n - pos) return DecodeResult::Corrupt;
      for (uint32_t i = 0; i < run; ++i, ++pos) {
        uint8_t b;
        if (!r.get(b)) return failOf(r);
        if (dest) { dest[pos] = b; crc = crcReg(crc, b); }
        if (pos < prefixLen) prefix[pos] = b;
      }
    } else {
      uint32_t len = kMinMatch + (c & 0x7Fu), off;
      if ((c & 0x7Fu) == 0x7Fu) {
        uint32_t ext;
        if (!getVarint(r, ext, err)) return err;
        len += ext;
      }
      if (!getVarint(r, off, err)) return err;
      if (off == 0 || off > pos || len > n - pos) return DecodeResult::Corrupt;
      for (uint32_t i = 0; i < len; ++i, ++pos) {
        const uint32_t from = pos - off;
        if (dest) { const uint8_t b = dest[from]; dest[pos] = b; crc = crcReg(crc, b); }
        if (pos < prefixLen) prefix[pos] = prefix[from];  // from < pos < prefixLen
      }
    }
  }
  if (rawCrc) *rawCrc = ~crc;
  return DecodeResult::Ok;
}

}  // namespace pg::io::detail
