#include "listen.h"

#include <SDL.h>

#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "audio.h"

namespace desk {

// ---- SdlCapture ---------------------------------------------------------------------

namespace {
bool containsNoCase(const char* hay, const char* needle) {
  const size_t n = std::strlen(needle);
  for (; *hay; ++hay) {
    size_t i = 0;
    while (i < n && hay[i] && std::tolower(static_cast<unsigned char>(hay[i])) ==
                                  std::tolower(static_cast<unsigned char>(needle[i])))
      ++i;
    if (i == n) return true;
  }
  return false;
}
}  // namespace

bool SdlCapture::present() {
  if (dev_) return !lost_.load();   // listing while open is not needed (and some backends dislike it)
  const int n = SDL_GetNumAudioDevices(1);
  std::string exact, partial;
  for (int i = 0; i < n; ++i) {
    const char* nm = SDL_GetAudioDeviceName(i, 1);
    if (!nm) continue;
    if (!std::strcmp(nm, kListenDeviceName)) exact = nm;
    else if (partial.empty() && containsNoCase(nm, kListenDeviceName)) partial = nm;
  }
  name_ = !exact.empty() ? exact : partial;
  return !name_.empty();
}

bool SdlCapture::open(int rate, DriftBuffer* sink, int* blockFrames, std::string* err) {
  close();
  if (name_.empty() && !present()) {
    *err = "no Pocket Groove audio input";
    return false;
  }
  sink_ = sink;
  lost_.store(false);
  SDL_AudioSpec want, have;
  SDL_zero(want);
  want.freq = rate;             // the engine's rate: SDL converts if the device runs at 32 kHz
  want.format = AUDIO_S16SYS;
  want.channels = 2;
  want.samples = 256;
  want.callback = &SdlCapture::callback;
  want.userdata = this;
  dev_ = SDL_OpenAudioDevice(name_.c_str(), 1, &want, &have, 0);
  if (!dev_) {
    *err = SDL_GetError();
    return false;
  }
  *blockFrames = have.samples ? int(have.samples) : 256;
  SDL_PauseAudioDevice(dev_, 0);
  return true;
}

void SdlCapture::close() {
  if (dev_) SDL_CloseAudioDevice(dev_);   // waits for a running callback
  dev_ = 0;
}

bool SdlCapture::lost() {
  if (!dev_) return false;
  if (SDL_GetAudioDeviceStatus(dev_) == SDL_AUDIO_STOPPED) lost_.store(true);
  return lost_.load();
}

void SdlCapture::deviceEvent(bool added, bool capture, uint32_t which) {
  if (!capture) return;
  if (!added && dev_ && which == dev_) lost_.store(true);
}

void SdlCapture::callback(void* user, uint8_t* stream, int len) {
  SdlCapture& c = *static_cast<SdlCapture*>(user);
  c.sink_->push(reinterpret_cast<const int16_t*>(stream), uint32_t(len / 4));
}

// ---- SimCapture ---------------------------------------------------------------------

bool SimCapture::open(int rate, DriftBuffer* sink, int* blockFrames, std::string* err) {
  close();
  if (!plugged_.load()) {
    *err = "simulated device unplugged";
    return false;
  }
  rate_ = rate;
  sink_ = sink;
  *blockFrames = 256;
  stop_.store(false);
  open_ = true;
  th_ = std::thread([this] { run(); });
  return true;
}

void SimCapture::close() {
  if (!open_) return;
  stop_.store(true);
  if (th_.joinable()) th_.join();
  open_ = false;
}

void SimCapture::run() {
  // Its clock: the app's output clock (frames rendered) times 1 + ppm, so the
  // drift is exact even under SDL's dummy driver, which runs at no fixed speed.
  const uint64_t base = out_->framesRendered();
  uint64_t produced = 0;
  uint32_t rng = 0x12345u;
  int16_t block[1024 * 2];
  const double w = 2.0 * M_PI * 1000.0 / rate_;
  while (!stop_.load()) {
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    std::this_thread::sleep_for(std::chrono::microseconds(1000 + rng % 5000));   // 1..6 ms jitter
    if (!plugged_.load()) continue;   // unplugged: nothing arrives (lost() reports it)
    const double due = double(out_->framesRendered() - base) * (1.0 + ppm_ * 1e-6);
    while (produced + 1 <= uint64_t(due) && !stop_.load()) {
      uint64_t n = uint64_t(due) - produced;
      if (n > 1024) n = 1024;
      for (uint64_t i = 0; i < n; ++i) {
        const int16_t v = int16_t(std::lrint(6000.0 * std::sin(w * double(produced + i))));
        block[2 * i] = v;
        block[2 * i + 1] = int16_t(-v);
      }
      sink_->push(block, uint32_t(n));
      produced += n;
    }
  }
}

// ---- Listen -------------------------------------------------------------------------

void Listen::toggle(uint32_t nowMs) {
  wanted_ = !wanted_;
  std::printf("[listen] %s\n", wanted_ ? "on" : "off");
  if (!wanted_ && open_) closeNow("switched off");
  unplugged_ = false;
  openErr_.clear();
  rescan_ = true;
  retryAt_ = nowMs;
  tick(nowMs);
  std::fflush(stdout);
}

void Listen::openNow(uint32_t nowMs) {
  int block = 256;
  std::string err;
  if (!src_->open(rate_, buf_, &block, &err)) {
    openErr_ = err;
    retryAt_ = nowMs + 3000;
    std::printf("[listen] cannot open \"%s\": %s\n", src_->deviceName().c_str(), err.c_str());
    return;
  }
  const int outFrames = out_->stats().bufferFrames;
  buf_->setTarget(DriftBuffer::targetFor(outFrames > 0 ? outFrames : 256, block, rate_));
  buf_->restart();
  buf_->takePeak();
  out_->setListen(buf_, true);
  open_ = true;
  unplugged_ = false;
  silent_ = false;
  openErr_.clear();
  lastSignal_ = nowMs;
  std::printf("[listen] playing \"%s\" (%d frames per block, target %.1f ms)\n", src_->deviceName().c_str(), block,
              1000.0 * DriftBuffer::targetFor(outFrames > 0 ? outFrames : 256, block, rate_) / rate_);
}

void Listen::closeNow(const char* why) {
  out_->setListen(buf_, false);   // the output stops reading first
  src_->close();
  open_ = false;
  silent_ = false;
  const DriftBuffer::Stats s = buf_->stats();
  std::printf("[listen] closed (%s): underruns %u, overflows %u, resyncs %u, clock %+.1f ppm\n", why, s.underruns,
              s.overflows, s.resyncs, double(s.clockPpm));
}

void Listen::tick(uint32_t nowMs) {
  if (!src_) return;
  if (rescan_ || nowMs - lastScan_ >= 2000) {
    rescan_ = false;
    lastScan_ = nowMs;
    const bool was = present_;
    present_ = src_->present();
    if (present_ != was) std::printf("[listen] device %s\n", present_ ? "connected" : "not connected");
  }
  if (open_ && (src_->lost() || !wanted_)) {
    if (wanted_) unplugged_ = true;
    closeNow(wanted_ ? "device gone" : "switched off");
    present_ = src_->present();
    lastScan_ = nowMs;
  }
  if (wanted_ && !open_ && present_ && int32_t(nowMs - retryAt_) >= 0) openNow(nowMs);
  if (open_) {
    if (buf_->takePeak() > 0) lastSignal_ = nowMs;
    const bool s = nowMs - lastSignal_ > 3000;
    if (s != silent_) std::printf("[listen] %s\n", s ? "only silence arrives" : "signal");
    silent_ = s;
  }
  std::fflush(stdout);
}

void Listen::shutdown() {
  if (open_) closeNow("quit");
}

std::string Listen::statusLine() const {
  char t[64];
  if (!wanted_) return present_ ? "LISTEN OFF" : "LISTEN OFF";
  if (open_) {
    if (silent_) return "SILENT: STOPPED? MIC ALLOWED?";
    const DriftBuffer::Stats s = buf_->stats();
    if (!s.primed) return "LISTEN: STARTING";
    std::snprintf(t, sizeof(t), "LISTENING %.0f MS %+.0f PPM", 1000.0 * double(s.fillFrames) / rate_,
                  double(s.clockPpm));
    return t;
  }
  if (!openErr_.empty()) return "WON'T OPEN: MIC PERMISSION?";
  if (unplugged_) return "UNPLUGGED: WAITING";
  return "NO POCKET GROOVE INPUT";
}

bool Listen::hintIsWarning() const {
  return wanted_ && (!open_ || silent_);
}

}  // namespace desk
