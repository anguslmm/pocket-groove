// test_desktop_listen: Listen's capture path without SDL: CaptureSink
// (host/app/capture_convert.h: whatever SDL delivered -> stereo int16) and
// DriftBuffer (host/app/listen_buffer.h: the rate match between the device's
// USB audio clock and the computer's output clock), against simulated clocks.
//
// Discrete-event simulation: the capture side delivers `capBurst` callbacks
// of capFrames back to back every capBurst * capFrames / (capRate * (1+ppm))
// seconds, in the obtained spec under test (format, channels, rate); the
// output side asks for outFrames, outBurst callbacks back to back, or models
// SDL3 behind sdl2-compat: a device at outDevRate taking outDevFrames per
// cycle out of a resampling stream that our 256-frame callback refills
// (0..n calls per cycle). Each event arrives late by a random jitter
// (uniform 0..J, never reordering a stream's own events). The device sends a
// 997 Hz sine at -12 dBFS.
//
// Checked: no underrun, overflow or resync after the start, the fill (=
// added latency) bounded, the correction settles on the clock difference, no
// glitch (a step larger than the sine's steepest slope); and on a stretch of
// the output, fitted: the frequency (the device's, in its own clock), the
// level, and THD+N (everything that is not the tone) below -60 dB.
//
// The "Mac" cases are the delivery patterns of CoreAudio's AudioQueue behind
// sdl2-compat (several capture callbacks back to back, output resampled to
// 48 kHz in 1024-frame device cycles): the rate match of 2026-11-12 sized its
// buffer from the callback sizes SDL reported (256 + 256 frames, which
// sdl2-compat only echoes) and ran dry about 43 times a second on them.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>

#include "capture_convert.h"
#include "listen_buffer.h"
#include "sine_fit.h"

using desk::CaptureSink;
using desk::CaptureSpec;
using desk::DriftBuffer;

namespace {

int g_fail = 0;
const bool g_trace = std::getenv("PG_LISTEN_TRACE") != nullptr;
#define CHECK(c, ...)                                   \
  do {                                                  \
    if (!(c)) {                                         \
      std::printf("  FAIL %s:%d: ", __FILE__, __LINE__); \
      std::printf(__VA_ARGS__);                         \
      std::printf("\n");                                \
      ++g_fail;                                         \
    }                                                   \
  } while (0)

struct Rng {
  uint64_t s;
  double unit() {
    s ^= s << 13; s ^= s >> 7; s ^= s << 17;
    return double(s >> 11) / double(1ull << 53);
  }
};

struct Cfg {
  const char* name;
  double ppm = 0.0;
  // capture: the obtained spec
  uint16_t format = desk::kFmtS16LE;
  int channels = 2;
  int capRate = 0;              // 0 = the output rate
  int capFrames = 256;
  bool capVariable = false;     // blocks of capFrames/2 .. 3*capFrames/2
  int capBurst = 1;             // callbacks back to back
  double burstChangeAt = -1.0;  // from then on capBurst2 callbacks back to back (the pattern grows)
  int capBurst2 = 1;
  bool splitBytes = false;      // callbacks cut at arbitrary byte counts (frames split across callbacks)
  // output
  int outFrames = 256;
  int outBurst = 1;
  int outDevRate = 0, outDevFrames = 0;   // SDL3 behind sdl2-compat: a device cycle that resamples
  double jitterMs = 3.0;        // per event, both sides
  double seconds = 3600.0;
  double gapAt = -1.0, gapLen = 0.0;          // capture stops (unplug) and resumes
  double stallAt = -1.0, stallLen = 0.0;      // output stops calling back
  int rate = 44100;
  double analyzeAt = -1.0;      // fit the tone on 0.5 s of output from here
};

struct Result {
  uint32_t underruns, overflows, resyncs, primes;
  int minFill, maxFill;          // raw fill before each output call, after the first minute (outside gaps/stalls)
  double meanFill;
  double finalPpm, clockPpm;
  long glitches;
  double maxSmoothedErr;         // |smoothed fill - target| after the first minute
  int target;                    // the target in force at the end (measured)
  uint64_t clipped;
  sinefit::Fit fit;
  bool fitted;
};

DriftBuffer g_buf;   // 128 KB: static
CaptureSink g_sink;

Result run(const Cfg& c) {
  new (&g_buf) DriftBuffer();
  new (&g_sink) CaptureSink();
  CaptureSpec spec;
  spec.format = c.format;
  spec.channels = c.channels;
  spec.rate = c.capRate ? c.capRate : c.rate;
  spec.frames = c.capFrames;
  g_sink.setBuffer(&g_buf);
  g_sink.begin(spec);
  const int outIn = int(std::ceil(double(c.outFrames) * spec.rate / c.rate));
  g_buf.setRates(spec.rate, c.rate);
  g_buf.setTarget(DriftBuffer::targetFor(outIn, c.capFrames, spec.rate));
  g_buf.restart();
  Rng rng{0x9E3779B97F4A7C15ull ^ uint64_t(c.ppm * 1000 + c.capFrames + spec.rate)};
  const double inRate = spec.rate * (1.0 + c.ppm * 1e-6);
  const double J = c.jitterMs * 1e-3;
  const int bpf = spec.bytesPerFrame(), bps = spec.bytesPerSample();
  // the capture side
  uint64_t produced = 0;
  std::vector<int> chunks;
  double tNow = 0.0;
  auto plan = [&] {
    chunks.clear();
    const int burst = c.burstChangeAt >= 0 && tNow >= c.burstChangeAt ? c.capBurst2 : c.capBurst;
    for (int b = 0; b < burst; ++b)
      chunks.push_back(c.capVariable ? c.capFrames / 2 + int(rng.unit() * c.capFrames) : c.capFrames);
  };
  plan();
  auto capDue = [&](double jit) {
    uint64_t n = 0;
    for (int k : chunks) n += uint64_t(k);
    return double(produced + n) / inRate + jit;
  };
  double tCap = capDue(rng.unit() * J);
  std::vector<uint8_t> bytes(size_t(c.capFrames) * 2 * size_t(bpf) + 64);
  // the output side
  uint64_t outEvents = 0;
  const bool devModel = c.outDevRate > 0;
  const double outPeriod = devModel ? double(c.outDevFrames) / c.outDevRate
                                    : double(c.outFrames) * c.outBurst / c.rate;
  double streamAvail = 0.0;   // devModel: our frames waiting in SDL3's stream
  double tOut = outPeriod + rng.unit() * J;
  std::vector<int16_t> out(size_t(c.outFrames) * 2);
  const double w = 2.0 * M_PI * 997.0 / spec.rate;   // the device's sine, in its own samples
  const double amp = 8192.0;
  const double maxStep = amp * 2.0 * M_PI * 997.0 / c.rate * 1.05 + 4.0;   // steepest slope + overshoot + rounding
  Result res{};
  res.minFill = 1 << 30;
  res.maxFill = 0;
  int16_t prevL = 0;
  bool havePrev = false;
  uint32_t primesSeen = 0;
  long sinceFade = 0;
  double fillSum = 0.0;
  long fillN = 0;
  double nextTrace = 0.0;
  std::vector<float> rec;
  const size_t recWant = size_t(c.rate / 2);
  size_t splitCarry = 0;   // splitBytes: bytes held back to the next callback
  std::vector<uint8_t> pending;
  for (;;) {
    const double t = tCap < tOut ? tCap : tOut;
    if (t > c.seconds) break;
    tNow = t;
    const bool inGap = c.gapAt >= 0 && t >= c.gapAt && t < c.gapAt + c.gapLen;
    const bool inStall = c.stallAt >= 0 && t >= c.stallAt && t < c.stallAt + c.stallLen;
    if (tCap <= tOut) {
      for (int n : chunks) {
        if (bytes.size() < size_t(n) * size_t(bpf)) bytes.resize(size_t(n) * size_t(bpf));
        for (int i = 0; i < n; ++i) {
          const int16_t v = int16_t(std::lrint(amp * std::sin(w * double(produced + uint64_t(i)))));
          uint8_t* f = bytes.data() + size_t(i) * size_t(bpf);
          for (int ch = 0; ch < spec.channels; ++ch)
            desk::s16ToSample(ch == 0 ? v : ch == 1 ? int16_t(-v) : int16_t(1000 * ch), spec, f + ch * bps);
        }
        if (!inGap) {
          if (!c.splitBytes) {
            g_sink.deliver(bytes.data(), n * bpf, t);
          } else {   // the same bytes, cut somewhere else
            pending.insert(pending.end(), bytes.begin(), bytes.begin() + n * bpf);
            const size_t cut = pending.size() > 7 ? pending.size() - 1 - size_t(rng.unit() * 7) : 0;
            if (cut) {
              g_sink.deliver(pending.data(), int(cut), t);
              pending.erase(pending.begin(), pending.begin() + long(cut));
            }
            splitCarry = pending.size();
          }
        }
        produced += uint64_t(n);   // the device's clock runs on while unplugged
      }
      plan();
      tCap = capDue(rng.unit() * J);
      continue;
    }
    // output event: one or more callbacks back to back
    ++outEvents;
    tOut = double(outEvents + 1) * outPeriod + rng.unit() * J;
    if (inStall) continue;
    int calls = c.outBurst;
    if (devModel) {
      const double take = double(c.outDevFrames) * c.rate / c.outDevRate;
      calls = 0;
      while (streamAvail < take + 4.0) { streamAvail += c.outFrames; ++calls; }
      streamAvail -= take;
    }
    for (int call = 0; call < calls; ++call) {
      std::memset(out.data(), 0, out.size() * 2);
      const int raw = int(g_buf.waiting());
      g_buf.mixInto(out.data(), c.outFrames, c.rate, t);
      const DriftBuffer::Stats s = g_buf.stats();
      if (s.primes != primesSeen) {   // (re)primed in this call: the fade restarts
        primesSeen = s.primes;
        havePrev = false;
        sinceFade = 0;
      }
      const bool settled = t > 60.0 && !inGap && !inStall &&
                           !(c.gapAt >= 0 && t >= c.gapAt && t < c.gapAt + c.gapLen + 30.0) &&
                           !(c.stallAt >= 0 && t >= c.stallAt && t < c.stallAt + c.stallLen + 30.0);
      const bool nearEvent = (c.gapAt >= 0 && t >= c.gapAt && t < c.gapAt + c.gapLen + 1.0) ||
                             (c.stallAt >= 0 && t >= c.stallAt && t < c.stallAt + c.stallLen + 1.0);
      for (int i = 0; i < c.outFrames && s.primed; ++i) {
        const int16_t L = out[size_t(i) * 2];
        if (sinceFade++ < DriftBuffer::kFadeFrames + 8) { prevL = L; havePrev = true; continue; }
        if (havePrev && !nearEvent && std::abs(int(L) - int(prevL)) > maxStep) ++res.glitches;
        prevL = L;
        havePrev = true;
      }
      if (!s.primed) havePrev = false;
      if (c.analyzeAt >= 0 && t >= c.analyzeAt && rec.size() < recWant)
        for (int i = 0; i < c.outFrames && rec.size() < recWant; ++i) rec.push_back(float(out[size_t(i) * 2]));
      if (g_trace && t >= nextTrace) {   // PG_LISTEN_TRACE=1: the controller every 10 s
        nextTrace += 10.0;
        std::printf("    t %6.0f s  fill %7.1f  target %5d  rate %+8.1f ppm  clock %+7.1f ppm  raw %5d\n", t,
                    double(s.fillFrames), s.target, double(s.ppm), double(s.clockPpm), raw);
      }
      if (settled) {
        fillSum += raw;
        ++fillN;
        const double e = std::fabs(double(s.fillFrames) - s.target);
        if (e > res.maxSmoothedErr) res.maxSmoothedErr = e;
        if (raw < res.minFill) res.minFill = raw;
        if (raw > res.maxFill) res.maxFill = raw;
      }
    }
  }
  (void)splitCarry;
  const DriftBuffer::Stats s = g_buf.stats();
  res.underruns = s.underruns;
  res.overflows = s.overflows;
  res.resyncs = s.resyncs;
  res.primes = s.primes;
  res.finalPpm = s.ppm;
  res.clockPpm = s.clockPpm;
  res.target = s.target;
  res.clipped = s.clipped;
  res.meanFill = fillN ? fillSum / double(fillN) : 0.0;
  res.fitted = rec.size() == recWant;
  if (res.fitted) res.fit = sinefit::fit(rec.data(), rec.size(), c.rate, 997.0);
  return res;
}

void report(const Cfg& c, const Result& r) {
  const int capRate = c.capRate ? c.capRate : c.rate;
  std::printf("  %-44s target %4d fr  fill %4d..%4d fr, mean %.1f ms, max %.1f ms  |err| %5.1f fr  "
              "clock %+6.1f ppm  under %u over %u resync %u primes %u glitches %ld",
              c.name, r.target, r.minFill, r.maxFill, 1000.0 * r.meanFill / capRate, 1000.0 * r.maxFill / capRate,
              r.maxSmoothedErr, r.clockPpm, r.underruns, r.overflows, r.resyncs, r.primes, r.glitches);
  if (r.fitted)
    std::printf("  | %.3f Hz, %.0f peak, THD+N %.1f dB", r.fit.freq, r.fit.amp, r.fit.thdnDb);
  std::printf("\n");
}

// The tone came through: the device's frequency in its own clock, the level, nothing else.
void checkTone(const Cfg& c, const Result& r, double maxThdn = -60.0) {
  CHECK(r.fitted, "%s: no output to analyse", c.name);
  if (!r.fitted) return;
  const double want = 997.0 * (1.0 + c.ppm * 1e-6);
  // within 100 ppm (0.17 cent): the slow loop wanders a little; a wrong rate would be percent
  CHECK(std::fabs(r.fit.freq / want - 1.0) < 100e-6, "%s: tone at %.4f Hz, want %.4f", c.name, r.fit.freq, want);
  const double db = 20.0 * std::log10(r.fit.amp / 8192.0);
  CHECK(std::fabs(db) < 0.05, "%s: level %+.3f dB off", c.name, db);
  CHECK(r.fit.thdnDb < maxThdn, "%s: THD+N %.1f dB (want < %.0f)", c.name, r.fit.thdnDb, maxThdn);
}

void steady(const Cfg& c, double maxLatencyMs = 40.0) {
  const Result r = run(c);
  report(c, r);
  const int capRate = c.capRate ? c.capRate : c.rate;
  CHECK(r.underruns == 0, "%s: %u underruns", c.name, r.underruns);
  CHECK(r.overflows == 0, "%s: %u overflows", c.name, r.overflows);
  CHECK(r.resyncs == 0, "%s: %u resyncs", c.name, r.resyncs);
  CHECK(r.primes == 1, "%s: primed %u times (want once)", c.name, r.primes);
  CHECK(r.glitches == 0, "%s: %ld glitches", c.name, r.glitches);
  CHECK(r.maxSmoothedErr < 0.6 * r.target, "%s: fill strayed %.0f frames from target %d", c.name,
        r.maxSmoothedErr, r.target);
  CHECK(1000.0 * r.meanFill / capRate < maxLatencyMs, "%s: %.1f ms of added latency (want < %.0f)", c.name,
        1000.0 * r.meanFill / capRate, maxLatencyMs);
  CHECK(r.maxFill < 2 * r.target, "%s: fill up to %d frames (target %d)", c.name, r.maxFill, r.target);
  CHECK(r.minFill > 0, "%s: fill down to %d frames", c.name, r.minFill);
  CHECK(std::fabs(r.clockPpm - c.ppm) < 30.0, "%s: estimated clock %+.1f ppm for a %+.1f ppm clock", c.name,
        r.clockPpm, c.ppm);
  CHECK(r.clipped == 0, "%s: %llu clipped samples", c.name, static_cast<unsigned long long>(r.clipped));
  if (c.analyzeAt >= 0) checkTone(c, r);
}

// ---- CaptureSink: every format, channel count and callback cut ------------------------

void convertCase(const char* name, const CaptureSpec& s, const std::vector<uint8_t>& raw,
                 const std::vector<int16_t>& wantLR) {
  // whole, then one byte at a time: the same frames
  for (int mode = 0; mode < 3; ++mode) {
    new (&g_buf) DriftBuffer();
    new (&g_sink) CaptureSink();
    g_sink.setBuffer(&g_buf);
    g_sink.begin(s);
    if (mode == 0) g_sink.deliver(raw.data(), int(raw.size()));
    else if (mode == 1) for (size_t i = 0; i < raw.size(); ++i) g_sink.deliver(&raw[i], 1);
    else for (size_t i = 0; i < raw.size(); i += 5) g_sink.deliver(&raw[i], int(raw.size() - i < 5 ? raw.size() - i : 5));
    const uint64_t frames = g_buf.stats().pushed;
    // read back exactly: the ring holds what was pushed (frames start at index 0)
    bool ok = frames == wantLR.size() / 2;
    std::vector<int16_t> ring(size_t(frames) * 2 + 2);
    for (uint64_t i = 0; i < frames; ++i) g_buf.peekFrame(i, &ring[size_t(i) * 2]);
    for (size_t i = 0; ok && i < wantLR.size(); ++i) ok = ring[i] == wantLR[i];
    CHECK(ok, "convert %s (%s, %s): %llu frames, first %d %d (want %d %d)", name, s.describe().c_str(),
          mode == 0 ? "whole" : mode == 1 ? "byte by byte" : "5-byte pieces", static_cast<unsigned long long>(frames),
          frames ? ring[0] : 0, frames ? ring[1] : 0, wantLR[0], wantLR[1]);
  }
}

void convertTests() {
  std::printf("CaptureSink: formats, channels, split callbacks\n");
  const int16_t vals[] = {0, 1, -1, 1000, -1000, 12345, -12345, 32767, -32768, 256, -256};
  const uint16_t fmts[] = {desk::kFmtS16LE, desk::kFmtS16BE, desk::kFmtU16LE, desk::kFmtU16BE, desk::kFmtS32LE,
                           desk::kFmtS32BE, desk::kFmtF32LE, desk::kFmtF32BE, desk::kFmtU8, desk::kFmtS8};
  for (uint16_t f : fmts)
    for (int ch : {1, 2, 6}) {
      CaptureSpec s;
      s.format = f;
      s.channels = ch;
      std::vector<uint8_t> raw;
      std::vector<int16_t> want;
      for (int16_t v : vals) {
        for (int c = 0; c < ch; ++c) {
          uint8_t b[4];
          const int16_t x = c == 0 ? v : c == 1 ? int16_t(-int(v) > 32767 ? 32767 : -int(v)) : int16_t(7777);
          desk::s16ToSample(x, s, b);
          raw.insert(raw.end(), b, b + s.bytesPerSample());
        }
        // what the conversion must give: 8-bit keeps the top byte only
        auto q = [&](int16_t x) { return s.bits() == 8 ? int16_t(x & ~0xFF) : x; };
        const int16_t l = q(v), r = q(int16_t(-int(v) > 32767 ? 32767 : -int(v)));
        want.push_back(l);
        want.push_back(ch == 1 ? l : r);
      }
      char nm[32];
      std::snprintf(nm, sizeof(nm), "%s x%d", s.formatName().c_str(), ch);
      convertCase(nm, s, raw, want);
    }
  // Floats beyond +-1 saturate; NaN is silence.
  {
    CaptureSpec s;
    s.format = desk::kFmtF32LE;
    s.channels = 2;
    const float fv[] = {1.5f, -2.0f, NAN, 0.5f};
    std::vector<uint8_t> raw(sizeof(fv));
    std::memcpy(raw.data(), fv, sizeof(fv));
    convertCase("f32 out of range", s, raw, {32767, -32768, 0, 16384});
  }
  // Byte order: s16be 0x1234 is bytes 12 34.
  {
    CaptureSpec s;
    s.format = desk::kFmtS16BE;
    s.channels = 2;
    convertCase("s16be bytes", s, {0x12, 0x34, 0xFF, 0xFE}, {0x1234, -2});
  }
  CaptureSpec p;
  CHECK(CaptureSpec::parse("f32:1:48000:1024", &p) && p.format == desk::kFmtF32LE && p.channels == 1 &&
            p.rate == 48000 && p.frames == 1024,
        "parse f32:1:48000:1024");
  CHECK(CaptureSpec::parse("0x9010:2:44100:441", &p) && p.format == desk::kFmtS16BE, "parse a hex format");
  CHECK(!CaptureSpec::parse("s24:2:44100:256", &p) && !CaptureSpec::parse("f32:0:48000:256", &p), "parse rejects");
}

}  // namespace

int main() {
  convertTests();
  std::printf("DriftBuffer against simulated clocks\n");
  {
    Cfg c; c.name = "+500 ppm, 256/256, 3 ms jitter, 1 h"; c.ppm = 500; c.analyzeAt = 3000; steady(c);
  }
  {
    Cfg c; c.name = "-500 ppm, 256/256, 3 ms jitter, 1 h"; c.ppm = -500; steady(c);
  }
  {
    Cfg c; c.name = "+61 ppm (the device), 512/512, 1 h"; c.ppm = 61; c.capFrames = 512; c.outFrames = 512;
    c.jitterMs = 4.0; c.analyzeAt = 1800; steady(c, 50.0);
  }
  {
    Cfg c; c.name = "-500 ppm, variable capture, out 100"; c.ppm = -500; c.capVariable = true; c.outFrames = 100;
    c.jitterMs = 1.5; c.seconds = 900; c.analyzeAt = 600; steady(c);
  }
  {
    Cfg c; c.name = "+500 ppm, 32 kHz, cap 128, out 512"; c.ppm = 500; c.rate = 32000; c.capFrames = 128;
    c.outFrames = 512; c.jitterMs = 2.0; c.seconds = 900; c.analyzeAt = 600; steady(c, 50.0);
  }
  {
    Cfg c; c.name = "0 ppm, no jitter"; c.jitterMs = 0.0; c.seconds = 300; c.analyzeAt = 200; steady(c);
  }
  std::printf("Obtained specs other than the one asked for, and the Mac's delivery patterns\n");
  {
    // sdl2-compat on CoreAudio: capture AudioQueue buffers handed over 4 x 256 at a time;
    // output: a 48 kHz device taking 1024 frames a cycle through a resampling stream.
    Cfg c; c.name = "Mac: s16 2ch, capture 4x256, output 48k/1024"; c.ppm = 61; c.capBurst = 4;
    c.outDevRate = 48000; c.outDevFrames = 1024; c.jitterMs = 1.0; c.seconds = 600; c.analyzeAt = 300;
    steady(c, 75.0);
  }
  {
    Cfg c; c.name = "Mac: s16 2ch, capture 2x256, output 48k/512"; c.ppm = 61; c.capBurst = 2;
    c.outDevRate = 48000; c.outDevFrames = 512; c.jitterMs = 1.0; c.seconds = 600; c.analyzeAt = 300;
    steady(c, 40.0);
  }
  {
    Cfg c; c.name = "f32 mono 48 kHz, 1024-frame callbacks"; c.ppm = -200; c.format = desk::kFmtF32LE;
    c.channels = 1; c.capRate = 48000; c.capFrames = 1024; c.outFrames = 512; c.seconds = 600; c.analyzeAt = 300;
    steady(c, 70.0);
  }
  {
    Cfg c; c.name = "s32 stereo 32 kHz, odd 441-frame callbacks"; c.ppm = 300; c.format = desk::kFmtS32LE;
    c.capRate = 32000; c.capFrames = 441; c.capVariable = true; c.outFrames = 256; c.seconds = 600;
    c.analyzeAt = 300; steady(c, 70.0);
  }
  {
    Cfg c; c.name = "f32be 6ch 96 kHz, bytes split anywhere"; c.ppm = 100; c.format = desk::kFmtF32BE;
    c.channels = 6; c.capRate = 96000; c.capFrames = 480; c.splitBytes = true; c.outFrames = 256; c.seconds = 600;
    c.analyzeAt = 300; steady(c, 40.0);
  }
  {
    Cfg c; c.name = "u16 stereo 22.05 kHz, 4096-frame callbacks"; c.ppm = -61; c.format = desk::kFmtU16LE;
    c.capRate = 22050; c.capFrames = 4096; c.outFrames = 256; c.seconds = 600; c.analyzeAt = 300;
    steady(c, 250.0);
  }
  {
    // The pattern changes while playing (bursts of 4 x 256 from 2 min on): the target grows,
    // approached at 1000 ppm by feed-forward; at most one dropout, the clock estimate untouched.
    Cfg c; c.name = "+61 ppm, bursts grow 1 -> 4 at 120 s"; c.ppm = 61; c.capBurst = 1; c.burstChangeAt = 120;
    c.capBurst2 = 4; c.seconds = 600; c.analyzeAt = 500; c.jitterMs = 1.0;
    const Result r = run(c);
    report(c, r);
    CHECK(r.underruns <= 1 && r.primes <= 2, "growing bursts: %u underruns, %u primes", r.underruns, r.primes);
    CHECK(r.overflows == 0 && r.resyncs == 0, "growing bursts: %u overflows %u resyncs", r.overflows, r.resyncs);
    CHECK(r.target > 1024, "growing bursts: the target grew (%d)", r.target);
    CHECK(std::fabs(r.clockPpm - c.ppm) < 30.0, "growing bursts: clock %+.1f ppm", r.clockPpm);
    checkTone(c, r);
  }
  {
    // Unplugged for 2 s at 2 min, plugged back in: one underrun, primes again, then steady.
    Cfg c; c.name = "+500 ppm, unplug 2 s at 120 s"; c.ppm = 500; c.seconds = 600; c.gapAt = 120; c.gapLen = 2;
    const Result r = run(c);
    report(c, r);
    CHECK(r.underruns == 1, "unplug: %u underruns (want 1)", r.underruns);
    CHECK(r.primes == 2, "unplug: primed %u times (want 2)", r.primes);
    CHECK(r.overflows == 0 && r.resyncs == 0, "unplug: %u overflows %u resyncs", r.overflows, r.resyncs);
    CHECK(r.glitches == 0, "unplug: %ld glitches outside the gap", r.glitches);
    CHECK(r.maxFill < 2 * r.target, "unplug: fill up to %d after re-plug", r.maxFill);
    CHECK(r.target < 2000, "unplug: the gap was not learnt as a delivery pattern (target %d)", r.target);
    CHECK(std::fabs(r.clockPpm - c.ppm) < 30.0, "unplug: clock %+.1f ppm", r.clockPpm);
  }
  {
    // The output stops for 250 ms (the computer stalled): the input piles up; the
    // consumer skips back to the target once instead of keeping 250 ms of latency.
    Cfg c; c.name = "-500 ppm, output stall 250 ms"; c.ppm = -500; c.seconds = 600; c.stallAt = 200; c.stallLen = 0.25;
    const Result r = run(c);
    report(c, r);
    CHECK(r.resyncs == 1, "stall: %u resyncs (want 1)", r.resyncs);
    CHECK(r.overflows == 0, "stall: %u overflows", r.overflows);
    CHECK(r.underruns == 0, "stall: %u underruns", r.underruns);
    CHECK(r.maxFill < 2 * r.target, "stall: fill up to %d afterwards", r.maxFill);
    CHECK(r.target < 2000, "stall: not learnt as a delivery pattern (target %d)", r.target);
    CHECK(r.glitches == 0, "stall: %ld glitches outside the skip", r.glitches);
  }
  {
    // Saturating mix: the device on top of a full-scale engine output clips, never wraps,
    // and every clipped sample is counted.
    new (&g_buf) DriftBuffer();
    g_buf.setTarget(1024);
    g_buf.restart();
    static int16_t in[64 * 2];
    for (int i = 0; i < 64; ++i) { in[2 * i] = 30000; in[2 * i + 1] = -30000; }
    int16_t o[64 * 2];
    bool ok = true;
    uint64_t wantClipped = 0;
    for (int k = 0; k < 600; ++k) {   // 64 in, 64 out: past the watch and the fade
      g_buf.push(in, 64);
      for (int i = 0; i < 64; ++i) { o[2 * i] = 30000; o[2 * i + 1] = -30000; }
      g_buf.mixInto(o, 64, 44100);
      if (k >= 590) {
        for (int i = 0; i < 64; ++i) ok = ok && o[2 * i] == 32767 && o[2 * i + 1] == -32768;
        wantClipped = g_buf.stats().clipped;
      }
    }
    CHECK(g_buf.stats().primed && ok, "the mix saturates");
    CHECK(wantClipped >= 128 && g_buf.stats().underruns == 0, "clipped samples counted (%llu)",
          static_cast<unsigned long long>(wantClipped));
  }
  std::printf(g_fail ? "FAILED: %d check(s)\n" : "OK\n", g_fail);
  return g_fail ? 1 : 0;
}
