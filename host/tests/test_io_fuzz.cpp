// Decoder fuzzing: random bytes, random bytes behind a valid header, forged
// token streams with correct CRCs (so the LZ structure checks are what stops
// them), every bit flip and every truncation of valid files.
// Contract checked on every input: status != Ok unless the output is exactly
// right; destination untouched (sentinel) on every error except
// DestClobbered, where it is all zero. Run under ASan/UBSan for bounds.
#include "tests/test_io_util.h"

using namespace pg;
using namespace pg::io;
using namespace iot;

static int g_runs = 0, g_clobbered = 0;

static void tryDecode(const std::vector<uint8_t>& f, const Project* expect) {
  static auto d = newProject();
  sentinel(*d);
  const Status s = decodeFromBuffer(f.data(), f.size(), *d);
  ++g_runs;
  if (s == Status::Ok) {
    CHECK(expect && same(*d, *expect));
  } else if (s == Status::DestClobbered) {
    ++g_clobbered;
    CHECK(allZero(*d));
  } else {
    CHECK(isSentinel(*d));
  }
}

static void put32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i)); }

// Valid header around an arbitrary body (both CRCs of the header/body right).
static std::vector<uint8_t> wrap(const std::vector<uint8_t>& body, uint32_t rawCrc) {
  std::vector<uint8_t> f(kHeaderSize, 0);
  std::memcpy(f.data(), "PGVF", 4);
  f[4] = 1; f[5] = 1; f[6] = kHeaderSize; f[7] = 0;
  f[8] = kProjectVersion; f[9] = 1; f[10] = 124;
  put32(&f[12], uint32_t(sizeof(Project)));
  put32(&f[16], uint32_t(body.size()));
  put32(&f[20], rawCrc);
  put32(&f[24], crc32(0, body.data(), body.size()));
  std::memcpy(&f[28], "FUZZ", 4);
  put32(&f[kHeaderSize - 4], crc32(0, f.data(), kHeaderSize - 4));
  f.insert(f.end(), body.begin(), body.end());
  return f;
}

static void varint(std::vector<uint8_t>& b, uint32_t v) {
  while (v >= 0x80) { b.push_back(uint8_t(v | 0x80)); v >>= 7; }
  b.push_back(uint8_t(v));
}

// Plausible-but-wrong token stream: mostly in-bounds tokens, sometimes not.
static std::vector<uint8_t> forgedBody(Rng& r, uint32_t* producedOut) {
  std::vector<uint8_t> b;
  uint32_t pos = 0;
  const uint32_t n = uint32_t(sizeof(Project));
  b.push_back(4); b.push_back('P'); b.push_back('G'); b.push_back('R'); b.push_back('V'); b.push_back(kProjectVersion);
  pos = 5;
  const int tokens = 1 + int(r.below(200));
  for (int t = 0; t < tokens && pos < n + 100; ++t) {
    const uint32_t kind = r.below(10);
    if (kind < 3) {
      const uint32_t run = 1 + r.below(128);
      b.push_back(uint8_t(run - 1));
      for (uint32_t i = 0; i < run; ++i) b.push_back(r.byte());
      pos += run;
    } else if (kind < 9) {
      uint32_t len = 4 + r.below(r.below(4) == 0 ? 70000 : 200);
      uint32_t off = r.below(8) == 0 ? r.below(pos + 3) : 1 + r.below(pos);
      if (r.below(30) == 0) len = n - pos;  // exactly to the end, sometimes
      if (len - 4 < 0x7F) b.push_back(uint8_t(0x80 | (len - 4)));
      else { b.push_back(0xFF); varint(b, len - 4 - 0x7F); }
      varint(b, off);
      pos += len;
    } else {
      b.push_back(r.byte());  // raw garbage token
      if (r.below(2)) { b.push_back(0xFF); b.push_back(0xFF); b.push_back(0xFF); b.push_back(0xFF); }
    }
  }
  *producedOut = pos;
  return b;
}

int main() {
  auto empty = newProject(), typical = newProject(), rnd = newProject();
  initProject(*empty);
  typicalProject(*typical);
  randomProject(*rnd);
  const auto fe = encode(*empty), ft = encode(*typical), fr = encode(*rnd);
  CHECK(!fe.empty() && !ft.empty() && !fr.empty());
  Rng r(2024);

  // 1. pure random bytes, random lengths
  for (int i = 0; i < 3000; ++i) {
    std::vector<uint8_t> f(r.below(400));
    for (auto& x : f) x = r.byte();
    if (i % 3 == 0 && f.size() >= 4) std::memcpy(f.data(), "PGVF", 4);
    tryDecode(f, nullptr);
  }
  // 2. valid header, random body (body CRC right, so the decoder runs)
  for (int i = 0; i < 3000; ++i) {
    std::vector<uint8_t> body(1 + r.below(i % 10 == 0 ? 70000 : 600));
    for (auto& x : body) x = r.byte();
    tryDecode(wrap(body, r.next()), nullptr);
  }
  // 3. forged token streams, with correct CRCs
  for (int i = 0; i < 20000; ++i) {
    uint32_t produced = 0;
    auto body = forgedBody(r, &produced);
    if (body.empty() || body.size() > sizeof(Project) + 2048) continue;
    tryDecode(wrap(body, r.next()), nullptr);
  }
  // 4. a real body re-wrapped with a wrong rawCrc: passes pass 1, must clobber
  {
    std::vector<uint8_t> body(ft.begin() + kHeaderSize, ft.end());
    const int before = g_clobbered;
    tryDecode(wrap(body, 0x12345678u), nullptr);
    CHECK(g_clobbered == before + 1);
    uint32_t raw = crc32(0, reinterpret_cast<const uint8_t*>(typical.get()), sizeof(Project));
    tryDecode(wrap(body, raw), typical.get());
  }
  // 5. every bit flip of the empty and typical files; sampled flips of the random one
  int flips = 0;
  for (const auto* src : {&fe, &ft}) {
    for (size_t i = 0; i < src->size(); ++i)
      for (int bit = 0; bit < 8; ++bit) {
        auto f = *src;
        f[i] ^= uint8_t(1u << bit);
        tryDecode(f, nullptr);  // CRC32 detects every single-bit error
        ++flips;
      }
  }
  for (int i = 0; i < 400; ++i) {
    auto f = fr;
    f[r.below(uint32_t(f.size()))] ^= uint8_t(1u << r.below(8));
    tryDecode(f, nullptr);
    ++flips;
  }
  // multi-byte random corruption
  for (int i = 0; i < 2000; ++i) {
    auto f = ft;
    const int n = 1 + int(r.below(8));
    for (int k = 0; k < n; ++k) f[r.below(uint32_t(f.size()))] = r.byte();
    if (f == ft) continue;
    tryDecode(f, nullptr);
  }
  // 6. every truncation (and extension) of the empty and typical files
  int truncs = 0;
  for (const auto* src : {&fe, &ft}) {
    for (size_t l = 0; l < src->size(); ++l) {
      std::vector<uint8_t> f(src->begin(), src->begin() + long(l));
      tryDecode(f, nullptr);
      ++truncs;
    }
    auto f = *src;
    f.push_back(0);
    tryDecode(f, nullptr);
  }
  for (size_t l = 0; l < fr.size(); l += (l < 200 || l > fr.size() - 200) ? 1 : 97) {
    std::vector<uint8_t> f(fr.begin(), fr.begin() + long(l));
    tryDecode(f, nullptr);
    ++truncs;
  }
  // 7. the originals still decode
  tryDecode(fe, empty.get());
  tryDecode(ft, typical.get());
  tryDecode(fr, rnd.get());
  std::printf("fuzz: %d decodes (%d bit flips, %d truncations), %d clobbered (forged CRC-valid bodies only)\n", g_runs,
              flips, truncs, g_clobbered);
  return finish("test_io_fuzz");
}
