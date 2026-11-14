#include "settings.h"

#include <cstring>

#include "pg/sampler/sample_format.h"   // crc32

namespace pgd {

namespace {
struct W {
  uint8_t* p;
  void u8(uint8_t v) { *p++ = v; }
  void f32(float v) {
    std::memcpy(p, &v, 4);
    p += 4;
  }
};
struct R {
  const uint8_t* p;
  uint8_t u8() { return *p++; }
  float f32() {
    float v;
    std::memcpy(&v, p, 4);
    p += 4;
    return v;
  }
};
}  // namespace

size_t DaisySettings::encode(uint8_t* out, size_t cap) const {
  if (cap < kBytes) return 0;
  W w{out};
  w.u8('P');
  w.u8('G');
  w.u8('D');
  w.u8('S');
  w.u8(kVersion);
  w.u8(uint8_t(kBytes));
  w.u8(PadScanner::kPads);
  w.u8(0);
  for (const PadCal& c : pads) {
    w.f32(c.threshold);
    w.f32(c.ceiling);
    w.f32(c.curve);
    w.u8(c.on ? 1 : 0);
  }
  w.u8(padGlobal.windowScans);
  w.u8(padGlobal.releaseScans);
  w.u8(padGlobal.xtalkScans);
  w.f32(padGlobal.xtalkRatio);
  w.f32(padGlobal.releaseRatio);
  w.u8(displayTurned ? 1 : 0);
  w.u8(uint8_t(touchOrientation));
  w.u8(spiDivider);
  w.u8(padJob);
  const uint32_t crc = pg::sampler::crc32(out, size_t(w.p - out));
  std::memcpy(w.p, &crc, 4);
  return kBytes;
}

bool DaisySettings::decode(const uint8_t* in, size_t n) {
  if (!in || n < 8 || std::memcmp(in, "PGDS", 4) != 0 || in[6] != PadScanner::kPads) return false;
  const size_t want = in[4] == 1 ? kBytesV1 : in[4] == kVersion ? kBytes : 0;
  if (!want || n != want || in[5] != want) return false;
  uint32_t crc;
  std::memcpy(&crc, in + want - 4, 4);
  if (crc != pg::sampler::crc32(in, want - 4)) return false;
  DaisySettings s;
  R r{in + 8};
  for (PadCal& c : s.pads) {
    c.threshold = r.f32();
    c.ceiling = r.f32();
    c.curve = r.f32();
    c.on = r.u8() != 0;
    c = PadScanner::sanitize(c);
  }
  s.padGlobal.windowScans = r.u8();
  s.padGlobal.releaseScans = r.u8();
  s.padGlobal.xtalkScans = r.u8();
  s.padGlobal.xtalkRatio = r.f32();
  s.padGlobal.releaseRatio = r.f32();
  s.padGlobal = PadScanner::sanitize(s.padGlobal);
  s.displayTurned = r.u8() != 0;
  s.touchOrientation = int8_t(r.u8());
  if (s.touchOrientation < -1 || s.touchOrientation > 7) s.touchOrientation = -1;
  s.spiDivider = r.u8();
  if (s.spiDivider != 2 && s.spiDivider != 4 && s.spiDivider != 8 && s.spiDivider != 16) s.spiDivider = 2;
  s.padJob = want == kBytes ? r.u8() : 0;
  if (s.padJob > 1) s.padJob = 0;
  *this = s;
  return true;
}

}  // namespace pgd
