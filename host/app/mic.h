// The desktop's microphone (stage F2, docs/DESKTOP.md "Recording"): hold L on a
// sampler instrument's page and the Mac's microphone records into the track's slot,
// as on the device. pg::sampler::MicInput for the Ui:
//
//   MicSource      where the sound comes from: SDL's default capture device
//                  (SdlMicSource, mic_sdl.cpp), opened asking for mono int16 at
//                  44,100 Hz (SDL converts whatever the Mac has), or a synthetic one
//                  (the tests); it calls back from its own thread
//   Mic            a lock-free ring between that thread and the main thread; on the
//                  main thread (poll, every UI frame) the same pg::sampler::Recorder
//                  as the device (half-band to 22,050 Hz, DC block, threshold start
//                  with pre-roll, the trim at the end) into RAM; a finished recording
//                  becomes samples/NN-recNN.wav (22,050 Hz mono 16-bit, the slot's
//                  other files removed) and is loaded like any file there (trimmed,
//                  levelled to -1 dBFS: host/common/sample_convert.h)
//
// The capture device is open only while L is held. No SDL in this header.
#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "pg/sampler/recorder.h"

namespace desk {

class Samples;

class MicSource {
 public:
  using Callback = void (*)(const int16_t* mono, int n, void* user);
  virtual ~MicSource() = default;
  // Opens it: cb gets mono int16 at 44,100 Hz from the source's own thread.
  virtual bool open(Callback cb, void* user, std::string* err) = 0;
  virtual void close() = 0;
  virtual std::string name() const = 0;
};

class Mic final : public pg::sampler::MicInput {
 public:
  static constexpr uint32_t kRing = 1u << 17;   // 3 s at 44.1 kHz: the main thread may stall

  void init(Samples* samples, MicSource* src) {
    samples_ = samples;
    src_ = src;
  }

  bool ready(const char** why) override;
  bool slotErased(int) override { return true; }   // files: nothing to erase first
  bool erase(int slot) override;
  bool start(int slot) override;
  void release() override;
  bool poll(pg::sampler::RecView* v) override;

  bool armed() const { return armed_; }
  const std::string& lastFile() const { return lastFile_; }
  const std::string& lastError() const { return err_; }
  uint32_t dropped() const { return dropped_.load(); }

 private:
  static void onAudio(const int16_t* mono, int n, void* user);
  void drain();
  void finishSession();

  struct Sink final : pg::sampler::RecordSink {
    std::vector<int16_t> v;
    bool write(uint32_t offset, const int16_t* d, uint32_t n) override {
      if (offset != v.size()) return false;
      v.insert(v.end(), d, d + n);
      return true;
    }
  };

  Samples* samples_ = nullptr;
  MicSource* src_ = nullptr;
  pg::sampler::Recorder rec_;
  Sink sink_;
  std::vector<int16_t> ring_ = std::vector<int16_t>(kRing);
  std::atomic<uint32_t> head_{0}, tail_{0}, dropped_{0};
  bool armed_ = false;
  int slot_ = 0;
  const char* why_ = "";
  std::string err_, lastFile_;
};

}  // namespace desk
