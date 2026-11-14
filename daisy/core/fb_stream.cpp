#include "fb_stream.h"

#include <cstdio>
#include <cstring>

#include "pg/io/xfer.h"

namespace pgd {

bool FbStream::start(const pg::Framebuffer* fb, int y0, int y1, const char* name) {
  if (fb_ || !fb) return false;
  if (y0 < 0) y0 = 0;
  if (y1 > pg::kScreenH) y1 = pg::kScreenH;
  if (y0 >= y1) return false;
  fb_ = fb;
  y0_ = y_ = y0;
  y1_ = y1;
  phase_ = 0;
  ++seq_;
  std::snprintf(name_, sizeof(name_), "%s", name && *name ? name : "-");
  for (char* p = name_; *p; ++p)
    if (*p == ' ') *p = '_';
  return true;
}

size_t FbStream::pump(size_t room, Sink sink, void* user) {
  size_t sent = 0;
  char line[kMaxLine + 8];
  while (fb_) {
    int n = 0;
    if (phase_ == 0) {
      n = std::snprintf(line, sizeof(line), "~PGF B %lu %d %d %d %d %s\n", static_cast<unsigned long>(seq_),
                        pg::kScreenW, pg::kScreenH, y0_, y1_, name_);
    } else if (phase_ == 1) {
      uint8_t pal[512];
      for (int i = 0; i < 256; ++i) {
        pal[2 * i] = static_cast<uint8_t>(fb_->palette[i]);
        pal[2 * i + 1] = static_cast<uint8_t>(fb_->palette[i] >> 8);
      }
      char b64[700];
      pg::io::base64Encode(pal, sizeof(pal), b64, sizeof(b64));
      // The palette line is longer than a row: it goes out alone when there is room.
      const int len = std::snprintf(nullptr, 0, "~PGF P %s\n", b64);
      if (len <= 0 || sent + static_cast<size_t>(len) > room) break;
      char big[720];
      n = std::snprintf(big, sizeof(big), "~PGF P %s\n", b64);
      sink(user, big, static_cast<size_t>(n));
      sent += static_cast<size_t>(n);
      phase_ = 2;
      continue;
    } else if (phase_ == 2) {
      if (y_ >= y1_) {
        phase_ = 3;
        continue;
      }
      char b64[(pg::kScreenW + 2) / 3 * 4 + 4];
      pg::io::base64Encode(fb_->px + y_ * pg::kScreenW, pg::kScreenW, b64, sizeof(b64));
      n = std::snprintf(line, sizeof(line), "~PGF R %d %s\n", y_, b64);
    } else {
      n = std::snprintf(line, sizeof(line), "~PGF E %lu\n", static_cast<unsigned long>(seq_));
    }
    if (n <= 0 || sent + static_cast<size_t>(n) > room) break;
    sink(user, line, static_cast<size_t>(n));
    sent += static_cast<size_t>(n);
    if (phase_ == 0) phase_ = 1;
    else if (phase_ == 2) ++y_;
    else if (phase_ == 3) {
      fb_ = nullptr;
      ++frames_;
    }
  }
  return sent;
}

}  // namespace pgd
