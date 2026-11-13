#include "listen.h"

#include <SDL.h>

#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>

#include "audio.h"

namespace desk {

// ---- SdlCapture ---------------------------------------------------------------------

namespace {
std::string dbfs(int peak) {
  if (peak <= 0) return "-inf dBFS";
  char t[32];
  std::snprintf(t, sizeof(t), "%.1f dBFS", 20.0 * std::log10(double(peak) / 32768.0));
  return t;
}
}  // namespace

bool SdlCapture::present() {
  if (dev_) return !lost_.load();   // listing while open is not needed (and some backends dislike it)
  const char* want = wanted_.empty() ? kListenDeviceName : wanted_.c_str();
  const int n = SDL_GetNumAudioDevices(1);
  std::vector<std::string> names;
  std::string list;
  for (int i = 0; i < n; ++i) {
    const char* nm = SDL_GetAudioDeviceName(i, 1);
    if (!nm) continue;
    list += std::string(list.empty() ? "" : ", ") + "\"" + nm + "\"";
    names.push_back(nm);
  }
  list_ = list.empty() ? "(none)" : list;
  const int at = findInput(names, want);   // exactly, else the first containing it (input_names.h)
  name_ = at < 0 ? std::string() : names[size_t(at)];
  return !name_.empty();
}

bool SdlCapture::open(const CaptureSpec& want, CaptureSink* sink, CaptureSpec* got, std::string* err) {
  close();
  if (name_.empty() && !present()) {
    *err = "no Pocket Groove audio input";
    return false;
  }
  sink_ = sink;
  lost_.store(false);
  SDL_AudioSpec w, have;
  SDL_zero(w);
  SDL_zero(have);
  w.freq = want.rate;   // the engine's rate
  w.format = SDL_AudioFormat(want.format);
  w.channels = Uint8(want.channels);
  w.samples = Uint16(want.frames);
  w.callback = &SdlCapture::callback;
  w.userdata = this;
  // Any change allowed: whatever SDL gives is read back and converted by
  // CaptureSink (format, channels, callback size) and DriftBuffer (rate), so
  // nothing depends on SDL honouring the request. (sdl2-compat forces
  // allowed_changes to 0 and converts itself; classic SDL2 may hand over the
  // device's own format.)
  dev_ = SDL_OpenAudioDevice(name_.c_str(), 1, &w, &have, SDL_AUDIO_ALLOW_ANY_CHANGE);
  if (!dev_) {
    *err = SDL_GetError();
    return false;
  }
  got->format = uint16_t(have.format);
  got->channels = int(have.channels);
  got->rate = have.freq;
  got->frames = int(have.samples);
  if (!got->supported()) {
    *err = "unsupported capture format: " + got->describe();
    SDL_CloseAudioDevice(dev_);
    dev_ = 0;
    return false;
  }
  sink->begin(*got);
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
  c.sink_->deliver(stream, len);
}

// ---- SimCapture ---------------------------------------------------------------------

bool SimCapture::open(const CaptureSpec& want, CaptureSink* sink, CaptureSpec* got, std::string* err) {
  close();
  if (!plugged_.load()) {
    *err = "simulated device unplugged";
    return false;
  }
  got_ = haveSpec_ ? spec_ : want;
  outRate_ = want.rate;
  *got = got_;
  sink_ = sink;
  sink->begin(got_);
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
  const CaptureSpec s = got_;
  const uint64_t base = out_->framesRendered();
  const double perOut = double(s.rate) / double(outRate_) * (1.0 + ppm_ * 1e-6);
  const int bpf = s.bytesPerFrame(), bps = s.bytesPerSample();
  const int maxFrames = vary_ ? s.frames * 3 / 2 + 1 : s.frames;
  std::vector<uint8_t> blk(size_t(maxFrames) * size_t(bpf));
  uint64_t produced = 0;
  uint32_t rng = 0x12345u;
  auto next = [&rng] {
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return rng;
  };
  const double w = 2.0 * M_PI * 1000.0 / s.rate;
  while (!stop_.load()) {
    std::this_thread::sleep_for(std::chrono::microseconds(1000 + next() % 5000));   // 1..6 ms jitter
    if (!plugged_.load()) continue;   // unplugged: nothing arrives (lost() reports it)
    const double due = double(out_->framesRendered() - base) * perOut;
    while (produced + uint64_t(s.frames) * uint64_t(burst_) <= uint64_t(due) && !stop_.load()) {
      for (int b = 0; b < burst_; ++b) {   // back to back, as an AudioQueue hands over its buffers
        const int n = vary_ ? s.frames / 2 + int(next() % uint32_t(s.frames + 1)) : s.frames;
        for (int i = 0; i < n; ++i) {
          const int16_t v = int16_t(std::lrint(6000.0 * std::sin(w * double(produced + uint64_t(i)))));
          uint8_t* f = blk.data() + size_t(i) * size_t(bpf);
          for (int c = 0; c < s.channels; ++c)
            s16ToSample(c == 0 ? v : c == 1 ? int16_t(-v) : int16_t(0), s, f + c * bps);
        }
        sink_->deliver(blk.data(), n * bpf);
        produced += uint64_t(n);
      }
    }
  }
}

// ---- ListenDump ---------------------------------------------------------------------

namespace {
void le16(uint8_t* p, uint32_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); }
void le32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i)); }

void wavHeader(uint8_t* h, const CaptureSpec& s, uint32_t dataBytes) {
  std::memcpy(h, "RIFF", 4);
  le32(h + 4, 36 + dataBytes);
  std::memcpy(h + 8, "WAVEfmt ", 8);
  le32(h + 16, 16);
  le16(h + 20, s.isFloat() ? 3 : 1);   // IEEE float or PCM
  le16(h + 22, uint32_t(s.channels));
  le32(h + 24, uint32_t(s.rate));
  le32(h + 28, uint32_t(s.rate) * uint32_t(s.bytesPerFrame()));
  le16(h + 32, uint32_t(s.bytesPerFrame()));
  le16(h + 34, uint32_t(s.bits()));
  std::memcpy(h + 36, "data", 4);
  le32(h + 40, dataBytes);
}
}  // namespace

ByteRing* ListenDump::ring() {
  if (!ring_) ring_.reset(new ByteRing());
  return ring_.get();
}

bool ListenDump::start(const std::string& path, const CaptureSpec& spec, std::string* err) {
  stop();
  ring();
  f_ = std::fopen(path.c_str(), "wb");
  if (!f_) {
    *err = "cannot write " + path;
    return false;
  }
  path_ = path;
  spec_ = spec;
  bytes_ = 0;
  uint8_t h[44];
  wavHeader(h, spec_, 0);
  std::fwrite(h, 1, sizeof(h), f_);
  stop_.store(false);
  th_ = std::thread([this] { run(); });
  return true;
}

void ListenDump::drain() {
  const uint8_t *a, *b;
  uint32_t na, nb;
  const uint32_t n = ring_->peek(&a, &na, &b, &nb);
  if (!n) return;
  // WAV is little-endian: a big-endian capture is byte-swapped per sample (the only change).
  const int bps = spec_.bytesPerSample();
  const uint32_t whole = spec_.bigEndian() && bps > 1 ? n - n % uint32_t(bps) : n;
  uint8_t tmp[4096];
  uint32_t done = 0;
  while (done < whole) {
    uint32_t k = whole - done < sizeof(tmp) ? whole - done : uint32_t(sizeof(tmp));
    if (spec_.bigEndian() && bps > 1) k -= k % uint32_t(bps);
    for (uint32_t i = 0; i < k; ++i) {
      const uint32_t at = done + i;
      tmp[i] = at < na ? a[at] : b[at - na];
    }
    if (spec_.bigEndian() && bps > 1)
      for (uint32_t i = 0; i < k; i += uint32_t(bps))
        for (int j = 0; j < bps / 2; ++j) std::swap(tmp[i + uint32_t(j)], tmp[i + uint32_t(bps - 1 - j)]);
    std::fwrite(tmp, 1, k, f_);
    done += k;
  }
  bytes_ += whole;
  ring_->consume(whole);
}

void ListenDump::run() {
  while (!stop_.load()) {
    drain();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
}

void ListenDump::stop() {
  if (!f_) return;
  stop_.store(true);
  if (th_.joinable()) th_.join();
  drain();   // the source is closed: whatever it delivered last
  uint8_t h[44];
  const uint64_t frames = bytes_ / uint64_t(spec_.bytesPerFrame());
  wavHeader(h, spec_, uint32_t(bytes_ > 0xFFFFFF00ull ? 0xFFFFFF00ull : bytes_));
  std::fseek(f_, 0, SEEK_SET);
  std::fwrite(h, 1, sizeof(h), f_);
  std::fclose(f_);
  f_ = nullptr;
  std::printf("[listen] dump: %s, %llu frames (%.1f s) of %s as delivered, %llu bytes dropped\n", path_.c_str(),
              static_cast<unsigned long long>(frames), double(frames) / spec_.rate, spec_.describe().c_str(),
              static_cast<unsigned long long>(ring_->dropped()));
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
  CaptureSpec want;
  want.format = uint16_t(AUDIO_S16SYS);
  want.channels = 2;
  want.rate = rate_;
  want.frames = 256;
  std::string err;
  sink_->setBuffer(buf_);
  sink_->setDump(dumpPath_.empty() ? nullptr : dump_.ring());
  CaptureSpec got;
  if (!src_->open(want, sink_, &got, &err)) {
    openErr_ = err;
    retryAt_ = nowMs + 3000;
    std::printf("[listen] cannot open \"%s\": %s\n", src_->deviceName().c_str(), err.c_str());
    return;
  }
  got_ = got;
  const int outFrames = out_->stats().bufferFrames > 0 ? out_->stats().bufferFrames : 256;
  const int outInFrames = int(std::ceil(double(outFrames) * got.rate / rate_));
  const int base = DriftBuffer::targetFor(outInFrames, got.frames, got.rate);
  buf_->setRates(got.rate, rate_);
  buf_->setTarget(base);
  buf_->restart();
  buf_->takePeak();
  int d, m;
  buf_->takeMixPeaks(&d, &m);
  out_->callbackStats().reset();
  out_->setListen(buf_, true);
  open_ = true;
  unplugged_ = false;
  silent_ = false;
  reportedCallbacks_ = false;
  openErr_.clear();
  lastSignal_ = nowMs;
  openedAt_ = lastReport_ = nowMs;
  ++opens_;
  std::printf("[listen] playing \"%s\" (%d frames per block, target %.1f ms to start with)\n",
              src_->deviceName().c_str(), got.frames, 1000.0 * base / got.rate);
  // The diagnostics block: everything that can differ between machines.
  std::printf("[listen] ---- diagnostics (send this block if Listen sounds wrong) ----\n");
  if (!sdlInfo_.empty()) std::printf("[listen] %s\n", sdlInfo_.c_str());
  std::printf("[listen] capture inputs: %s\n", src_->inputList().c_str());
  std::printf("[listen] capture device: \"%s\"\n", src_->deviceName().c_str());
  std::printf("[listen] capture desired:  %s\n", want.describe().c_str());
  std::printf("[listen] capture obtained: %s%s\n", got.describe().c_str(),
              got == want ? " (= desired; sdl2-compat always echoes the request: SDL3 converts behind it)" : "");
  std::printf("[listen] output desired:   %s\n", out_->desiredSpec().describe().c_str());
  std::printf("[listen] output obtained:  %s\n", out_->obtainedSpec().describe().c_str());
  std::printf("[listen] conversion: %s; mixed into the output saturating (clips counted)\n",
              sink_->conversion(rate_).c_str());
  if (!dumpPath_.empty()) {
    std::string path = dumpPath_;
    if (opens_ > 1) {   // a re-plug: the next file
      const size_t dot = path.rfind('.');
      const std::string suffix = "-" + std::to_string(opens_);
      path = dot == std::string::npos ? path + suffix : path.substr(0, dot) + suffix + path.substr(dot);
    }
    std::string derr;
    if (dump_.start(path, got, &derr))
      std::printf("[listen] dump: writing the raw capture to %s (%s, as delivered)\n", path.c_str(),
                  got.describe().c_str());
    else {
      sink_->setDump(nullptr);   // nothing drains the ring: stop filling it
      std::printf("[listen] dump: %s\n", derr.c_str());
    }
  }
  std::printf("[listen] ---- callback sizes follow in 1.5 s, counters every 5 s ----\n");
}

void Listen::closeNow(const char* why) {
  out_->setListen(buf_, false);   // the output stops reading first
  src_->close();
  sink_->setDump(nullptr);
  dump_.stop();
  open_ = false;
  silent_ = false;
  const DriftBuffer::Stats s = buf_->stats();
  std::printf("[listen] closed (%s): underruns %u, overflows %u, resyncs %u, clipped %llu, clock %+.1f ppm\n", why,
              s.underruns, s.overflows, s.resyncs, static_cast<unsigned long long>(s.clipped), double(s.clockPpm));
}

void Listen::printCallbacks() {
  const DriftBuffer::Stats s = buf_->stats();
  std::printf("[listen] capture callbacks: %s%s\n", CallbackStats::describe(sink_->callbacks()).c_str(),
              sink_->partialCallbacks() ? " (some split a frame)" : "");
  std::printf("[listen] output callbacks:  %s\n", CallbackStats::describe(out_->callbackStats().view()).c_str());
  std::printf("[listen] measured: input arrives up to %d frames at once, the output takes up to %d between "
              "arrivals, %d per output burst; target %d frames = %.1f ms (from the reported sizes: %d)\n",
              s.maxArrival, s.maxRun, s.maxCall, s.target, 1000.0 * s.target / got_.rate, s.baseTarget);
}

void Listen::printStats() {
  // (also after Listen stopped: the counters of the last session)
  const DriftBuffer::Stats s = buf_->stats();
  int dev = 0, mix = 0;
  buf_->takeMixPeaks(&dev, &mix);
  std::printf("[listen] stats: primed %d, fill %.0f fr (%.1f ms), target %d, step %.6f, rate %+.1f ppm, clock %+.1f "
              "ppm, underruns %u, overflows %u, resyncs %u, primes %u, pushed %llu, device peak %s, after mix %s, "
              "clipped %llu\n",
              s.primed ? 1 : 0, double(s.fillFrames), 1000.0 * double(s.fillFrames) / got_.rate, s.target, s.ratio,
              double(s.ppm), double(s.clockPpm), s.underruns, s.overflows, s.resyncs, s.primes,
              static_cast<unsigned long long>(s.pushed), dbfs(dev).c_str(), dbfs(mix).c_str(),
              static_cast<unsigned long long>(s.clipped));
  std::fflush(stdout);
}

std::string Listen::stripStats() const {
  const DriftBuffer::Stats s = buf_->stats();
  char t[64];
  std::snprintf(t, sizeof(t), "U%u O%u R%u CLIP%llu %.0fMS %+.0fPPM", s.underruns, s.overflows, s.resyncs,
                static_cast<unsigned long long>(s.clipped), 1000.0 * double(s.fillFrames) / got_.rate,
                double(s.clockPpm));
  return t;
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
    if (!reportedCallbacks_ && nowMs - openedAt_ >= 1500) {
      reportedCallbacks_ = true;
      printCallbacks();
    }
    if (nowMs - lastReport_ >= 5000) {
      lastReport_ = nowMs;
      printStats();
    }
  }
  std::fflush(stdout);
}

void Listen::shutdown() {
  if (open_) {
    printCallbacks();
    printStats();
    closeNow("quit");
  }
}

std::string Listen::statusLine() const {
  char t[64];
  if (!wanted_) return "LISTEN OFF";
  if (open_) {
    if (silent_) return "SILENT: STOPPED? MIC ALLOWED?";
    const DriftBuffer::Stats s = buf_->stats();
    if (!s.primed) return "LISTEN: STARTING";
    std::snprintf(t, sizeof(t), "LISTENING %.0f MS %+.0f PPM", 1000.0 * double(s.fillFrames) / got_.rate,
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
