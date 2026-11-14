// Single-producer single-consumer byte ring over caller-owned memory (no heap).
// The Daisy's USB receive callback (an interrupt) pushes, the main loop pops; the
// main loop pushes serial output, the USB flush pops. Capacity must be a power of
// two. Indices run freely and wrap; the difference is the fill level.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace pgd {

class ByteRing {
 public:
  ByteRing() = default;
  ByteRing(uint8_t* mem, uint32_t capacity) { attach(mem, capacity); }
  // capacity: a power of two (else it is rounded down to one).
  void attach(uint8_t* mem, uint32_t capacity) {
    uint32_t c = 1;
    while (c * 2 <= capacity && c < 0x80000000u) c *= 2;
    buf_ = mem;
    cap_ = mem ? c : 0;
    head_.store(0, std::memory_order_relaxed);
    tail_.store(0, std::memory_order_relaxed);
  }
  uint32_t capacity() const { return cap_; }
  uint32_t size() const { return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire); }
  uint32_t space() const { return cap_ - size(); }
  bool empty() const { return size() == 0; }

  // Producer. All or nothing: false (nothing written) if it does not fit.
  bool push(const uint8_t* p, uint32_t n) {
    const uint32_t h = head_.load(std::memory_order_relaxed);
    const uint32_t t = tail_.load(std::memory_order_acquire);
    if (n > cap_ - (h - t)) return false;
    for (uint32_t i = 0; i < n; ++i) buf_[(h + i) & (cap_ - 1)] = p[i];
    head_.store(h + n, std::memory_order_release);
    return true;
  }
  bool push(uint8_t b) { return push(&b, 1); }
  // Producer: as much as fits. Returns the count written.
  uint32_t pushSome(const uint8_t* p, uint32_t n) {
    const uint32_t s = space();
    const uint32_t k = n < s ? n : s;
    push(p, k);
    return k;
  }

  // Consumer: up to cap bytes. Returns the count.
  uint32_t pop(uint8_t* out, uint32_t cap) {
    const uint32_t t = tail_.load(std::memory_order_relaxed);
    const uint32_t h = head_.load(std::memory_order_acquire);
    uint32_t n = h - t;
    if (n > cap) n = cap;
    for (uint32_t i = 0; i < n; ++i) out[i] = buf_[(t + i) & (cap_ - 1)];
    tail_.store(t + n, std::memory_order_release);
    return n;
  }
  int popByte() {
    uint8_t b;
    return pop(&b, 1) ? b : -1;
  }

 private:
  uint8_t* buf_ = nullptr;
  uint32_t cap_ = 0;
  std::atomic<uint32_t> head_{0}, tail_{0};
};

}  // namespace pgd
