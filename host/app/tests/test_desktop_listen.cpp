// test_desktop_listen: DriftBuffer (host/app/listen_buffer.h), the rate match
// between the device's USB audio clock and the computer's output clock,
// against two simulated clocks.
//
// Discrete-event simulation: the capture side delivers a block every
// capFrames / (rate * (1 + ppm)) seconds, the output side asks for outFrames
// every outFrames / rate seconds; each callback arrives late by a random
// jitter (uniform 0..J, never reordering a stream's own callbacks). The input
// is a 997 Hz sine at -12 dBFS; any dropout, skip or repeated block shows as a
// step larger than the sine's steepest slope.
//
// Checked for an hour of simulated time at +-500 ppm (the requirement; the
// measured device is +61 ppm): no underrun, overflow or resync after the
// start, fill (= added latency) bounded and under 40 ms, the correction
// settles on the clock difference, no glitch. Plus: variable capture block
// sizes, an output buffer that is not a power of two, unplug + re-plug, and
// an output stall.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>

#include "listen_buffer.h"

using desk::DriftBuffer;

namespace {

int g_fail = 0;
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
  int capFrames = 256;
  bool capVariable = false;     // blocks of capFrames/2 .. 3*capFrames/2 (the target allows for the largest)
  int outFrames = 256;
  double jitterMs = 3.0;        // per callback, both sides
  double seconds = 3600.0;
  double gapAt = -1.0, gapLen = 0.0;          // capture stops (unplug) and resumes
  double stallAt = -1.0, stallLen = 0.0;      // output stops calling back
  int rate = 44100;
};

struct Result {
  uint32_t underruns, overflows, resyncs, primes;
  int minFill, maxFill;          // raw fill before each output call, after the first minute (outside gaps/stalls)
  double meanFill;
  double finalPpm, clockPpm;
  long glitches;
  double maxSmoothedErr;         // |smoothed fill - target| after the first minute
  int target;
};

DriftBuffer g_buf;   // 64 KB: static

Result run(const Cfg& c) {
  new (&g_buf) DriftBuffer();
  const int target = DriftBuffer::targetFor(c.outFrames, c.capVariable ? c.capFrames * 3 / 2 : c.capFrames, c.rate);
  g_buf.setTarget(target);
  g_buf.restart();
  Rng rng{0x9E3779B97F4A7C15ull ^ uint64_t(c.ppm * 1000 + c.capFrames)};
  const double inRate = c.rate * (1.0 + c.ppm * 1e-6);
  const double J = c.jitterMs * 1e-3;
  // the capture side
  uint64_t produced = 0;
  int nextChunk = c.capFrames;
  auto capDue = [&](double jit) { return double(produced + uint64_t(nextChunk)) / inRate + jit; };
  double tCap = capDue(rng.unit() * J);
  // the output side
  uint64_t outCalls = 0;
  double tOut = double(c.outFrames) / c.rate + rng.unit() * J;
  std::vector<int16_t> cap(size_t(c.capFrames) * 4), out(size_t(c.outFrames) * 2);
  const double w = 2.0 * M_PI * 997.0 / c.rate;   // the device's sine, in its own samples
  const double amp = 8192.0;
  const double maxStep = amp * w * 1.05 + 4.0;     // steepest slope + Hermite overshoot + rounding
  Result res{};
  res.minFill = 1 << 30;
  res.maxFill = 0;
  res.target = target;
  int16_t prevL = 0;
  bool havePrev = false;
  uint32_t primesSeen = 0;
  long sinceFade = 0;
  double fillSum = 0.0;
  long fillN = 0;
  for (;;) {
    const double t = tCap < tOut ? tCap : tOut;
    if (t > c.seconds) break;
    const bool inGap = c.gapAt >= 0 && t >= c.gapAt && t < c.gapAt + c.gapLen;
    const bool inStall = c.stallAt >= 0 && t >= c.stallAt && t < c.stallAt + c.stallLen;
    if (tCap <= tOut) {
      for (int i = 0; i < nextChunk; ++i) {
        const int16_t v = int16_t(std::lrint(amp * std::sin(w * double(produced + uint64_t(i)))));
        cap[size_t(i) * 2] = v;
        cap[size_t(i) * 2 + 1] = int16_t(-v);
      }
      if (!inGap) g_buf.push(cap.data(), uint32_t(nextChunk));
      produced += uint64_t(nextChunk);   // the device's clock runs on while unplugged
      nextChunk = c.capVariable ? c.capFrames / 2 + int(rng.unit() * c.capFrames) : c.capFrames;
      tCap = capDue(rng.unit() * J);
      continue;
    }
    // output callback
    ++outCalls;
    tOut = double((outCalls + 1) * uint64_t(c.outFrames)) / c.rate + rng.unit() * J;
    if (inStall) continue;
    std::memset(out.data(), 0, out.size() * 2);
    const int raw = int(g_buf.waiting());
    g_buf.mixInto(out.data(), c.outFrames, c.rate);
    const DriftBuffer::Stats s = g_buf.stats();
    // glitch check on the left channel, from the end of each fade-in on
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
    if (settled) {
      const int fill = raw;
      fillSum += raw;
      ++fillN;
      const double e = std::fabs(double(s.fillFrames) - target);
      if (e > res.maxSmoothedErr) res.maxSmoothedErr = e;
      if (fill < res.minFill) res.minFill = fill;
      if (fill > res.maxFill) res.maxFill = fill;
    }
  }
  const DriftBuffer::Stats s = g_buf.stats();
  res.underruns = s.underruns;
  res.overflows = s.overflows;
  res.resyncs = s.resyncs;
  res.primes = s.primes;
  res.finalPpm = s.ppm;
  res.clockPpm = s.clockPpm;
  res.meanFill = fillN ? fillSum / double(fillN) : 0.0;
  return res;
}

void report(const Cfg& c, const Result& r) {
  std::printf("  %-36s target %4d fr  fill %4d..%4d fr, mean %.1f ms, max %.1f ms  |err| %5.1f fr  "
              "clock %+6.1f ppm  under %u over %u resync %u primes %u glitches %ld\n",
              c.name, r.target, r.minFill, r.maxFill, 1000.0 * r.meanFill / c.rate, 1000.0 * r.maxFill / c.rate,
              r.maxSmoothedErr, r.clockPpm, r.underruns, r.overflows, r.resyncs, r.primes, r.glitches);
}

void steady(const Cfg& c) {
  const Result r = run(c);
  report(c, r);
  CHECK(r.underruns == 0, "%s: %u underruns", c.name, r.underruns);
  CHECK(r.overflows == 0, "%s: %u overflows", c.name, r.overflows);
  CHECK(r.resyncs == 0, "%s: %u resyncs", c.name, r.resyncs);
  CHECK(r.primes == 1, "%s: primed %u times (want once)", c.name, r.primes);
  CHECK(r.glitches == 0, "%s: %ld glitches", c.name, r.glitches);
  CHECK(r.maxSmoothedErr < 0.5 * r.target, "%s: fill strayed %.0f frames from target %d", c.name, r.maxSmoothedErr,
        r.target);
  CHECK(1000.0 * r.meanFill / c.rate < 40.0, "%s: %.1f ms of added latency (want < 40)", c.name,
        1000.0 * r.meanFill / c.rate);
  CHECK(r.maxFill < 2 * r.target, "%s: fill up to %d frames (target %d)", c.name, r.maxFill, r.target);
  CHECK(r.minFill > c.outFrames, "%s: fill down to %d frames", c.name, r.minFill);
  CHECK(std::fabs(r.clockPpm - c.ppm) < 30.0, "%s: estimated clock %+.1f ppm for a %+.1f ppm clock", c.name,
        r.clockPpm, c.ppm);
}

}  // namespace

int main() {
  std::printf("DriftBuffer against simulated clocks\n");
  {
    Cfg c; c.name = "+500 ppm, 256/256, 3 ms jitter, 1 h"; c.ppm = 500; steady(c);
  }
  {
    Cfg c; c.name = "-500 ppm, 256/256, 3 ms jitter, 1 h"; c.ppm = -500; steady(c);
  }
  {
    Cfg c; c.name = "+61 ppm (the device), 512/512, 1 h"; c.ppm = 61; c.capFrames = 512; c.outFrames = 512;
    c.jitterMs = 4.0; steady(c);
  }
  {
    Cfg c; c.name = "-500 ppm, variable capture, out 100"; c.ppm = -500; c.capVariable = true; c.outFrames = 100;
    c.jitterMs = 1.5; c.seconds = 900; steady(c);
  }
  {
    Cfg c; c.name = "+500 ppm, 32 kHz, cap 128, out 512"; c.ppm = 500; c.rate = 32000; c.capFrames = 128;
    c.outFrames = 512; c.jitterMs = 2.0; c.seconds = 900; steady(c);
  }
  {
    Cfg c; c.name = "0 ppm, no jitter"; c.jitterMs = 0.0; c.seconds = 300; steady(c);
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
    CHECK(r.glitches == 0, "stall: %ld glitches outside the skip", r.glitches);
  }
  {
    // Saturating mix: the device on top of a full-scale engine output clips, never wraps.
    new (&g_buf) DriftBuffer();
    g_buf.setTarget(1024);
    g_buf.restart();
    int16_t o[64 * 2];
    for (int16_t& v : o) v = 0;
    g_buf.mixInto(o, 64, 44100);   // picks up the restart
    static int16_t in[2048 * 2];
    for (int i = 0; i < 2048; ++i) { in[2 * i] = 30000; in[2 * i + 1] = -30000; }
    g_buf.push(in, 2048);
    bool ok = true;
    for (int k = 0; k < 8; ++k) {
      for (int i = 0; i < 64; ++i) { o[2 * i] = 30000; o[2 * i + 1] = -30000; }
      g_buf.mixInto(o, 64, 44100);
      if (k >= 6)
        for (int i = 0; i < 64; ++i) ok = ok && o[2 * i] == 32767 && o[2 * i + 1] == -32768;
    }
    CHECK(ok, "the mix saturates");
  }
  std::printf(g_fail ? "FAILED: %d check(s)\n" : "OK\n", g_fail);
  return g_fail ? 1 : 0;
}
