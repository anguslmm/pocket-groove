// Live recording (F9): drains the audio callback's FrameRing on the main
// thread and writes 16-bit stereo WAV, so the audio thread never touches a
// file. While not recording, drain() just skips what the callback rendered.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>

#include "audio.h"

namespace desk {

class Recorder {
 public:
  // dir = where timestamped files go (created on first use).
  void init(FrameRing* ring, int sampleRate, std::string dir) {
    ring_ = ring;
    rate_ = sampleRate;
    dir_ = std::move(dir);
  }
  bool start(char* err, int errCap);   // opens <dir>/YYYYMMDD-HHMMSS.wav
  void stop();                          // patches the header, closes
  bool active() const { return f_ != nullptr; }
  void drain();                         // call every main-loop iteration

  const std::string& path() const { return path_; }
  uint64_t startFrame() const { return startFrame_; }   // absolute engine frame of the WAV's first frame
  uint64_t frames() const { return frames_; }
  uint64_t droppedAtStart() const { return dropped0_; }
  double seconds() const { return double(frames_) / double(rate_); }
  bool full() const { return frames_ > (0xFFFFFFFFull - 1024 * 1024) / 4; }   // WAV size limit (~6.7 h)

 private:
  FrameRing* ring_ = nullptr;
  int rate_ = 44100;
  std::string dir_, path_;
  FILE* f_ = nullptr;
  uint64_t frames_ = 0, startFrame_ = 0, dropped0_ = 0;
  bool writeErr_ = false;
};

}  // namespace desk
