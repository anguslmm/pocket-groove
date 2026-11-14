// The screen, mirrored to the computer over the USB serial port (stage 0 has no
// display; tools/pg-daisy shows or saves what the UI draws). The framebuffer is the
// UI's own (8-bit indices + RGB565 palette, pg/platform.h), sent as text lines that
// share the port with everything else (tools/pg ignores them: they do not start
// with "~PGX "):
//   ~PGF B <seq> <w> <h> <y0> <y1> <name|->   a frame (or the rows y0..y1-1 of one) begins
//   ~PGF P <base64 of 512 bytes>             the palette, little-endian RGB565
//   ~PGF R <y> <base64 of w bytes>           one row of indices
//   ~PGF E <seq>                             the frame is complete
// A full frame is ~44 KB of text (~0.1 s at USB full speed). pump() sends what fits
// the room the caller has in its transmit buffer, so a frame is spread over main
// loop passes instead of blocking one. Portable (host/daisy/test_daisy_core.cpp).
#pragma once
#include <cstddef>
#include <cstdint>

#include "pg/platform.h"

namespace pgd {

class FbStream {
 public:
  static constexpr size_t kMaxLine = 360;   // a row line incl. newline (the palette line is ~700)
  using Sink = void (*)(void* user, const char* line, size_t len);

  // Starts sending rows [y0, y1) of fb (clamped). A frame already being sent is
  // finished first (its rows stay queued): start() then returns false.
  bool start(const pg::Framebuffer* fb, int y0, int y1, const char* name);
  bool active() const { return fb_ != nullptr; }
  // Sends whole lines while `room` (bytes) allows; returns the bytes sent.
  size_t pump(size_t room, Sink sink, void* user);
  uint32_t framesSent() const { return frames_; }

 private:
  const pg::Framebuffer* fb_ = nullptr;
  int y_ = 0, y1_ = 0, y0_ = 0;
  int phase_ = 0;   // 0 begin, 1 palette, 2 rows, 3 end
  uint32_t seq_ = 0, frames_ = 0;
  char name_[24] = {};
};

}  // namespace pgd
