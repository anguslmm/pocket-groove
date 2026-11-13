// desk::Mic (host/app/mic.h): the desktop's microphone path with a synthetic source
// standing in for SDL's capture device (mono int16 at 44.1 kHz, delivered as a
// capture thread would, here from the test's own thread):
//   record    start -> the room (WAITING) -> a voice (RECORDING) -> release: the slot's
//             file samples/03-rec03.wav is written (22,050 Hz mono 16-bit), the slot
//             loads it (levelled to -1 dBFS by the converter), the source is closed
//   replace   erase() removes the slot's files (the Ui asks first); a new recording
//             takes the slot; the old file is gone
//   nothing   release before any sound: CANCELLED, no file, the slot stays empty
//   failure   a source that cannot open: start() is refused with a reason
//   overrun   the main thread stalled for 4 s: the ring (3 s) drops the excess and counts it
//   usage: test_desktop_mic WORKDIR
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "common/sample_convert.h"
#include "mic.h"
#include "samples.h"

namespace {

int g_fail = 0, g_pass = 0;
void check(bool ok, const char* what) {
  if (ok) {
    ++g_pass;
  } else {
    ++g_fail;
    std::printf("FAIL %s\n", what);
  }
}

class SynthSource final : public desk::MicSource {
 public:
  bool failOpen = false;
  bool isOpen = false;
  int opens = 0;
  bool open(Callback cb, void* user, std::string* err) override {
    if (failOpen) {
      *err = "no such device";
      return false;
    }
    cb_ = cb;
    user_ = user;
    isOpen = true;
    ++opens;
    return true;
  }
  void close() override { isOpen = false; }
  std::string name() const override { return "synthetic"; }
  // ms of the room plus, at amp > 0, a sung 220 Hz vowel, in 512-sample callbacks
  void deliver(uint32_t ms, double amp) {
    if (!isOpen) return;
    std::normal_distribution<double> nd(0.0, 25.0);
    const size_t n = size_t(ms) * 441 / 10;
    std::vector<int16_t> b(512);
    for (size_t done = 0; done < n;) {
      const size_t k = std::min<size_t>(512, n - done);
      for (size_t i = 0; i < k; ++i) {
        double x = nd(rng_);
        const double w = 2.0 * 3.14159265358979 * 220.0 * double(t_++) / 44100.0;
        if (amp > 0) x += amp * (0.6 * std::sin(w) + 0.3 * std::sin(2 * w) + 0.1 * std::sin(3 * w));
        b[i] = int16_t(std::lround(x));
      }
      cb_(b.data(), int(k), user_);
      done += k;
    }
  }

 private:
  Callback cb_ = nullptr;
  void* user_ = nullptr;
  std::mt19937 rng_{5};
  uint64_t t_ = 0;
};

void rmTree(const std::string& d) {
  if (DIR* dd = opendir(d.c_str())) {
    while (dirent* e = readdir(dd))
      if (e->d_name[0] != '.') std::remove((d + "/" + e->d_name).c_str());
    closedir(dd);
  }
  ::rmdir(d.c_str());
}

bool exists(const std::string& p) {
  struct stat st;
  return ::stat(p.c_str(), &st) == 0;
}

pg::sampler::RecState pollState(desk::Mic& m) {
  pg::sampler::RecView v;
  m.poll(&v);
  return v.state;
}

}  // namespace

int main(int argc, char** argv) {
  using pg::sampler::RecState;
  const std::string work = argc > 1 ? argv[1] : "build/host/desktop_test_mic";
  const std::string dir = work + "/samples";
  rmTree(dir);
  ::mkdir(work.c_str(), 0755);
  desk::Samples samples;
  check(samples.init(dir), "init");
  SynthSource src;
  desk::Mic mic;
  mic.init(&samples, &src);
  const char* why = "";
  check(mic.ready(&why), "ready");
  check(mic.slotErased(2), "files need no erase");

  // record
  check(mic.start(2) && mic.armed() && src.isOpen, "start opens the source");
  src.deliver(300, 0.0);
  check(pollState(mic) == RecState::Waiting, "waiting in the room");
  src.deliver(700, 9000.0);
  pg::sampler::RecView v;
  mic.poll(&v);
  check(v.state == RecState::Recording && v.samples > 13000 && v.colsUsed > 0, "recording the voice");
  src.deliver(200, 0.0);
  mic.release();
  src.deliver(40, 0.0);
  mic.poll(&v);
  check(v.state == RecState::Done, "done after the release");
  check(!src.isOpen && !mic.armed(), "the source is closed");
  check(samples.fileOf(2) == "03-rec03.wav" && exists(dir + "/03-rec03.wav"), "the slot's file");
  {
    std::vector<uint8_t> f;
    sconv::Audio a;
    std::string err;
    check(sconv::readFile(dir + "/03-rec03.wav", &f) && sconv::parseWav(f, &a, &err) && a.rate == 22050 &&
              a.channels == 1 && a.mono.size() == v.samples,
          "a 22,050 Hz mono WAV of the recording's length");
  }
  pg::sampler::SampleRef r;
  pg::sampler::resolve(samples.pool(), 2, &r);
  check(!r.builtin && std::strcmp(r.name, "REC03") == 0, "the slot plays it, named REC03");
  check(r.peak > 28000 && r.peak < 30000, "levelled to -1 dBFS on load");

  // replace: erase (the Ui asked), then a new one
  check(mic.erase(2) && samples.fileOf(2).empty() && !exists(dir + "/03-rec03.wav"), "erase removes the file");
  pg::sampler::resolve(samples.pool(), 2, &r);
  check(r.builtin, "the slot plays its built-in sound again");
  check(mic.start(2), "start again");
  src.deliver(250, 0.0);
  src.deliver(400, 5000.0);
  mic.release();
  src.deliver(30, 0.0);
  check(pollState(mic) == RecState::Done && samples.fileOf(2) == "03-rec03.wav", "a new recording takes the slot");

  // nothing
  check(mic.start(5), "start slot 6");
  src.deliver(500, 0.0);
  mic.release();
  check(pollState(mic) == RecState::Cancelled && samples.fileOf(5).empty(), "no sound: no file");
  check(pollState(mic) == RecState::Off, "then off");

  // failure
  src.failOpen = true;
  check(!mic.start(7) && !mic.armed(), "a source that cannot open");
  mic.poll(&v);
  check(std::strcmp(v.why, "CANNOT OPEN") == 0, "says why");
  src.failOpen = false;

  // overrun: 4 s without a poll
  check(mic.start(8), "start slot 9");
  src.deliver(4000, 0.0);
  check(mic.dropped() > 30000, "the ring drops what does not fit and counts it");
  mic.release();
  mic.poll(&v);
  check(!mic.armed(), "ends");

  std::printf("test_desktop_mic: %d passed, %d failed\n", g_pass, g_fail);
  rmTree(dir);
  return g_fail ? 1 : 0;
}
