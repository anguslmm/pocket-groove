// listen_analyze: the tone in a WAV (a Cmd+R recording made while listening,
// or a --listen-dump file): frequency, level and THD+N of one channel over a
// stretch, by the same fit as test_desktop_listen (sine_fit.h). For
// desktop_check.py, and for a file the owner sends back.
//
//   listen_analyze FILE.wav [--from S] [--seconds S] [--channel N] [--freq HZ]
//
// Prints one line:  rate R channels C frames N freq F amp A dbfs D thdn T worst W
// (amp in int16 units, whatever the file's format). 16-bit PCM, 8-bit PCM,
// 32-bit PCM and 32-bit float files.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "sine_fit.h"

namespace {
uint32_t rd32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint16_t rd16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: listen_analyze FILE.wav [--from S] [--seconds S] [--channel N] [--freq HZ]\n");
    return 2;
  }
  double from = 0.0, secs = 1.0, guess = 1000.0;
  int channel = 0;
  for (int i = 2; i + 1 < argc; i += 2) {
    const std::string a = argv[i];
    if (a == "--from") from = std::atof(argv[i + 1]);
    else if (a == "--seconds") secs = std::atof(argv[i + 1]);
    else if (a == "--channel") channel = std::atoi(argv[i + 1]);
    else if (a == "--freq") guess = std::atof(argv[i + 1]);
  }
  FILE* f = std::fopen(argv[1], "rb");
  if (!f) {
    std::fprintf(stderr, "cannot read %s\n", argv[1]);
    return 1;
  }
  std::vector<uint8_t> d;
  uint8_t buf[65536];
  for (size_t n; (n = std::fread(buf, 1, sizeof(buf), f)) > 0;) d.insert(d.end(), buf, buf + n);
  std::fclose(f);
  if (d.size() < 44 || std::memcmp(d.data(), "RIFF", 4) || std::memcmp(d.data() + 8, "WAVE", 4)) {
    std::fprintf(stderr, "%s: not a WAV file\n", argv[1]);
    return 1;
  }
  int tag = 0, ch = 0, rate = 0, bits = 0;
  size_t dataAt = 0, dataLen = 0;
  for (size_t p = 12; p + 8 <= d.size();) {
    const uint32_t len = rd32(&d[p + 4]);
    if (!std::memcmp(&d[p], "fmt ", 4) && p + 24 <= d.size()) {
      tag = rd16(&d[p + 8]);
      ch = rd16(&d[p + 10]);
      rate = int(rd32(&d[p + 12]));
      bits = rd16(&d[p + 22]);
    } else if (!std::memcmp(&d[p], "data", 4)) {
      dataAt = p + 8;
      dataLen = len;
      if (dataAt + dataLen > d.size()) dataLen = d.size() - dataAt;
      break;
    }
    p += 8 + len + (len & 1);
  }
  if (!dataAt || ch < 1 || channel >= ch || (bits != 8 && bits != 16 && bits != 32) || (tag != 1 && tag != 3)) {
    std::fprintf(stderr, "%s: unsupported WAV (tag %d, %d ch, %d bits)\n", argv[1], tag, ch, bits);
    return 1;
  }
  const size_t bpf = size_t(ch) * size_t(bits / 8);
  const size_t frames = dataLen / bpf;
  const size_t a = size_t(from * rate), n = size_t(secs * rate);
  if (a + n > frames || n < 64) {
    std::fprintf(stderr, "%s: %zu frames, want %zu from %zu\n", argv[1], frames, n, a);
    return 1;
  }
  std::vector<float> x(n);
  for (size_t i = 0; i < n; ++i) {
    const uint8_t* s = &d[dataAt + (a + i) * bpf + size_t(channel) * size_t(bits / 8)];
    float v;
    if (bits == 8) v = float((int(s[0]) - 128) * 256);
    else if (bits == 16) v = float(int16_t(rd16(s)));
    else if (tag == 3) {
      const uint32_t u = rd32(s);
      float fl;
      std::memcpy(&fl, &u, 4);
      v = fl * 32768.0f;
    } else v = float(int32_t(rd32(s))) / 65536.0f;
    x[i] = v;
  }
  // Window by window (2048 samples, 46 ms at 44.1 kHz), each with its own fit:
  // a slow drift of the pitch (the rate match settling) is not distortion,
  // and a dropout or click shows in its window. THD+N = all residual energy
  // over all tone energy; worst = the worst window.
  const size_t win = 2048;
  const sinefit::Fit first = sinefit::fit(x.data(), n < win ? n : win, rate, guess, 0.05);
  double res = 0.0, sig = 0.0, fsum = 0.0, asum = 0.0, worst = -999.0;
  int windows = 0;
  for (size_t at = 0; at + win <= n; at += win) {
    const sinefit::Fit f = sinefit::fit(x.data() + at, win, rate, first.freq, 0.005);
    const double e = f.amp * f.amp / 2.0 * double(win);
    sig += e;
    res += e * std::pow(10.0, f.thdnDb / 10.0);
    fsum += f.freq;
    asum += f.amp;
    if (f.thdnDb > worst) worst = f.thdnDb;
    ++windows;
  }
  if (!windows) {
    std::fprintf(stderr, "%s: too short\n", argv[1]);
    return 1;
  }
  const double amp = asum / windows;
  std::printf("rate %d channels %d frames %zu freq %.4f amp %.1f dbfs %.2f thdn %.1f worst %.1f\n", rate, ch,
              frames, fsum / windows, amp, amp > 0 ? 20.0 * std::log10(amp / 32768.0) : -999.0,
              10.0 * std::log10((res > 1e-30 ? res : 1e-30) / sig), worst);
  return 0;
}
