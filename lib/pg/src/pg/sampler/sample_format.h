// The sample pool's slot format (docs/MANUAL.md "Samples", docs/STATE.md stage F1).
//
// TWELVE SLOTS, each a fixed region of the unused OTA slot `app1` on the device
// (stock default_8MB.csv: 0x340000 + 0x330000 = exactly 12 x 68 sectors):
//
//   sector 0        the header (SlotHeader, below; the rest of the sector is 0xFF)
//   sectors 1..67   the audio: mono int16 little-endian, 22,050 Hz, up to
//                   kSlotMaxSamples (274,432 bytes, 6.22 s)
//
// A slot without a valid header (magic, version, header CRC, sane counts) is
// EMPTY: it plays a built-in sound (rom_samples.h). So an erased slot, the
// hw-spike's own recordings (which live in the same partition, docs/hw-spike.md)
// and anything half-written are all empty.
//
// The header carries what the screen needs without reading the audio: the
// name, the peak and a 232-column waveform overview (min / max per column, as
// signed bytes: the sample's high byte). The desktop keeps the same headers in
// RAM. Portable, no heap; both little-endian targets read the struct directly.
#pragma once
#include <cstddef>
#include <cstdint>

#include "../config.h"

namespace pg {
namespace sampler {

constexpr int kPoolSlots = kSampleSlots;
constexpr uint32_t kSectorBytes = 4096;
constexpr uint32_t kSlotSectors = 68;
constexpr uint32_t kSlotBytes = kSlotSectors * kSectorBytes;              // 278,528
constexpr uint32_t kSlotAudioBytes = kSlotBytes - kSectorBytes;            // 274,432
constexpr uint32_t kSlotMaxSamples = kSlotAudioBytes / 2;                  // 137,216
constexpr uint32_t kPoolBytes = kPoolSlots * kSlotBytes;                   // 3,342,336 = 0x330000
constexpr uint32_t kPoolRate = 22050;
constexpr uint32_t kMinSamples = 64;     // shorter is refused (and plays as empty)
constexpr int kNameLen = 8;
constexpr int kOverviewCols = 232;

constexpr uint32_t kHeaderMagic = 0x4D534750u;   // "PGSM"
constexpr uint16_t kHeaderVersion = 1;

struct SlotHeader {
  uint32_t magic;          // kHeaderMagic
  uint16_t version;        // kHeaderVersion
  uint16_t flags;          // 0
  uint32_t samples;        // kMinSamples..kSlotMaxSamples
  uint32_t rate;           // kPoolRate
  char     name[kNameLen]; // upper case A-Z 0-9 - _, NUL padded (not terminated when 8 long)
  uint16_t peak;           // largest |sample|, 0..32768
  uint16_t gain;           // playback gain in 1/256 (0 = 1.0, what every put sample has): a
                           // microphone recording is stored as it came in and levelled here
                           // (stage F2, setPlaybackGain); headers before F2 read 0 (was reserved)
  uint32_t dataCrc;        // crc32 of the audio bytes (samples * 2)
  int8_t   overview[kOverviewCols][2];   // per column: min, max (sample >> 8; times the gain)
  uint32_t headerCrc;      // crc32 of every byte before this field
};
static_assert(sizeof(SlotHeader) == 500, "SlotHeader layout");
static_assert(offsetof(SlotHeader, overview) == 32 && offsetof(SlotHeader, headerCrc) == 496, "SlotHeader layout");
static_assert(kPoolBytes == 0x330000, "twelve slots fill app1 exactly");

// CRC-32 (IEEE, reflected, as zlib), chainable: crc32(b, n, crc32(a, m)) == crc32 of a then b.
uint32_t crc32(const void* data, size_t bytes, uint32_t crc = 0);

// The overview of `samples` (kOverviewCols min/max pairs).
void makeOverview(const int16_t* data, uint32_t samples, int8_t (*out)[2]);
// A complete header for this audio (name: any string; it is cleaned into the
// 8-character form: upper case, A-Z 0-9 - _, the rest dropped).
void makeHeader(const int16_t* data, uint32_t samples, const char* name, SlotHeader* out);
void cleanName(const char* in, char out[kNameLen]);
// A microphone recording (stage F2) is written as it came in (no second pass over
// the flash): its level is made right at playback. This sets h->gain so the peak
// plays at kRecordTargetPeak (at most kRecordMaxGain; a peak already there or above
// keeps 0 = 1.0), scales the overview the same way (the picture shows what plays)
// and recomputes the header CRC.
constexpr uint16_t kRecordTargetPeak = 29195;   // -1 dBFS: what the converter levels a put to (0.891)
constexpr float kRecordMaxGain = 16.0f;         // +24 dB: a quiet room is not lifted into hiss
void setPlaybackGain(SlotHeader* h);
// The header's gain as a factor (0 = 1.0).
inline float gainOf(const SlotHeader& h) { return h.gain ? float(h.gain) * (1.0f / 256.0f) : 1.0f; }
// magic, version, header CRC, rate and count in range. Does not read the audio.
bool headerValid(const SlotHeader& h);
// The header's name as a C string (out: kNameLen + 1).
void headerName(const SlotHeader& h, char* out);

}  // namespace sampler
}  // namespace pg
