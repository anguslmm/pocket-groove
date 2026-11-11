// render_demo: render a factory demo song (pg::demo) to WAV, either the whole
// song chain once through or one pattern looped.
//
//   render_demo --demo N [--song | --pattern P [--loops L]] [--sr 44100|32000]
//               [--mono] [--duck-off | --duck-beat] [--solo T] [--mute T[,T..]] [--tail S]
//               [--seed N] [--out PATH.wav]
//
// --song (default) plays the chain from slot 0 until the slot before the first
// "end" (repeats == 0), i.e. exactly one pass of the song. --pattern P loops
// pattern P (0-based) L times (default 2) with song mode off. --duck-off sets
// the ducker source to Off (A/B against the normal render with ears.py
// --compare); --duck-beat sets it to Every beat, so a solo without the kick
// still shows the ducking. Prints the length and peak.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "common/wav.h"
#include "pg/demo/demo_songs.h"
#include "pg/engine/engine_hooks.h"
#include "pg/instrument/instrument.h"

namespace {

pg::Instrument g_inst;

void mkdirs(const std::string& path) {
  for (size_t i = 1; i < path.size(); ++i)
    if (path[i] == '/') mkdir(path.substr(0, i).c_str(), 0755);
}

void usage() {
  std::fprintf(stderr,
               "usage: render_demo --demo N [--song | --pattern P [--loops L]] [--sr 44100|32000] [--mono]\n"
               "                   [--duck-off | --duck-beat] [--solo T] [--mute T,T..] [--tail S] [--seed N] [--out PATH.wav]\n");
}

}  // namespace

int main(int argc, char** argv) {
  float sr = 44100.0f;
  bool mono = false, duckOff = false, duckBeat = false;
  int demo = -1, pattern = -1, loops = 2, solo = -1;
  uint32_t seed = 0;
  float tail = 1.0f;
  std::string out, mutes;
  for (int i = 1; i < argc; ++i) {
    const char* a = argv[i];
    auto next = [&]() -> const char* {
      if (i + 1 >= argc) { usage(); std::exit(2); }
      return argv[++i];
    };
    if (!std::strcmp(a, "--sr")) sr = float(std::atof(next()));
    else if (!std::strcmp(a, "--mono")) mono = true;
    else if (!std::strcmp(a, "--duck-off")) duckOff = true;
    else if (!std::strcmp(a, "--duck-beat")) duckBeat = true;
    else if (!std::strcmp(a, "--demo")) demo = std::atoi(next());
    else if (!std::strcmp(a, "--song")) pattern = -1;
    else if (!std::strcmp(a, "--pattern")) pattern = std::atoi(next());
    else if (!std::strcmp(a, "--loops")) loops = std::atoi(next());
    else if (!std::strcmp(a, "--solo")) solo = std::atoi(next());
    else if (!std::strcmp(a, "--mute")) mutes = next();
    else if (!std::strcmp(a, "--tail")) tail = float(std::atof(next()));
    else if (!std::strcmp(a, "--seed")) seed = uint32_t(std::strtoul(next(), nullptr, 0));
    else if (!std::strcmp(a, "--out")) out = next();
    else { usage(); return 2; }
  }
  if (sr != 44100.0f && sr != 32000.0f) { std::fprintf(stderr, "--sr must be 44100 or 32000\n"); return 2; }
  // The songs are called directly (not through pg::demo::build), so this
  // renders them even while the ROM list in demo.cpp is being reworked.
  using Builder = void (*)(const pg::Engine&, pg::Project&);
  const Builder kSongs[] = {pg::demo::buildAcidJam, pg::demo::buildBassHaus, pg::demo::buildWarehous};
  const int kNumSongs = int(sizeof(kSongs) / sizeof(kSongs[0]));
  if (demo < 0 || demo >= kNumSongs) { std::fprintf(stderr, "--demo must be 0..%d\n", kNumSongs - 1); return 2; }
  if (pattern >= pg::kNumPatterns || loops < 1 || loops > 64) { usage(); return 2; }

  g_inst.init(sr, mono);
  pg::Engine& eng = g_inst.engine();
  pg::Project& p = g_inst.project();
  kSongs[demo](eng, p);
  eng.sanitizeProject(p);
  if (duckOff) p.globals[pg::kGDuckSource] = 0;
  if (duckBeat) p.globals[pg::kGDuckSource] = 2;   // keeps ducking with the kick muted (solo measurements)
  if (solo >= 0 && solo < pg::kNumTracks)
    for (int t = 0; t < pg::kNumTracks; ++t) p.tracks[t].mute = t == solo ? 0 : 1;
  for (size_t k = 0; k < mutes.size(); ++k)
    if (mutes[k] >= '0' && mutes[k] <= '7') p.tracks[mutes[k] - '0'].mute = 1;
  if (seed) pg::engine_detail::setRngSeed(seed);

  long steps = 0;
  if (pattern < 0) {
    p.songMode = 1;
    for (int s = 0; s < pg::kSongSlots && p.song[s].repeats; ++s)
      steps += long(p.patterns[p.song[s].pattern].length) * p.song[s].repeats;
    if (!steps) { std::fprintf(stderr, "demo %d has an empty song\n", demo); return 1; }
  } else {
    p.songMode = 0;
    pg::Command q{};
    q.type = pg::CmdType::QueuePattern;
    q.a = uint8_t(pattern);
    eng.post(q);
    steps = long(p.patterns[pattern].length) * loops;
  }
  pg::Command play{};
  play.type = pg::CmdType::Play;
  eng.post(play);

  const int bpm = p.globals[pg::kGTempo];
  const double stepSamples = double(sr) * 15.0 / double(bpm);
  const long playFrames = long(std::ceil(double(steps) * stepSamples / pg::kQuantum)) * pg::kQuantum;
  const long tailFrames = long(std::ceil(double(tail) * double(sr) / pg::kQuantum)) * pg::kQuantum;
  std::vector<int16_t> pcm(size_t(playFrames + tailFrames) * 2);
  const int kBlock = 256;
  long f = 0;
  while (f < playFrames) {
    const int n = int(std::min<long>(kBlock, playFrames - f));
    g_inst.process(pcm.data() + 2 * f, n);
    f += n;
  }
  pg::Command stop{};
  stop.type = pg::CmdType::Stop;
  eng.post(stop);
  while (f < playFrames + tailFrames) {
    const int n = int(std::min<long>(kBlock, playFrames + tailFrames - f));
    g_inst.process(pcm.data() + 2 * f, n);
    f += n;
  }
  int peak = 0;
  for (int16_t s : pcm) peak = std::max(peak, std::abs(int(s)));
  if (out.empty())
    out = "build/renders/demo/demo" + std::to_string(demo) +
          (pattern < 0 ? std::string("_song") : "_p" + std::to_string(pattern + 1)) + (mono ? "_mono" : "_stereo") +
          (duckOff ? "_duckoff" : "") + ".wav";
  mkdirs(out);
  if (!wav::write(out.c_str(), pcm, int(sr), 2)) { std::fprintf(stderr, "cannot write %s\n", out.c_str()); return 1; }
  std::printf("%s: %s %s, %ld steps at %d BPM, %.1f s, peak %.2f dBFS, faults %u\n", out.c_str(),
              p.name, pattern < 0 ? "song" : "pattern", steps, bpm, double(pcm.size() / 2) / double(sr),
              20.0 * std::log10(std::max(1, peak) / 32767.0), pg::engine_detail::faultCount());
  return 0;
}
