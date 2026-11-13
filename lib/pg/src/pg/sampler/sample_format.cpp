#include "sample_format.h"

#include <cstring>

namespace pg {
namespace sampler {

uint32_t crc32(const void* data, size_t bytes, uint32_t crc) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  uint32_t c = ~crc;
  for (size_t i = 0; i < bytes; ++i) {
    c ^= p[i];
    for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
  }
  return ~c;
}

void makeOverview(const int16_t* data, uint32_t samples, int8_t (*out)[2]) {
  for (int c = 0; c < kOverviewCols; ++c) {
    const uint32_t a = uint32_t((uint64_t(samples) * uint32_t(c)) / kOverviewCols);
    uint32_t b = uint32_t((uint64_t(samples) * uint32_t(c + 1)) / kOverviewCols);
    if (b <= a) b = a + 1;
    int lo = 0, hi = 0;
    if (data && a < samples) {
      lo = hi = data[a];
      for (uint32_t i = a + 1; i < b && i < samples; ++i) {
        const int v = data[i];
        if (v < lo) lo = v;
        if (v > hi) hi = v;
      }
    }
    out[c][0] = int8_t(lo >> 8);   // arithmetic shift: -32768 -> -128
    out[c][1] = int8_t(hi >> 8);
  }
}

void cleanName(const char* in, char out[kNameLen]) {
  int n = 0;
  std::memset(out, 0, kNameLen);
  for (; in && *in && n < kNameLen; ++in) {
    char ch = *in;
    if (ch >= 'a' && ch <= 'z') ch = char(ch - 'a' + 'A');
    if ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_') out[n++] = ch;
  }
}

void makeHeader(const int16_t* data, uint32_t samples, const char* name, SlotHeader* h) {
  std::memset(h, 0, sizeof(*h));
  h->magic = kHeaderMagic;
  h->version = kHeaderVersion;
  h->samples = samples;
  h->rate = kPoolRate;
  cleanName(name, h->name);
  int peak = 0;
  for (uint32_t i = 0; i < samples; ++i) {
    const int v = data[i] < 0 ? -int(data[i]) : int(data[i]);
    if (v > peak) peak = v;
  }
  h->peak = uint16_t(peak);
  h->dataCrc = crc32(data, size_t(samples) * 2u);
  makeOverview(data, samples, h->overview);
  h->headerCrc = crc32(h, offsetof(SlotHeader, headerCrc));
}

bool headerValid(const SlotHeader& h) {
  return h.magic == kHeaderMagic && h.version == kHeaderVersion && h.rate == kPoolRate &&
         h.samples >= kMinSamples && h.samples <= kSlotMaxSamples &&
         h.headerCrc == crc32(&h, offsetof(SlotHeader, headerCrc));
}

void headerName(const SlotHeader& h, char* out) {
  std::memcpy(out, h.name, kNameLen);
  out[kNameLen] = 0;
}

}  // namespace sampler
}  // namespace pg
