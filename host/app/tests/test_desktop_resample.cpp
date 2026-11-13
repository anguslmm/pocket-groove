// Resampling on the desktop (docs/DESKTOP.md "Resampling"): the app's own path, with
// no audio input device at all. The real Instrument runs on a second thread (as SDL's
// audio callback does), the host's engine-output recorder (host/common/host_resample.h)
// is polled from this thread (as the UI frame does) and commits through desk::Samples
// (samples.cpp: no SDL), as main.cpp wires it:
//   in time   the groove plays (a lead note on step 9 only: the bar starts silent),
//             the take starts on a bar line and, let go mid-bar, ends on the next:
//             samples/03-res03.wav (22,050 Hz mono 16-bit) holds exactly the recording,
//             the slot loads it WHOLE (the leading silence kept: not trimmed, unlike a
//             microphone take or a dropped file) and levelled to -1 dBFS; a fresh
//             Samples on the same folder (the next launch) loads it whole again
//   stopped   transport stopped: a note played starts it, the release ends it
//   replace   erase() clears the slot's file (the Ui asks first)
//   threads   the tap is removed while the audio thread runs, many times: no crash, no
//             recording after the end
//   usage: test_desktop_resample WORKDIR
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "common/host_resample.h"
#include "common/sample_convert.h"
#include "pg/instrument/instrument.h"
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

pg::Instrument g_inst;
std::atomic<bool> g_run{false};
std::atomic<long> g_frames{0};

// The audio thread: blocks of 128 frames at about four times real time.
void audioThread() {
  int16_t buf[2 * 128];
  while (g_run.load()) {
    g_inst.process(buf, 128);
    g_frames.fetch_add(128);
    std::this_thread::sleep_for(std::chrono::microseconds(700));
  }
}

void post(const pg::Command& c) {
  while (!g_inst.engine().post(c)) std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

// Polls (the UI frame) until `frames` more have played or the take is over.
void pollFor(hostrs::HostResample& rs, long frames, pg::sampler::RecView* v) {
  const long end = g_frames.load() + frames;
  while (g_frames.load() < end) {
    rs.poll(v);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}
bool pollUntilOver(hostrs::HostResample& rs, pg::sampler::RecView* v, long maxFrames) {
  const long end = g_frames.load() + maxFrames;
  while (g_frames.load() < end) {
    rs.poll(v);
    if (!rs.armed()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return false;
}

bool exists(const std::string& p) {
  struct stat st;
  return ::stat(p.c_str(), &st) == 0;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string work = argc > 1 ? argv[1] : "build/host/desktop_test_resample";
  const std::string dir = work + "/samples";
  std::string cmd = "rm -rf '" + work + "' && mkdir -p '" + dir + "'";
  if (std::system(cmd.c_str()) != 0) return 2;

  desk::Samples samples;
  check(samples.init(dir), "the samples folder");
  g_inst.init(44100.0f, false);
  g_inst.setSamplePool(samples.pool());
  hostrs::HostResample rs;
  desk::Samples* smp = &samples;
  rs.init(
      &g_inst.engine(), &g_inst.sampler(), 44100.0f,
      [smp](int slot, const int16_t* d, uint32_t n, std::string* err) { return smp->putRecording(slot, d, n, err, "res"); },
      [smp](int slot) {
        std::string msg;
        return smp->clearSlot(slot, &msg);
      });

  // a lead note on step 9 only, 180 BPM (a bar is 1.33 s)
  pg::Step st = pg::kDefaultStep;
  st.setFlags(pg::kStepOn);
  st.note = 60;
  st.vel = 120;
  st.gate = 4;
  post(pg::Command{pg::CmdType::SetStep, 0, uint8_t(pg::kTrackLead), 8, st, nullptr});
  post(pg::Command{pg::CmdType::SetGlobal, uint8_t(pg::kGTempo), 0, 180, pg::kDefaultStep, nullptr});
  g_run.store(true);
  std::thread audio(audioThread);
  post(pg::Command{pg::CmdType::Play, 0, 0, 0, pg::kDefaultStep, nullptr});
  while (!g_inst.engine().isPlaying()) std::this_thread::sleep_for(std::chrono::milliseconds(1));

  const double bar = 44100.0 * 60.0 / 180.0 * 4.0;
  pg::sampler::RecView v;
  // ---- in time
  {
    const char* why = "";
    check(rs.ready(&why), "ready with no input device");
    check(rs.start(2), "start (slot 3)");
    pollFor(rs, long(bar * 1.4), &v);
    check(v.state == pg::sampler::RecState::Recording && v.sync && v.resample, "recording in time");
    rs.release();
    check(pollUntilOver(rs, &v, long(bar * 2)), "over within a bar of the release");
    check(rs.lastState == pg::sampler::RecState::Done, "Done");
    const uint32_t len = rs.lastLength;
    check(std::fabs(double(rs.lastEndFrame) - std::round(double(rs.lastEndFrame) / bar) * bar) <= 16.0 &&
              rs.lastEndFrame >= uint32_t(bar) - 16,
          "whole bars");
    check(samples.fileOf(2) == "03-res03.wav" && exists(dir + "/03-res03.wav"), "samples/03-res03.wav");
    std::vector<uint8_t> bytes;
    sconv::Audio a;
    std::string err;
    check(sconv::readFile(dir + "/03-res03.wav", &bytes) && sconv::parseWav(bytes, &a, &err) && a.rate == 22050 &&
              a.channels == 1 && a.mono.size() == len,
          "the file: 22,050 Hz mono, the recording's length");
    const pg::sampler::SlotHeader* h = samples.memoryPool().header(2);
    check(h && h->samples == len, "loaded whole: not trimmed (the bar's silent start kept)");
    long lead = 0;
    for (uint32_t i = 0; i < len / 4; ++i) lead += rs.recorded()[i] != 0 ? 1 : 0;
    check(lead < long(len / 4), "the take does start with silence (the note is on step 9)");
    check(h && h->peak > 28000 && h->peak < 29500, "levelled to -1 dBFS");
    std::this_thread::sleep_for(std::chrono::milliseconds(800));   // a file younger than 0.7 s waits (samples.h)
    desk::Samples again;
    again.init(dir);
    again.scanNow();
    const pg::sampler::SlotHeader* h2 = again.memoryPool().header(2);
    check(h2 && h2->samples == len, "the next launch loads it whole too");
  }
  // ---- stopped: a note played starts it
  {
    post(pg::Command{pg::CmdType::Stop, 0, 0, 0, pg::kDefaultStep, nullptr});
    while (g_inst.engine().isPlaying()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    pollFor(rs, 44100 * 3, &v);   // the tails die away
    check(rs.start(4), "start stopped (slot 5)");
    pollFor(rs, 8000, &v);
    check(v.state == pg::sampler::RecState::Waiting && !v.sync, "waiting for a sound");
    post(pg::Command{pg::CmdType::NoteOn, uint8_t(pg::kTrackLead), 57, 110, st, nullptr});
    pollFor(rs, 20000, &v);
    post(pg::Command{pg::CmdType::NoteOff, uint8_t(pg::kTrackLead), 57, 0, st, nullptr});
    check(v.state == pg::sampler::RecState::Recording, "a note starts it");
    pollFor(rs, 10000, &v);
    rs.release();
    check(pollUntilOver(rs, &v, 44100), "the release ends it");
    check(rs.lastState == pg::sampler::RecState::Done && samples.fileOf(4) == "05-res05.wav", "05-res05.wav");
    check(rs.lastLength > 12000 && rs.lastLength < 18000, "from the note to the release");
  }
  // ---- replace: erase clears the file
  {
    check(rs.erase(2) && samples.fileOf(2).empty() && !exists(dir + "/03-res03.wav"), "erase clears the slot");
  }
  // ---- threads: the tap in and out while the audio thread runs
  {
    post(pg::Command{pg::CmdType::Play, 0, 0, 0, pg::kDefaultStep, nullptr});
    int ok = 0;
    for (int i = 0; i < 40; ++i) {
      if (rs.start(7)) ++ok;
      rs.release();
      pollUntilOver(rs, &v, 44100 * 3);
    }
    check(ok == 40 && !rs.armed() && samples.fileOf(7).empty(), "40 takes let go at once: nothing recorded");
  }
  g_run.store(false);
  audio.join();
  std::printf("test_desktop_resample: %d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
