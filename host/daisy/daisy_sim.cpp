// daisy_sim: the Daisy firmware's App (daisy/app, the same code the device runs) on
// the host, its USB serial port on a pseudo-terminal, so the computer-side tools
// (tools/pg-daisy: keys, report, screen; tools/pg: projects, samples) can be tried
// and tested without the board (tools/test_pg_daisy.py).
//
//   daisy_sim [--flash FILE] [--autoplay] [--stdio] [--line-in mono|stereo|silent]
//             [--clock-wrap-in S]
//
// --clock-wrap-in S: the simulated millis(), micros() and cycles() start so that each
// 32-bit clock wraps S seconds after the start (cycles() at 480 MHz wraps every 8.9 s
// anyway), to try the firmware across its clocks' wraps (host/daisy/test_daisy_clock.cpp).
//
// --line-in: a synthetic signal on the codec's line input (the recording path,
// line_mic.h): a 440 Hz tone at -12 dBFS, 0.8 s on and 0.7 s off, over a -66 dBFS
// noise floor; "mono" puts it on L with R digital silence (a mono plug), "stereo"
// on both (R a fifth up), "silent" only the floor. Without it there is no line input.
//
// Prints the pty path on the first line of stdout, then runs until killed (or, with
// --stdio, serves stdin / stdout and ends at EOF). The audio "interrupt" is the
// render call, made in the loop whenever a 96-frame block is due (real time), so
// the engine runs as on the device, single-threaded. The QSPI flash (/int's
// snapshot banks and the pool's copy) is a RAM image with NOR rules; --flash keeps
// it in FILE between runs, so a restart finds what was saved, as the board would.
#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "app/app.h"
#include "core/layout.h"
#include "core/nor_sim.h"
#include "core/ring.h"

namespace {

using namespace pgd;

constexpr uint32_t kRate = 48000, kBlock = 96;
constexpr uint32_t kFlashBytes = 2 * layout::kFsBankBytes + layout::kPoolBytes;

uint32_t nowMs() {
  using namespace std::chrono;
  return uint32_t(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}
uint64_t nowUs64() {
  using namespace std::chrono;
  return uint64_t(duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

// An offset view of one flash image (the device: two QSPI regions).
struct Region final : pg::sampler::FlashDriver {
  pg::sampler::FlashDriver* base = nullptr;
  uint32_t off = 0;
  bool erase(uint32_t o, uint32_t n) override { return base->erase(off + o, n); }
  bool write(uint32_t o, const void* d, uint32_t n) override { return base->write(off + o, d, n); }
  bool read(uint32_t o, void* d, uint32_t n) override { return base->read(off + o, d, n); }
};

struct SimHal final : Hal {
  int fdIn = -1, fdOut = -1;
  ByteRing tx;
  uint64_t t0 = nowUs64();
  uint32_t msOff = 0, usOff = 0;   // --clock-wrap-in
  uint32_t millis() override { return uint32_t((nowUs64() - t0) / 1000u) + msOff; }
  uint32_t micros() override { return uint32_t(nowUs64() - t0) + usOff; }
  uint32_t cycles() override { return uint32_t((nowUs64() - t0 + usOff) * 480u); }
  uint32_t cpuHz() override { return 480000000u; }
  void serialWrite(const char* p, size_t n) override { tx.pushSome(reinterpret_cast<const uint8_t*>(p), uint32_t(n)); }
  size_t serialRoom() override { return tx.space(); }
  int serialRead() override {
    uint8_t b;
    const ssize_t r = ::read(fdIn, &b, 1);
    return r == 1 ? b : -1;
  }
  void serialFlush() override {
    uint8_t buf[4096];
    for (;;) {
      const uint32_t n = tx.pop(buf, sizeof(buf));
      if (!n) break;
      size_t done = 0;
      while (done < n) {
        const ssize_t w = ::write(fdOut, buf + done, n - done);
        if (w > 0) done += size_t(w);
        else {
          struct pollfd p = {fdOut, POLLOUT, 0};
          poll(&p, 1, 20);
          if (!(p.revents & POLLOUT)) return;   // nobody reads: drop the rest of this chunk
        }
      }
    }
  }
  void setLed(bool) override {}
  void restart(bool) override { restartWanted = true; }
  bool audioRunning() override { return true; }
  uint32_t sampleRate() override { return kRate; }
  uint32_t blockFrames() override { return kBlock; }
  bool lineInput() override { return lineIn != 0; }
  int lineIn = 0;   // 0 none, 1 mono, 2 stereo, 3 silent
  const char* boardName() override { return "host simulation (no codec)"; }
  const char* stageName() override { return "stage 0 (simulated)"; }
  void reportLines(LineFn fn, void* user) override {
    fn(user, "[mem] host simulation: no memory map (the firmware's comes from its linker symbols)");
  }
  bool restartWanted = false;
};

std::vector<uint8_t> g_flash(kFlashBytes, 0xFF);
NorSim g_nor(g_flash.data(), kFlashBytes);
Region g_fsRegion, g_poolRegion;
std::vector<uint8_t> g_fsArena(size_t(RamFs::kMaxFiles) * layout::kFsSlotBytes);
std::vector<uint8_t> g_fsStage(layout::kFsBankBytes);
std::vector<uint8_t> g_poolImage(layout::kPoolBytes);
std::vector<uint8_t> g_poolStage(4096);
std::vector<uint8_t> g_txMem(layout::kTxRing);
std::vector<int16_t> g_lineRing(65536);
App g_app;   // one per program (the engine's state is a single static block)

// The synthetic line input (see the top).
struct LineSynth {
  int mode = 0;
  uint64_t n = 0;
  uint32_t seed = 1;
  void block(float* out, int frames) {
    for (int i = 0; i < frames; ++i, ++n) {
      const double t = double(n) / kRate;
      const bool on = mode != 3 && std::fmod(t, 1.5) < 0.8;
      seed = seed * 1664525u + 1013904223u;
      const float noise = float(int32_t(seed)) / 2147483648.0f * 0.0005f;   // -66 dBFS
      const float l = (on ? 0.25f * float(std::sin(2.0 * 3.14159265358979 * 440.0 * t)) : 0.0f) + noise;
      const float r = mode == 2 ? (on ? 0.25f * float(std::sin(2.0 * 3.14159265358979 * 660.0 * t)) : 0.0f) + noise
                                : mode == 3 ? noise : 0.0f;
      out[2 * i] = l;
      out[2 * i + 1] = r;
    }
  }
};

// Written whole to a temporary file, then renamed: a kill mid-write never leaves half an image.
void saveFlash(const std::string& path) {
  if (path.empty()) return;
  const std::string tmp = path + ".tmp";
  FILE* f = std::fopen(tmp.c_str(), "wb");
  if (!f) return;
  const bool ok = std::fwrite(g_flash.data(), 1, g_flash.size(), f) == g_flash.size();
  if (std::fclose(f) == 0 && ok) std::rename(tmp.c_str(), path.c_str());
}

volatile std::sig_atomic_t g_stop = 0;
void onSignal(int) { g_stop = 1; }

}  // namespace

int main(int argc, char** argv) {
  std::string flashPath;
  bool autoplay = false, useStdio = false;
  int lineIn = 0;
  double wrapIn = -1.0;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--flash" && i + 1 < argc) flashPath = argv[++i];
    else if (a == "--autoplay") autoplay = true;
    else if (a == "--stdio") useStdio = true;
    else if (a == "--clock-wrap-in" && i + 1 < argc) wrapIn = std::atof(argv[++i]);
    else if (a == "--line-in" && i + 1 < argc) {
      const std::string m = argv[++i];
      lineIn = m == "mono" ? 1 : m == "stereo" ? 2 : m == "silent" ? 3 : -1;
      if (lineIn < 0) {
        std::fprintf(stderr, "daisy_sim: --line-in mono|stereo|silent\n");
        return 2;
      }
    } else {
      std::fprintf(stderr,
                   "usage: daisy_sim [--flash FILE] [--autoplay] [--stdio] [--line-in mono|stereo|silent] "
                   "[--clock-wrap-in S]\n");
      return 2;
    }
  }
  if (!flashPath.empty()) {
    if (FILE* f = std::fopen(flashPath.c_str(), "rb")) {
      const size_t n = std::fread(g_flash.data(), 1, g_flash.size(), f);
      (void)n;
      std::fclose(f);
    }
  }
  std::signal(SIGTERM, onSignal);
  std::signal(SIGINT, onSignal);
  SimHal hal;
  hal.lineIn = lineIn;
  if (wrapIn >= 0.0) {
    hal.msOff = uint32_t(0x100000000ull - uint64_t(wrapIn * 1000.0));
    hal.usOff = uint32_t(0x100000000ull - uint64_t(wrapIn * 1.0e6));
  }
  hal.tx.attach(g_txMem.data(), uint32_t(g_txMem.size()));
  if (useStdio) {
    hal.fdIn = 0;
    hal.fdOut = 1;
    fcntl(0, F_SETFL, fcntl(0, F_GETFL) | O_NONBLOCK);
  } else {
    const int m = posix_openpt(O_RDWR | O_NOCTTY);
    if (m < 0 || grantpt(m) != 0 || unlockpt(m) != 0) {
      std::perror("pty");
      return 1;
    }
    struct termios t;
    const char* name = ptsname(m);
    const int s = ::open(name, O_RDWR | O_NOCTTY);   // keep the slave open: no EIO while the client reconnects
    if (s >= 0 && tcgetattr(s, &t) == 0) {
      cfmakeraw(&t);
      tcsetattr(s, TCSANOW, &t);
    }
    fcntl(m, F_SETFL, fcntl(m, F_GETFL) | O_NONBLOCK);
    hal.fdIn = hal.fdOut = m;
    std::printf("%s\n", name);
    std::fflush(stdout);
  }

  g_fsRegion.base = &g_nor;
  g_fsRegion.off = 0;
  g_poolRegion.base = &g_nor;
  g_poolRegion.off = 2 * layout::kFsBankBytes;
  AppMemory mem;
  mem.fsArena = g_fsArena.data();
  mem.fsSlotBytes = layout::kFsSlotBytes;
  mem.fsStaging = g_fsStage.data();
  mem.fsBankBytes = layout::kFsBankBytes;
  mem.fsFlash = &g_fsRegion;
  mem.poolImage = g_poolImage.data();
  mem.poolStaging = g_poolStage.data();
  mem.poolFlash = &g_poolRegion;
  mem.lineRing = g_lineRing.data();
  mem.lineRingSamples = uint32_t(g_lineRing.size());
  AppConfig cfg;
  cfg.sampleRate = float(kRate);
  cfg.autoplay = autoplay;
  cfg.version = "sim";
  g_app.boot(hal, mem, cfg);

  float block[kBlock * 2];
  float in[kBlock * 2];
  LineSynth synth;
  synth.mode = lineIn;
  const uint64_t periodUs = uint64_t(kBlock) * 1000000u / kRate;
  uint64_t nextBlock = nowUs64();
  uint32_t lastSave = nowMs();
  while (!g_stop) {
    // The "interrupt": every block that is due (the sim's output goes nowhere).
    int n = 0;
    while (nowUs64() >= nextBlock && n++ < 8) {
      if (lineIn) synth.block(in, int(kBlock));
      g_app.render(block, int(kBlock), lineIn ? in : nullptr);
      nextBlock += periodUs;
    }
    if (nowUs64() > nextBlock + 100000u) nextBlock = nowUs64();   // the host stalled: no catch-up burst
    g_app.loopOnce();
    if (hal.restartWanted) {
      hal.restartWanted = false;
      saveFlash(flashPath);
      g_app.boot(hal, mem, cfg);
    }
    if (nowMs() - lastSave > 2000) {
      lastSave = nowMs();
      saveFlash(flashPath);
    }
    if (useStdio) {
      struct pollfd p = {0, POLLIN, 0};
      if (poll(&p, 1, 0) > 0 && (p.revents & POLLHUP) && !(p.revents & POLLIN)) break;
    }
    std::this_thread::sleep_for(std::chrono::microseconds(500));
  }
  saveFlash(flashPath);
  return 0;
}
