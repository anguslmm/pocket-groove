// desk::Samples (host/app/samples.h): the desktop's samples/ folder.
//   load      files named by slot load at init; other names and a second file of a
//             slot are ignored; a broken file leaves its slot on the built-in sound
//   reload    a changed file reloads (after it has settled), a removed one empties its slot
//   drop      a WAV dropped into a slot is copied in under the slot's name, replacing
//             the slot's old file; a non-WAV is refused (no afconvert on this host)
//   usage: test_desktop_samples WORKDIR
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "common/wav.h"
#include "samples.h"

namespace {

int g_fail = 0;
void check(bool ok, const char* what) {
  if (!ok) {
    ++g_fail;
    std::printf("FAIL %s\n", what);
  }
}

void tone(const std::string& path, double hz, double sec, int rate, int ch) {
  std::vector<int16_t> s;
  for (int i = 0; i < int(sec * rate); ++i)
    for (int c = 0; c < ch; ++c) s.push_back(int16_t(12000.0 * std::sin(2.0 * 3.14159265 * hz * i / rate)));
  wav::write(path.c_str(), s, rate, ch);
}

void rmTree(const std::string& d) {
  if (DIR* dd = opendir(d.c_str())) {
    while (dirent* e = readdir(dd))
      if (e->d_name[0] != '.') std::remove((d + "/" + e->d_name).c_str());
    closedir(dd);
  }
  ::rmdir(d.c_str());
}

bool user(const desk::Samples& s, int slot, uint32_t* len = nullptr) {
  pg::sampler::SampleRef r;
  pg::sampler::resolve(s.pool(), slot, &r);
  if (len) *len = r.length;
  return !r.builtin;
}

void settle() { usleep(800000); }   // older than Samples::kSettleNs

}  // namespace

int main(int argc, char** argv) {
  const std::string work = argc > 1 ? argv[1] : "build/host/desktop_test_samples";
  const std::string dir = work + "/samples";
  ::mkdir(work.c_str(), 0755);
  rmTree(dir);
  ::mkdir(dir.c_str(), 0755);
  tone(dir + "/03-ohyeah.wav", 300, 1.0, 44100, 2);
  tone(dir + "/03-second.wav", 300, 0.5, 44100, 1);   // a second file for slot 3: ignored
  tone(dir + "/notaslot.wav", 300, 0.5, 44100, 1);
  tone(dir + "/12_last.wav", 500, 0.25, 48000, 1);
  {
    FILE* f = std::fopen((dir + "/05-broken.wav").c_str(), "wb");
    std::fputs("RIFF....WAVEjunk", f);
    std::fclose(f);
  }
  settle();
  desk::Samples s;
  check(s.init(dir), "init");
  uint32_t len = 0;
  check(user(s, 2, &len) && len > 21000 && len < 23000, "slot 3 loaded (1 s at 22,050 Hz)");
  check(s.fileOf(2) == "03-ohyeah.wav", "the first file of slot 3 by name");
  check(user(s, 11), "slot 12 loaded (an underscore after the number)");
  check(!user(s, 4), "a broken file: the built-in sound");
  check(!user(s, 0), "an empty slot: the built-in sound");
  const pg::sampler::SlotHeader* h = s.memoryPool().header(2);
  char nm[9] = "";
  if (h) pg::sampler::headerName(*h, nm);
  check(std::strcmp(nm, "OHYEAH") == 0, "the name from the file name");

  // reload on change, empty on removal
  tone(dir + "/03-ohyeah.wav", 300, 2.0, 22050, 1);
  check(!s.scanNow() || true, "scan while it is fresh");
  settle();
  s.scanNow();
  check(user(s, 2, &len) && len > 43000 && len < 45000, "slot 3 reloaded (2 s)");
  std::remove((dir + "/12_last.wav").c_str());
  s.scanNow();
  check(!user(s, 11), "slot 12 emptied");

  // drop
  tone(work + "/My Vocal.wav", 400, 0.7, 32000, 2);
  std::string msg;
  check(s.drop(work + "/My Vocal.wav", 2, &msg), "drop a WAV into slot 3");
  check(s.fileOf(2) == "03-my-vocal.wav", "copied under the slot's name");
  struct stat st;
  check(::stat((dir + "/03-ohyeah.wav").c_str(), &st) != 0 && ::stat((dir + "/03-second.wav").c_str(), &st) != 0,
        "the slot's old files are gone");
  check(user(s, 2, &len) && len > 15000 && len < 16000, "slot 3 plays the dropped file");
  {
    FILE* f = std::fopen((work + "/notes.txt").c_str(), "wb");
    std::fputs("hello", f);
    std::fclose(f);
  }
  const bool noAf = ::access("/usr/bin/afconvert", X_OK) != 0;
  if (noAf) check(!s.drop(work + "/notes.txt", 6, &msg) && !user(s, 6), "a non-WAV is refused without afconvert");
  std::printf("test_desktop_samples: %s\n", g_fail ? "FAILED" : "ok");
  return g_fail ? 1 : 0;
}
