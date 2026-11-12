#include "recorder.h"

#include <cstring>
#include <ctime>

#include "dirfs.h"

namespace desk {

namespace {
void le16(uint8_t* p, uint32_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); }
void le32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i)); }

void header(uint8_t* h, int rate, uint32_t dataBytes) {
  std::memcpy(h, "RIFF", 4);
  le32(h + 4, 36 + dataBytes);
  std::memcpy(h + 8, "WAVEfmt ", 8);
  le32(h + 16, 16);
  le16(h + 20, 1);              // PCM
  le16(h + 22, 2);              // stereo
  le32(h + 24, uint32_t(rate));
  le32(h + 28, uint32_t(rate) * 4);
  le16(h + 32, 4);
  le16(h + 34, 16);
  std::memcpy(h + 36, "data", 4);
  le32(h + 40, dataBytes);
}
}  // namespace

bool Recorder::start(char* err, int errCap) {
  if (f_) return true;
  if (!makeDirs(dir_)) {
    std::snprintf(err, size_t(errCap), "cannot create %s", dir_.c_str());
    return false;
  }
  const std::time_t t = std::time(nullptr);
  std::tm tm{};
  localtime_r(&t, &tm);
  char name[64];
  std::strftime(name, sizeof(name), "%Y%m%d-%H%M%S", &tm);
  path_ = dir_ + "/" + name + ".wav";
  for (int i = 2; i < 100; ++i) {   // two recordings in the same second
    FILE* probe = std::fopen(path_.c_str(), "rb");
    if (!probe) break;
    std::fclose(probe);
    path_ = dir_ + "/" + name + "-" + std::to_string(i) + ".wav";
  }
  f_ = std::fopen(path_.c_str(), "wb");
  if (!f_) {
    std::snprintf(err, size_t(errCap), "cannot write %s", path_.c_str());
    return false;
  }
  uint8_t h[44];
  header(h, rate_, 0);
  std::fwrite(h, 1, sizeof(h), f_);
  frames_ = 0;
  writeErr_ = false;
  startFrame_ = ring_->readPos();   // everything from here on is in the file
  dropped0_ = ring_->dropped();
  return true;
}

void Recorder::drain() {
  if (!ring_) return;
  for (;;) {
    const int16_t *a, *b;
    uint32_t na, nb;
    const uint32_t n = ring_->peek(&a, &na, &b, &nb, 1u << 16);
    if (!n) return;
    if (f_) {
      if (std::fwrite(a, 4, na, f_) != na || (nb && std::fwrite(b, 4, nb, f_) != nb)) writeErr_ = true;
      frames_ += n;
    }
    ring_->consume(n);
  }
}

void Recorder::stop() {
  if (!f_) return;
  drain();   // whatever the callback has rendered up to now
  FILE* f = f_;
  f_ = nullptr;
  uint8_t h[44];
  header(h, rate_, uint32_t(frames_ * 4));
  std::fseek(f, 0, SEEK_SET);
  std::fwrite(h, 1, sizeof(h), f);
  if (std::fclose(f) != 0) writeErr_ = true;
  if (writeErr_) std::fprintf(stderr, "[rec] write error in %s\n", path_.c_str());
}

}  // namespace desk
