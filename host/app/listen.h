// Listen (Cmd+L, docs/DESKTOP.md): the connected device's USB audio input
// ("Pocket Groove", stereo 16-bit) played through the app's output, on top of
// the app's own engine. The chain:
//
//   CaptureSource      SDL's capture device (SdlCapture), or SimCapture for
//                      the tests; opened asking for s16 stereo at the engine's
//                      rate, and whatever spec it really got is passed on
//   CaptureSink        (capture_convert.h) the obtained format, channels and
//                      callback size -> stereo int16; the --listen-dump tap
//   DriftBuffer        (listen_buffer.h) the rate match: capture rate and
//                      clock -> the output's, mixed into the output callback
//
// and the main-thread logic around it (find the device, open, unplug, hints)
// plus the diagnostics: one block at Listen start (SDL, devices, desired and
// obtained specs, the conversion chosen), the callback pattern after 1.5 s,
// and the rate match's counters every 5 s. All of it is read from atomics on
// the main thread; the dump file is written by its own thread.
#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "capture_convert.h"
#include "listen_buffer.h"

namespace desk {

class AudioOut;

constexpr const char* kListenDeviceName = "Pocket Groove";   // the firmware's USB product / audio input name

class CaptureSource {
 public:
  virtual ~CaptureSource() = default;
  // Is the device there? Main thread; re-scans (cheap enough every couple of seconds).
  virtual bool present() = 0;
  // Opens it asking for `want`, tells sink->begin() the spec it really got
  // (also in *got), then starts delivering raw callbacks into sink->deliver()
  // from its own thread.
  virtual bool open(const CaptureSpec& want, CaptureSink* sink, CaptureSpec* got, std::string* err) = 0;
  virtual void close() = 0;
  virtual bool lost() = 0;   // it went away while open
  // SDL_AUDIODEVICEADDED / REMOVED (main thread): which = index (added) or device id (removed).
  virtual void deviceEvent(bool added, bool capture, uint32_t which) { (void)added; (void)capture; (void)which; }
  virtual std::string deviceName() const = 0;
  virtual std::string inputList() const { return ""; }   // every input's name, for the diagnostics
};

// The real one: SDL's capture device list, the input whose name is
// "Pocket Groove" (exactly; failing that, the first name containing it), or
// the one --listen-device names.
class SdlCapture final : public CaptureSource {
 public:
  void setWanted(const std::string& name) { wanted_ = name; }   // "" = "Pocket Groove"
  bool present() override;
  bool open(const CaptureSpec& want, CaptureSink* sink, CaptureSpec* got, std::string* err) override;
  void close() override;
  bool lost() override;
  void deviceEvent(bool added, bool capture, uint32_t which) override;
  std::string deviceName() const override { return name_; }
  std::string inputList() const override { return list_; }

 private:
  static void callback(void* user, uint8_t* stream, int len);
  std::string wanted_, name_, list_;
  uint32_t dev_ = 0;
  CaptureSink* sink_ = nullptr;
  std::atomic<bool> lost_{false};
};

// The test's device (--listen-sim PPM): a 1 kHz tone at -14.7 dBFS (6000),
// left and right in opposite phase (mono: the tone), clocked by the app's own
// output clock * (1 + ppm) and delivered from a thread. By default it
// delivers what was asked (s16 stereo, 256-frame callbacks); `spec` makes it
// deliver another obtained spec instead (format, channels, rate, callback
// frames: --listen-sim-spec), `burst` callbacks back to back
// (--listen-sim-burst, as CoreAudio's AudioQueue does) and, with `vary`,
// callback sizes anywhere in frames/2 .. 3*frames/2. plug(false) is an unplug.
class SimCapture final : public CaptureSource {
 public:
  SimCapture(const AudioOut* out, double ppm) : out_(out), ppm_(ppm) {}
  ~SimCapture() override { close(); }
  void setSpec(const CaptureSpec& s, int burst, bool vary) {
    spec_ = s;
    haveSpec_ = true;
    burst_ = burst < 1 ? 1 : burst;
    vary_ = vary;
  }
  bool present() override { return plugged_.load(); }
  bool open(const CaptureSpec& want, CaptureSink* sink, CaptureSpec* got, std::string* err) override;
  void close() override;
  bool lost() override { return open_ && !plugged_.load(); }
  std::string deviceName() const override { return "Pocket Groove (simulated)"; }
  std::string inputList() const override { return "\"Pocket Groove (simulated)\""; }
  void plug(bool on) { plugged_.store(on); }

 private:
  void run();
  const AudioOut* out_;
  double ppm_;
  CaptureSpec spec_, got_;
  bool haveSpec_ = false, vary_ = false;
  int burst_ = 1, outRate_ = 44100;
  CaptureSink* sink_ = nullptr;
  std::thread th_;
  std::atomic<bool> plugged_{true}, stop_{false};
  bool open_ = false;
};

// --listen-dump FILE.wav: the raw capture exactly as SDL delivered it (before
// any conversion), with the obtained spec in the WAV header. The capture
// callback writes into a ByteRing; a thread here drains it to the file.
class ListenDump {
 public:
  ~ListenDump() { stop(); }
  ByteRing* ring();                                   // created on first use (main thread)
  bool start(const std::string& path, const CaptureSpec& spec, std::string* err);
  void stop();                                        // after the source is closed: drain, patch the header
  bool active() const { return f_ != nullptr; }

 private:
  void run();
  void drain();
  std::unique_ptr<ByteRing> ring_;
  FILE* f_ = nullptr;
  std::string path_;
  CaptureSpec spec_;
  uint64_t bytes_ = 0;
  std::thread th_;
  std::atomic<bool> stop_{false};
};

// Main-thread controller: what Cmd+L switches and what the strip shows.
class Listen {
 public:
  void init(CaptureSource* src, DriftBuffer* buf, CaptureSink* sink, AudioOut* out, int rate) {
    src_ = src;
    buf_ = buf;
    sink_ = sink;
    out_ = out;
    rate_ = rate;
  }
  // The diagnostics block's first line (SDL versions, sdl2-compat or not, driver).
  void setSdlInfo(const std::string& s) { sdlInfo_ = s; }
  void setDumpPath(const std::string& p) { dumpPath_ = p; }
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
  // The rate match and level counters (terminal), and a short form for the strip's STATS.
  void printStats();
  std::string stripStats() const;

 private:
  void openNow(uint32_t nowMs);
  void closeNow(const char* why);
  void printCallbacks();
  CaptureSource* src_ = nullptr;
  DriftBuffer* buf_ = nullptr;
  CaptureSink* sink_ = nullptr;
  AudioOut* out_ = nullptr;
  int rate_ = 44100;
  bool wanted_ = false, open_ = false, present_ = false, silent_ = false, unplugged_ = false;
  bool rescan_ = true, reportedCallbacks_ = false;
  std::string openErr_, sdlInfo_, dumpPath_;
  int opens_ = 0;
  CaptureSpec got_;
  ListenDump dump_;
  uint32_t lastScan_ = 0, retryAt_ = 0, lastSignal_ = 0, openedAt_ = 0, lastReport_ = 0;
};

}  // namespace desk
