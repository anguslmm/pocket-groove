// xfer_sim: the device side of the USB project transfer (pg::io::Xfer, the
// same code the firmware runs) on a pseudo-terminal, so the real Python
// client (tools/pg.py) can be tested on the desktop: tools/test_pg.py.
//
//   xfer_sim --root DIR [--no-sd] [--playing] [--seed N] [--fault P]
//            [--log-ms N] [--stall-after N] [--stall-ms N] [--demos]
//
// --demos first saves the factory demos the way the device saves projects
// (pg::io): ACIDJAM to int, BASSHAUS to sd (int without a card), and
// WAREHOUS as the autosaved session int/pg/_last.pgv.
//
// Prints the pty path on the first line of stdout, then serves until killed.
// The filesystem is host/common/hostfs.h on DIR (DIR/int/pg, DIR/sd/pg).
// Like the firmware it also prints log lines at any time (--log-ms, e.g.
// "[stats] ..."), answers the single-letter 'r' with a multi-line report,
// and services timeouts once per "frame" (10 ms).
// Faults, to exercise the client's retries: --fault P corrupts each line in
// either direction with probability P (a flipped byte, a dropped byte, a
// truncated line or a lost line, chosen at random). --stall-after N stops
// answering after N protocol lines for --stall-ms (default: forever), like a
// device that went away in the middle of a transfer.
#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "common/hostfs.h"
#include "pg/demo/demo.h"
#include "pg/instrument/instrument.h"
#include "pg/io/xfer.h"

namespace {

struct Rng {
  uint32_t s;
  uint32_t next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
  double unit() { return double(next() & 0xFFFFFF) / double(0x1000000); }
};

Rng g_rng{12345};
pg::Instrument g_inst;
pg::Project g_proj;
double g_fault = 0.0;
int g_fd = -1;

uint32_t nowMs() {
  using namespace std::chrono;
  return uint32_t(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

void writeAll(const char* p, size_t n) {
  while (n) {
    const ssize_t w = ::write(g_fd, p, n);
    if (w < 0) {
      if (errno == EINTR || errno == EAGAIN) { usleep(1000); continue; }
      return;   // nobody listening (EIO): the line is lost, as on the device
    }
    p += w;
    n -= size_t(w);
  }
}

// Applies a random fault to a line (either direction). Returns false to drop it.
bool mangle(std::string& l) {
  if (g_fault <= 0.0 || g_rng.unit() >= g_fault) return true;
  switch (g_rng.next() % 4) {
    case 0: l[g_rng.next() % l.size()] ^= char(1 + g_rng.next() % 127); return true;   // flipped byte
    case 1: l.erase(g_rng.next() % l.size(), 1); return true;                          // dropped byte
    case 2: l.resize(g_rng.next() % l.size()); return true;                            // truncated
    default: return false;                                                              // lost
  }
}

struct PtyOut final : pg::io::XferOut {
  void send(const char* line, size_t len) override {
    std::string l(line, len);
    if (mangle(l)) writeAll(l.data(), l.size());
  }
};

}  // namespace

int main(int argc, char** argv) {
  std::string root;
  bool sd = true, playing = false, demos = false;
  long logMs = 0, stallAfter = -1, stallMs = -1;
  for (int i = 1; i < argc; ++i) {
    const char* a = argv[i];
    auto next = [&]() -> const char* {
      if (i + 1 >= argc) { std::fprintf(stderr, "xfer_sim: %s needs a value\n", a); std::exit(2); }
      return argv[++i];
    };
    if (!std::strcmp(a, "--root")) root = next();
    else if (!std::strcmp(a, "--no-sd")) sd = false;
    else if (!std::strcmp(a, "--playing")) playing = true;
    else if (!std::strcmp(a, "--seed")) g_rng.s = uint32_t(std::strtoul(next(), nullptr, 0)) | 1u;
    else if (!std::strcmp(a, "--fault")) g_fault = std::atof(next());
    else if (!std::strcmp(a, "--log-ms")) logMs = std::atol(next());
    else if (!std::strcmp(a, "--stall-after")) stallAfter = std::atol(next());
    else if (!std::strcmp(a, "--stall-ms")) stallMs = std::atol(next());
    else if (!std::strcmp(a, "--demos")) demos = true;
    else { std::fprintf(stderr, "xfer_sim: unknown option %s\n", a); return 2; }
  }
  if (root.empty()) { std::fprintf(stderr, "usage: xfer_sim --root DIR [options]\n"); return 2; }
  pgtest::HostFs fs(root);
  if (!fs.createMounts(sd)) { std::fprintf(stderr, "xfer_sim: cannot create %s\n", root.c_str()); return 1; }
  if (demos) {
    g_inst.init(44100.0f, true);
    for (int d = 0; d < 3 && d < pg::demo::count(); ++d) {
      pg::demo::build(g_inst.engine(), g_proj, d);
      pg::io::Status st;
      if (d == 2) st = pg::io::saveSession(fs, g_proj);
      else st = pg::io::saveProject(fs, d == 1 && sd ? pg::io::kMountSd : pg::io::kMountInt, pg::demo::name(d), g_proj);
      if (st != pg::io::Status::Ok) { std::fprintf(stderr, "xfer_sim: save: %s\n", pg::io::statusName(st)); return 1; }
    }
  }

  g_fd = ::posix_openpt(O_RDWR | O_NOCTTY);
  if (g_fd < 0 || ::grantpt(g_fd) != 0 || ::unlockpt(g_fd) != 0) { std::perror("xfer_sim: pty"); return 1; }
  const char* slave = ::ptsname(g_fd);
  // Keep a slave fd open in raw mode: the pty survives client reconnects and
  // nothing echoes or translates newlines before the client configures it.
  const int sfd = ::open(slave, O_RDWR | O_NOCTTY);
  termios t;
  if (sfd < 0 || ::tcgetattr(sfd, &t) != 0) { std::perror("xfer_sim: slave"); return 1; }
  ::cfmakeraw(&t);
  ::tcsetattr(sfd, TCSANOW, &t);
  std::printf("%s\n", slave);
  std::fflush(stdout);

  pg::io::Xfer x;
  x.init(&fs, "sim-1.0");
  PtyOut out;
  pg::io::XferEnv env;
  env.playing = playing;
  uint32_t lastLog = nowMs(), stallStart = 0;
  long handled = 0;
  bool stalled = false;
  std::string hostLine;   // raw bytes of the current host line, for fault injection
  for (;;) {
    pollfd p{g_fd, POLLIN, 0};
    ::poll(&p, 1, 10);
    env.nowMs = nowMs();
    if (stalled && stallMs >= 0 && env.nowMs - stallStart > uint32_t(stallMs)) stalled = false;
    uint8_t buf[512];
    const ssize_t n = (p.revents & POLLIN) ? ::read(g_fd, buf, sizeof(buf)) : 0;
    for (ssize_t i = 0; i < n; ++i) {
      if (stalled) continue;   // the "device" is gone: bytes vanish
      const uint8_t b = buf[i];
      // Host -> device faults are applied per line before the protocol sees it.
      if (b == '~' || !hostLine.empty()) {
        hostLine += char(b);
        if (b != '\n' && hostLine.size() < 1000) continue;
        std::string l = hostLine;
        hostLine.clear();
        if (!mangle(l)) continue;
        for (char c : l) {
          if (!x.rx(uint8_t(c), env.nowMs)) continue;
          if (x.lineReady()) {
            x.service(env, out);
            if (stallAfter >= 0 && ++handled >= stallAfter && !stalled) {
              stalled = true;
              stallStart = env.nowMs;
              stallAfter = -1;
            }
          }
        }
        continue;
      }
      if (x.rx(b, env.nowMs)) continue;
      if (b == 'r') {
        const char* rep =
            "\n===== POCKET GROOVE REPORT sim =====\n[sys] uptime 1s ~ not a frame\n[fs] /int mounted\n===== END REPORT =====\n";
        writeAll(rep, std::strlen(rep));
      }
    }
    x.service(env, out);   // timeouts, once per "frame"
    if (logMs > 0 && env.nowMs - lastLog >= uint32_t(logMs)) {
      lastLog = env.nowMs;
      char l[128];
      const int k = std::snprintf(l, sizeof(l), "[stats] t=%lus cpu 31.2%% heap 40k min 38k%s\n",
                                  static_cast<unsigned long>(env.nowMs / 1000), x.busy() ? " xfer" : "");
      writeAll(l, size_t(k));
    }
  }
}
