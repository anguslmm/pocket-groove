// render_project: the full instrument (pg::Instrument: 8 voices + engine) to a WAV.
//
//   render_project [--sr 44100|32000] [--mono] [--seed N] [--bars N] [--tail S]
//                  [--groove groove|busy] [--project FILE.pgv] [--out PATH.wav]
//                  [--solo TRACK]
//
// Without --project it programs a built-in groove (host/common/grooves.h) on
// top of the default project, so it renders the DEFAULT sounds and mix.
// --project loads a .pgv file through pg::io (when that component is built).
// --mono is device mode (Engine::setMonoOutput): one bus, L == R.
// --seed seeds the step-probability RNG. --tail adds seconds after the last
// bar with the transport stopped (FX ring out). --solo 0..7 mutes every other
// track (mix balance checks; FX returns of the soloed track stay). Prints
// peak / faults.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "common/grooves.h"
#include "common/wav.h"
#include "pg/engine/engine_hooks.h"
#include "pg/instrument/instrument.h"

#if __has_include("pg/io/project_io.h")
#include "pg/io/project_io.h"
#define PG_HAVE_IO 1
#else
#define PG_HAVE_IO 0
#endif

// The demo songs (UI engineer's pg::demo) replace the built-in groove once
// they exist: --demo N renders demo N.
#if __has_include("pg/demo/demo.h")
#include "pg/demo/demo.h"
#define PG_HAVE_DEMO 1
#else
#define PG_HAVE_DEMO 0
#endif

namespace {

pg::Instrument g_inst;   // ~70 KB: static, as on the device
pg::Project g_loaded;

void usage() {
  std::fprintf(stderr,
               "usage: render_project [--sr 44100|32000] [--mono] [--seed N] [--bars N] [--tail S]\n"
               "                      [--groove groove|busy] [--project FILE.pgv] [--demo N] [--out PATH.wav] [--solo TRACK]\n");
}

void mkdirs(const std::string& path) {
  for (size_t i = 1; i < path.size(); ++i)
    if (path[i] == '/') mkdir(path.substr(0, i).c_str(), 0755);
}

bool loadPgv(const char* path, pg::Project& dest) {
#if PG_HAVE_IO
  FILE* f = std::fopen(path, "rb");
  if (!f) { std::fprintf(stderr, "cannot open %s\n", path); return false; }
  std::vector<uint8_t> buf;
  uint8_t tmp[4096];
  size_t n;
  while ((n = std::fread(tmp, 1, sizeof(tmp), f)) > 0) buf.insert(buf.end(), tmp, tmp + n);
  std::fclose(f);
  const pg::io::Status st = pg::io::decodeFromBuffer(buf.data(), buf.size(), dest);
  if (st != pg::io::Status::Ok) {
    std::fprintf(stderr, "%s: %s\n", path, pg::io::statusName(st));
    return false;
  }
  return true;
#else
  (void)dest;
  std::fprintf(stderr, "%s: this build has no pg::io component\n", path);
  return false;
#endif
}

}  // namespace

int main(int argc, char** argv) {
  float sr = 44100.0f;
  bool mono = false;
  uint32_t seed = 0;
  int bars = 8, solo = -1, demo = -1;
  float tail = 0.0f;
  std::string grooveName = "groove", projectPath, out;
  for (int i = 1; i < argc; ++i) {
    const char* a = argv[i];
    auto next = [&]() -> const char* {
      if (i + 1 >= argc) { usage(); std::exit(2); }
      return argv[++i];
    };
    if (!std::strcmp(a, "--sr")) sr = float(std::atof(next()));
    else if (!std::strcmp(a, "--mono")) mono = true;
    else if (!std::strcmp(a, "--seed")) seed = uint32_t(std::strtoul(next(), nullptr, 0));
    else if (!std::strcmp(a, "--bars")) bars = std::atoi(next());
    else if (!std::strcmp(a, "--tail")) tail = float(std::atof(next()));
    else if (!std::strcmp(a, "--groove")) grooveName = next();
    else if (!std::strcmp(a, "--project")) projectPath = next();
    else if (!std::strcmp(a, "--out")) out = next();
    else if (!std::strcmp(a, "--solo")) solo = std::atoi(next());
    else if (!std::strcmp(a, "--demo")) demo = std::atoi(next());
    else { usage(); return 2; }
  }
  if (sr != 44100.0f && sr != 32000.0f) { std::fprintf(stderr, "--sr must be 44100 or 32000\n"); return 2; }
  if (bars < 1 || bars > 1000) { std::fprintf(stderr, "--bars must be 1..1000\n"); return 2; }
  if (out.empty()) out = "build/renders/full/" + (demo >= 0 ? "demo" + std::to_string(demo) : grooveName) + (mono ? "_mono_device" : "_stereo") + ".wav";

  g_inst.init(sr, mono);
  pg::Engine& eng = g_inst.engine();
  if (!projectPath.empty()) {
    if (!loadPgv(projectPath.c_str(), g_loaded)) return 1;
    eng.suspend();                       // not running yet: returns at once
    std::memcpy(&g_inst.project(), &g_loaded, sizeof(pg::Project));
    eng.sanitizeProject(g_inst.project());
    eng.resume();                        // first quantum re-derives everything
  } else if (demo >= 0) {
#if PG_HAVE_DEMO
    if (demo >= pg::demo::count()) { std::fprintf(stderr, "--demo must be 0..%d\n", pg::demo::count() - 1); return 2; }
    pg::demo::build(eng, g_inst.project(), demo);
    eng.sanitizeProject(g_inst.project());
#else
    std::fprintf(stderr, "this build has no pg::demo component\n");
    return 2;
#endif
  } else if (grooveName == "groove") {
    grooves::programGroove(g_inst.project());
  } else if (grooveName == "busy") {
    grooves::programBusy(g_inst.project());
  } else {
    std::fprintf(stderr, "unknown groove '%s'\n", grooveName.c_str());
    return 2;
  }
  if (seed) pg::engine_detail::setRngSeed(seed);
  if (solo >= 0 && solo < pg::kNumTracks)
    for (int t = 0; t < pg::kNumTracks; ++t) g_inst.project().tracks[t].mute = t == solo ? 0 : 1;

  pg::Command play{};
  play.type = pg::CmdType::Play;
  eng.post(play);

  const int bpm = g_inst.project().globals[pg::kGTempo];
  const double stepSamples = double(sr) * 15.0 / double(bpm);
  // Pattern length in steps (song mode / other lengths: render bars of 16 steps).
  const long playFrames = long(std::ceil(double(bars) * 16.0 * stepSamples / pg::kQuantum)) * pg::kQuantum;
  const long tailFrames = long(std::ceil(double(tail) * double(sr) / pg::kQuantum)) * pg::kQuantum;
  std::vector<int16_t> pcm(size_t(playFrames + tailFrames) * 2);
  const int kBlock = 256;
  long f = 0;
  while (f < playFrames) {
    const int n = int(std::min<long>(kBlock, playFrames - f));
    g_inst.process(pcm.data() + 2 * f, n);
    f += n;
  }
  if (tailFrames) {
    pg::Command stop{};
    stop.type = pg::CmdType::Stop;
    eng.post(stop);
    while (f < playFrames + tailFrames) {
      const int n = int(std::min<long>(kBlock, playFrames + tailFrames - f));
      g_inst.process(pcm.data() + 2 * f, n);
      f += n;
    }
  }

  int peak = 0;
  long lrDiff = 0;
  for (size_t i = 0; i < pcm.size(); i += 2) {
    peak = std::max(peak, std::abs(int(pcm[i])));
    peak = std::max(peak, std::abs(int(pcm[i + 1])));
    if (pcm[i] != pcm[i + 1]) ++lrDiff;
  }
  mkdirs(out);
  if (!wav::write(out.c_str(), pcm, int(sr), 2)) { std::fprintf(stderr, "cannot write %s\n", out.c_str()); return 1; }
  std::printf("%s: %d bars at %d BPM, %.0f Hz, %s, %.2f s, peak %.2f dBFS, L!=R frames %ld, engine faults %u\n",
              out.c_str(), bars, bpm, double(sr), mono ? "mono device" : "stereo",
              double(pcm.size() / 2) / double(sr), 20.0 * std::log10(std::max(1, peak) / 32767.0), lrDiff,
              pg::engine_detail::faultCount());
  return 0;
}
