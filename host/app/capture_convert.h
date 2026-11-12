// Listen's capture side, between SDL's capture callback and DriftBuffer
// (listen_buffer.h): whatever format SDL hands us goes through ONE explicit,
// tested path to interleaved stereo int16 at the capture rate.
//
// SDL's capture device is opened asking for s16 stereo at the engine's rate,
// but the code never assumes it got that: SdlCapture reads the obtained spec
// back and CaptureSink converts from it.
//   format    any SDL audio format: u8 s8 u16 s16 s32 f32, either endianness
//             (decoded from the format bits, which SDL2 and SDL3 share:
//             bits 0-7 = bits per sample, 0x100 float, 0x1000 big endian,
//             0x8000 signed); floats are scaled by 32768 and saturated
//   channels  1 = the same on both sides; 2 as is; more = the first two
//   rate      left alone here: DriftBuffer reads at capture rate / output rate
//   size      any callback length, also one that splits a frame (the partial
//             frame waits for the next callback)
// Also here: the callback statistics (sizes, bursts, gaps) for the
// diagnostics, and the tap for --listen-dump (the raw bytes as delivered,
// before any conversion, into a lock-free byte ring a writer thread drains).
//
// The capture thread's side (deliver) takes no lock, allocates nothing and
// makes no system call (steady_clock is a vDSO / mach_absolute_time read).
// Pure C++ (no SDL), so test_desktop_listen runs it.
#pragma once
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "listen_buffer.h"

namespace desk {

// SDL_AudioFormat values (identical in SDL2 and SDL3).
constexpr uint16_t kFmtU8 = 0x0008, kFmtS8 = 0x8008;
constexpr uint16_t kFmtU16LE = 0x0010, kFmtS16LE = 0x8010, kFmtU16BE = 0x1010, kFmtS16BE = 0x9010;
constexpr uint16_t kFmtS32LE = 0x8020, kFmtS32BE = 0x9020, kFmtF32LE = 0x8120, kFmtF32BE = 0x9120;

struct CaptureSpec {
  uint16_t format = kFmtS16LE;
  int channels = 2;
  int rate = 44100;
  int frames = 256;   // per callback, as SDL reports it (sdl2-compat: just our request echoed)

  int bits() const { return format & 0xFF; }
  bool isFloat() const { return (format & 0x100) != 0; }
  bool bigEndian() const { return (format & 0x1000) != 0; }
  bool isSigned() const { return (format & 0x8000) != 0; }
  int bytesPerSample() const { return bits() / 8; }
  int bytesPerFrame() const { return bytesPerSample() * channels; }
  bool supported() const {
    const int b = bits();
    if (b != 8 && b != 16 && b != 32) return false;
    if (isFloat() && b != 32) return false;
    if (isFloat() && !isSigned()) return false;
    return channels >= 1 && channels <= 8 && rate >= 4000 && rate <= 384000;
  }
  bool operator==(const CaptureSpec& o) const {
    return format == o.format && channels == o.channels && rate == o.rate && frames == o.frames;
  }
  std::string formatName() const {
    char t[16];
    std::snprintf(t, sizeof(t), "%s%d%s", isFloat() ? "f" : isSigned() ? "s" : "u", bits(),
                  bits() == 8 ? "" : bigEndian() ? "be" : "le");
    return t;
  }
  std::string describe() const {
    char t[96];
    std::snprintf(t, sizeof(t), "%s, %d ch, %d Hz, %d frames per callback", formatName().c_str(), channels, rate,
                  frames);
    return t;
  }
  // "f32:1:48000:1024" (format name or hex code : channels : rate : frames); false if malformed.
  static bool parse(const char* s, CaptureSpec* out);
};

// One sample at p in format fmt, as int16 (saturating).
inline int16_t sampleToS16(const uint8_t* p, const CaptureSpec& s) {
  const int b = s.bits();
  if (b == 8) return s.isSigned() ? int16_t(int(int8_t(p[0])) * 256) : int16_t((int(p[0]) - 128) * 256);
  if (b == 16) {
    uint16_t u = s.bigEndian() ? uint16_t(p[0] << 8 | p[1]) : uint16_t(p[0] | p[1] << 8);
    if (!s.isSigned()) u ^= 0x8000;
    return int16_t(u);
  }
  if (b == 32) {
    const uint32_t u = s.bigEndian() ? (uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3])
                                     : (uint32_t(p[3]) << 24 | uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | p[0]);
    if (s.isFloat()) {
      float f;
      std::memcpy(&f, &u, 4);
      if (!(f == f)) return 0;   // NaN
      const float v = f * 32768.0f;
      if (v >= 32767.0f) return 32767;
      if (v <= -32768.0f) return -32768;
      return int16_t(std::lrint(v));
    }
    const int64_t v = (int64_t(int32_t(s.isSigned() ? u : u ^ 0x80000000u)) + 32768) >> 16;
    return int16_t(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
  }
  return 0;
}

// The inverse for the tests and the simulated device: one int16 value as a sample of format fmt.
inline void s16ToSample(int16_t v, const CaptureSpec& s, uint8_t* p) {
  const int b = s.bits();
  if (b == 8) {
    p[0] = s.isSigned() ? uint8_t(int8_t(v >> 8)) : uint8_t((v >> 8) + 128);
    return;
  }
  uint32_t u;
  if (b == 16) u = s.isSigned() ? uint16_t(v) : uint16_t(uint16_t(v) ^ 0x8000);
  else if (s.isFloat()) {
    const float f = float(v) / 32768.0f;
    std::memcpy(&u, &f, 4);
  } else {
    u = uint32_t(int32_t(v) * 65536);
    if (!s.isSigned()) u ^= 0x80000000u;
  }
  const int n = b / 8;
  for (int i = 0; i < n; ++i) p[s.bigEndian() ? n - 1 - i : i] = uint8_t(u >> (8 * i));
}

inline bool CaptureSpec::parse(const char* str, CaptureSpec* out) {
  char name[16] = {};
  int ch = 0, rate = 0, frames = 0;
  if (std::sscanf(str, "%15[^:]:%d:%d:%d", name, &ch, &rate, &frames) != 4) return false;
  static const struct { const char* n; uint16_t f; } kNames[] = {
      {"u8", kFmtU8}, {"s8", kFmtS8}, {"u16", kFmtU16LE}, {"u16le", kFmtU16LE}, {"u16be", kFmtU16BE},
      {"s16", kFmtS16LE}, {"s16le", kFmtS16LE}, {"s16be", kFmtS16BE}, {"s32", kFmtS32LE}, {"s32le", kFmtS32LE},
      {"s32be", kFmtS32BE}, {"f32", kFmtF32LE}, {"f32le", kFmtF32LE}, {"f32be", kFmtF32BE}};
  CaptureSpec s;
  bool found = false;
  for (const auto& k : kNames)
    if (!std::strcmp(k.n, name)) { s.format = k.f; found = true; }
  if (!found) {
    char* end = nullptr;
    const unsigned long v = std::strtoul(name, &end, 0);
    if (!end || *end || v == 0 || v > 0xFFFF) return false;
    s.format = uint16_t(v);
  }
  s.channels = ch;
  s.rate = rate;
  s.frames = frames;
  if (!s.supported() || frames < 1 || frames > 65535) return false;
  *out = s;
  return true;
}

// Callback sizes and timing, from the callback's thread (relaxed atomics; the
// main thread reads them for the diagnostics). A burst = callbacks that come
// less than 1.5 ms apart, i.e. back to back.
class CallbackStats {
 public:
  void onCallback(int frames) {
    const int64_t now = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count();
    if (resetReq_.exchange(false, std::memory_order_acquire)) {
      last_ = 0;
      burst_ = 0;
      calls_.store(0, std::memory_order_relaxed);
      frames_.store(0, std::memory_order_relaxed);
      min_.store(1 << 30, std::memory_order_relaxed);
      max_.store(0, std::memory_order_relaxed);
      burstMax_.store(0, std::memory_order_relaxed);
      gapMaxUs_.store(0, std::memory_order_relaxed);
    }
    if (last_ && now - last_ < 1500) burst_ += frames;
    else burst_ = frames;
    if (last_ && now - last_ > gapMaxUs_.load(std::memory_order_relaxed))
      gapMaxUs_.store(int(now - last_), std::memory_order_relaxed);
    last_ = now;
    if (burst_ > burstMax_.load(std::memory_order_relaxed)) burstMax_.store(burst_, std::memory_order_relaxed);
    if (frames < min_.load(std::memory_order_relaxed)) min_.store(frames, std::memory_order_relaxed);
    if (frames > max_.load(std::memory_order_relaxed)) max_.store(frames, std::memory_order_relaxed);
    calls_.fetch_add(1, std::memory_order_relaxed);
    frames_.fetch_add(uint64_t(frames), std::memory_order_relaxed);
  }
  void reset() { resetReq_.store(true, std::memory_order_release); }   // main thread: start over
  struct View {
    uint64_t calls, frames;
    int minFrames, maxFrames, burstMax;
    double gapMaxMs;
  };
  View view() const {
    View v;
    v.calls = calls_.load(std::memory_order_relaxed);
    v.frames = frames_.load(std::memory_order_relaxed);
    v.minFrames = v.calls ? min_.load(std::memory_order_relaxed) : 0;
    v.maxFrames = max_.load(std::memory_order_relaxed);
    v.burstMax = burstMax_.load(std::memory_order_relaxed);
    v.gapMaxMs = gapMaxUs_.load(std::memory_order_relaxed) / 1000.0;
    return v;
  }
  static std::string describe(const View& v) {
    char t[160];
    std::snprintf(t, sizeof(t), "%llu callbacks, %d..%d frames each, up to %d frames back to back, gaps up to %.1f ms",
                  static_cast<unsigned long long>(v.calls), v.minFrames, v.maxFrames, v.burstMax, v.gapMaxMs);
    return t;
  }

 private:
  int64_t last_ = 0;   // callback thread only
  int burst_ = 0;
  std::atomic<bool> resetReq_{true};
  std::atomic<uint64_t> calls_{0}, frames_{0};
  std::atomic<int> min_{1 << 30}, max_{0}, burstMax_{0}, gapMaxUs_{0};
};

// Single-producer (capture callback) / single-consumer (dump writer thread) byte ring.
class ByteRing {
 public:
  static constexpr uint32_t kBytes = 1u << 22;   // 4 MB: ~10 s of f32 stereo 48 kHz, drained every 20 ms
  void write(const uint8_t* p, uint32_t n) {
    const uint64_t w = w_.load(std::memory_order_relaxed);
    const uint64_t r = r_.load(std::memory_order_acquire);
    if (w + n - r > kBytes) {
      dropped_.fetch_add(n, std::memory_order_relaxed);
      return;
    }
    const uint32_t at = uint32_t(w % kBytes);
    const uint32_t first = n < kBytes - at ? n : kBytes - at;
    std::memcpy(buf_ + at, p, first);
    if (first < n) std::memcpy(buf_, p + first, n - first);
    w_.store(w + n, std::memory_order_release);
  }
  // Consumer: the bytes waiting, in up to two pieces.
  uint32_t peek(const uint8_t** a, uint32_t* na, const uint8_t** b, uint32_t* nb) const {
    const uint64_t r = r_.load(std::memory_order_relaxed);
    const uint64_t w = w_.load(std::memory_order_acquire);
    const uint32_t n = uint32_t(w - r);
    const uint32_t at = uint32_t(r % kBytes);
    const uint32_t first = n < kBytes - at ? n : kBytes - at;
    *a = buf_ + at;
    *na = first;
    *b = buf_;
    *nb = n - first;
    return n;
  }
  void consume(uint32_t n) { r_.store(r_.load(std::memory_order_relaxed) + n, std::memory_order_release); }
  uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }

 private:
  uint8_t buf_[kBytes];
  std::atomic<uint64_t> w_{0}, r_{0}, dropped_{0};
};

// What the capture callback calls: raw bytes in the obtained spec in,
// stereo int16 into the DriftBuffer out.
class CaptureSink {
 public:
  void setBuffer(DriftBuffer* buf) { buf_ = buf; }
  void setDump(ByteRing* ring) { dump_.store(ring, std::memory_order_release); }   // nullptr = no dump
  // Before the source delivers its first callback (the device is still paused).
  void begin(const CaptureSpec& s) {
    spec_ = s;
    bpf_ = s.bytesPerFrame();
    carry_ = 0;
    partial_.store(0, std::memory_order_relaxed);
    stats_.reset();
  }
  const CaptureSpec& spec() const { return spec_; }

  // The capture thread: `len` bytes as SDL delivered them.
  void deliver(const uint8_t* p, int len, double now = -1.0) {   // now: see DriftBuffer::push
    if (len <= 0) return;
    stats_.onCallback(len / (bpf_ > 0 ? bpf_ : 1));
    if (ByteRing* d = dump_.load(std::memory_order_acquire)) d->write(p, uint32_t(len));
    if (len % bpf_ || carry_) partial_.fetch_add(1, std::memory_order_relaxed);
    int16_t lr[kChunk * 2];
    int n = 0;
    if (carry_) {   // complete the frame the last callback split
      const int take = bpf_ - carry_ < len ? bpf_ - carry_ : len;
      std::memcpy(carryBuf_ + carry_, p, size_t(take));
      carry_ += take;
      p += take;
      len -= take;
      if (carry_ < bpf_) return;
      frame(carryBuf_, lr);
      n = 1;
      carry_ = 0;
    }
    const int frames = len / bpf_;
    for (int i = 0; i < frames; ++i) {
      frame(p + size_t(i) * size_t(bpf_), lr + 2 * n);
      if (++n == kChunk) {
        buf_->push(lr, uint32_t(n), now);
        n = 0;
      }
    }
    if (n) buf_->push(lr, uint32_t(n), now);
    const int rest = len - frames * bpf_;
    if (rest > 0) {
      std::memcpy(carryBuf_, p + size_t(frames) * size_t(bpf_), size_t(rest));
      carry_ = rest;
    }
  }

  CallbackStats::View callbacks() const { return stats_.view(); }
  uint32_t partialCallbacks() const { return partial_.load(std::memory_order_relaxed); }
  // One line: what is converted to what.
  std::string conversion(int outRate) const {
    std::string s;
    if (spec_.format == kFmtS16LE) s = "s16le as is";
    else s = spec_.formatName() + " -> s16 (saturating)";
    if (spec_.channels == 1) s += ", mono -> both sides";
    else if (spec_.channels == 2) s += ", stereo as is";
    else s += ", " + std::to_string(spec_.channels) + " ch -> the first two";
    char t[96];
    if (spec_.rate == outRate) std::snprintf(t, sizeof(t), ", %d Hz as is (rate match only)", spec_.rate);
    else
      std::snprintf(t, sizeof(t), ", %d -> %d Hz in the rate match (step %.6f)", spec_.rate, outRate,
                    double(spec_.rate) / double(outRate));
    return s + t;
  }

 private:
  static constexpr int kChunk = 256;
  void frame(const uint8_t* f, int16_t* lr) const {
    const int bps = spec_.bytesPerSample();
    lr[0] = sampleToS16(f, spec_);
    lr[1] = spec_.channels >= 2 ? sampleToS16(f + bps, spec_) : lr[0];
  }
  DriftBuffer* buf_ = nullptr;
  std::atomic<ByteRing*> dump_{nullptr};
  CaptureSpec spec_;
  int bpf_ = 4, carry_ = 0;
  uint8_t carryBuf_[64] = {};
  CallbackStats stats_;
  std::atomic<uint32_t> partial_{0};
};

}  // namespace desk
