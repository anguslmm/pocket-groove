// Listen (Cmd+L, docs/DESKTOP.md): the connected device's USB audio input
// ("Pocket Groove", stereo 16-bit) played through the app's output, on top of
// the app's own engine. The rate match between the two clocks is DriftBuffer
// (listen_buffer.h); this file is where the input comes from and the
// main-thread logic around it (find the device, open, unplug, hints).
//
// CaptureSource is the interface so that the headless test can feed a signal
// with its own clock (SimCapture, --listen-sim) where there is no device.
#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

#include "listen_buffer.h"

namespace desk {

class AudioOut;

constexpr const char* kListenDeviceName = "Pocket Groove";   // the firmware's USB product / audio input name

class CaptureSource {
 public:
  virtual ~CaptureSource() = default;
  // Is the device there? Main thread; re-scans (cheap enough every couple of seconds).
  virtual bool present() = 0;
  // Opens it and starts delivering interleaved stereo int16 at `rate` Hz into sink->push()
  // from its own thread. *blockFrames = the frames per delivery (for the fill target).
  virtual bool open(int rate, DriftBuffer* sink, int* blockFrames, std::string* err) = 0;
  virtual void close() = 0;
  virtual bool lost() = 0;   // it went away while open
  // SDL_AUDIODEVICEADDED / REMOVED (main thread): which = index (added) or device id (removed).
  virtual void deviceEvent(bool added, bool capture, uint32_t which) { (void)added; (void)capture; (void)which; }
  virtual std::string deviceName() const = 0;
};

// The real one: SDL's capture device list, the input whose name is
// "Pocket Groove" (exactly; failing that, the first name containing it).
class SdlCapture final : public CaptureSource {
 public:
  bool present() override;
  bool open(int rate, DriftBuffer* sink, int* blockFrames, std::string* err) override;
  void close() override;
  bool lost() override;
  void deviceEvent(bool added, bool capture, uint32_t which) override;
  std::string deviceName() const override { return name_; }

 private:
  static void callback(void* user, uint8_t* stream, int len);
  std::string name_;
  uint32_t dev_ = 0;
  DriftBuffer* sink_ = nullptr;
  std::atomic<bool> lost_{false};
};

// The test's device (--listen-sim PPM): a 1 kHz tone, left and right in
// opposite phase, clocked by the app's own output clock * (1 + ppm) and
// delivered from a thread in jittered blocks. plug(false) is an unplug.
class SimCapture final : public CaptureSource {
 public:
  SimCapture(const AudioOut* out, double ppm) : out_(out), ppm_(ppm) {}
  ~SimCapture() override { close(); }
  bool present() override { return plugged_.load(); }
  bool open(int rate, DriftBuffer* sink, int* blockFrames, std::string* err) override;
  void close() override;
  bool lost() override { return open_ && !plugged_.load(); }
  std::string deviceName() const override { return "Pocket Groove (simulated)"; }
  void plug(bool on) { plugged_.store(on); }

 private:
  void run();
  const AudioOut* out_;
  double ppm_;
  int rate_ = 44100;
  DriftBuffer* sink_ = nullptr;
  std::thread th_;
  std::atomic<bool> plugged_{true}, stop_{false};
  bool open_ = false;
};

// Main-thread controller: what Cmd+L switches and what the strip shows.
class Listen {
 public:
  void init(CaptureSource* src, DriftBuffer* buf, AudioOut* out, int rate) {
    src_ = src;
    buf_ = buf;
    out_ = out;
    rate_ = rate;
  }
  void toggle(uint32_t nowMs);
  void tick(uint32_t nowMs);          // every main-loop pass
  void rescanSoon() { rescan_ = true; }
  void shutdown();

  bool wanted() const { return wanted_; }
  bool active() const { return open_; }       // the input is open and mixed in
  bool present() const { return present_; }   // a "Pocket Groove" input exists
  bool silent() const { return open_ && silent_; }
  // One line for the strip (<= 29 characters): state, latency or a hint.
  std::string statusLine() const;
  bool hintIsWarning() const;

 private:
  void openNow(uint32_t nowMs);
  void closeNow(const char* why);
  CaptureSource* src_ = nullptr;
  DriftBuffer* buf_ = nullptr;
  AudioOut* out_ = nullptr;
  int rate_ = 44100;
  bool wanted_ = false, open_ = false, present_ = false, silent_ = false, unplugged_ = false;
  bool rescan_ = true;
  std::string openErr_;
  uint32_t lastScan_ = 0, retryAt_ = 0, lastSignal_ = 0;
};

}  // namespace desk
