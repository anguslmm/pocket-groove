// The desktop's microphone (stage F2, docs/DESKTOP.md "Recording"): hold L on a
// sampler instrument's page and the Mac's microphone records into the track's slot,
// as on the device. pg::sampler::MicInput for the Ui:
//
//   MicSource      where the sound comes from: an SDL capture device chosen as
//                  below (SdlMicSource, mic_sdl.cpp), opened asking for mono int16 at
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
//
// Which input (chooseMicInput, at every hold of L): --mic-device NAME (exact, or
// part of a name, any case) when given; otherwise never the connected device's own
// stream ("Pocket Groove", the input Listen plays: digital silence unless the device
// plays), the system's default input when SDL can name it and it is not that one,
// else the first input whose name says "microphone" or "built-in", else the first
// other input.
//
// A dead input: while WAITING, a second of samples that are all exactly 0 (the
// device's silent stream, or macOS withholding the microphone from the app the
// desktop was started from) prints what to do and shows MIC SILENT once per hold.
#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "pg/sampler/recorder.h"

namespace desk {

class Samples;

struct MicChoice {
  bool ok = false;     // false: no usable input (why says so)
  std::string open;    // the name to open; "" = the system's default input
  std::string name;    // the input's name, for the messages
  std::string why;     // how it was chosen, or why there is none
};
// inputs: the capture devices' names; defaultName: the system's default input,
// nullptr when SDL cannot tell; wanted: --mic-device ("" = choose).
MicChoice chooseMicInput(const std::vector<std::string>& inputs, const std::string* defaultName,
                         const std::string& wanted);
// "[mic] inputs: "A", "B"; using "B" (why)" (no newline)
std::string micInputsLine(const std::vector<std::string>& inputs, const MicChoice& c);

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
  static constexpr uint32_t kSilentSamples = 44100;   // 1 s of exact zeros while WAITING: a dead input

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
  // The screen text for a dead input ("MIC SILENT: NAME"), once per hold; "" otherwise.
  // The caller shows it as a toast.
  std::string takeSilentNotice() {
    std::string s;
    s.swap(silentNotice_);
    return s;
  }
  int silentNotices() const { return silentCount_; }   // since start (the tests)

 private:
  static void onAudio(const int16_t* mono, int n, void* user);
  void drain();
  void finishSession();
  void checkSilent();

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
  uint32_t zeroRun_ = 0;      // samples since the last non-zero one (this hold)
  uint32_t heard_ = 0;        // samples the input gave (this hold)
  int inPeak_ = 0;            // their peak
  bool silentShown_ = false;  // this hold
  int silentCount_ = 0;
  std::string silentNotice_;
};

}  // namespace desk
