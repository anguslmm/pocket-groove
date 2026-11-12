// desktop_audio_ref: checks a live recording of the desktop app against the
// offline engine (test_desktop_app, host/app/tests/desktop_check.py).
//
//   desktop_audio_ref --wav REC.wav --start-frame N --seconds S
//                     [--demo D | --project FILE.pgv] [--sr 44100|32000] [--mono]
//
// The recording's frame i is engine frame N + i (the app logs N when F9
// starts the recording). The app sat stopped from its first callback until
// SPACE, and the engine starts the transport at the start of the quantum in
// which it drains the Play command; which quantum that is depends on the
// wall clock, so it is searched: for each candidate quantum q just before the
// first non-silent sample, the reference is "init, load, render q frames
// stopped, post Play, render". The engine is deterministic and its output
// does not depend on how the frames are split into callbacks, so the right q
// must reproduce the recording BIT FOR BIT, idle prefix included, for S
// seconds after q. Exit 0 on an exact match, 1 otherwise (with the closest
// candidate's statistics).
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "pg/demo/demo.h"
#include "pg/instrument/instrument.h"
#include "pg/io/project_io.h"

namespace {
pg::Instrument g_inst;
pg::Project g_loaded;

bool readWav(const char* path, std::vector<int16_t>* pcm, int* rate, int* ch) {
  FILE* f = std::fopen(path, "rb");
  if (!f) return false;
  std::vector<uint8_t> b;
  uint8_t tmp[65536];
  size_t n;
  while ((n = std::fread(tmp, 1, sizeof(tmp), f)) > 0) b.insert(b.end(), tmp, tmp + n);
  std::fclose(f);
  if (b.size() < 44 || std::memcmp(b.data(), "RIFF", 4) || std::memcmp(b.data() + 8, "WAVE", 4)) return false;
  size_t p = 12;
  int bits = 0;
  while (p + 8 <= b.size()) {
    const uint32_t len = uint32_t(b[p + 4]) | uint32_t(b[p + 5]) << 8 | uint32_t(b[p + 6]) << 16 | uint32_t(b[p + 7]) << 24;
    if (!std::memcmp(b.data() + p, "fmt ", 4)) {
      *ch = b[p + 10] | b[p + 11] << 8;
      *rate = int(uint32_t(b[p + 12]) | uint32_t(b[p + 13]) << 8 | uint32_t(b[p + 14]) << 16 | uint32_t(b[p + 15]) << 24);
      bits = b[p + 22] | b[p + 23] << 8;
    } else if (!std::memcmp(b.data() + p, "data", 4)) {
      const size_t avail = b.size() - p - 8 < len ? b.size() - p - 8 : len;
      pcm->resize(avail / 2);
      std::memcpy(pcm->data(), b.data() + p + 8, pcm->size() * 2);
      return bits == 16;
    }
    p += 8 + len + (len & 1);
  }
  return false;
}

bool loadPgv(const char* path, pg::Project& dest) {
  FILE* f = std::fopen(path, "rb");
  if (!f) return false;
  std::vector<uint8_t> buf;
  uint8_t tmp[4096];
  size_t n;
  while ((n = std::fread(tmp, 1, sizeof(tmp), f)) > 0) buf.insert(buf.end(), tmp, tmp + n);
  std::fclose(f);
  return pg::io::decodeFromBuffer(buf.data(), buf.size(), dest) == pg::io::Status::Ok;
}
}  // namespace

int main(int argc, char** argv) {
  std::string wavPath, project;
  long long start = -1;
  double seconds = 2.0;
  int demo = 0, sr = 44100;
  bool mono = false;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() { return i + 1 < argc ? argv[++i] : ""; };
    if (a == "--wav") wavPath = next();
    else if (a == "--start-frame") start = std::atoll(next());
    else if (a == "--seconds") seconds = std::atof(next());
    else if (a == "--demo") demo = std::atoi(next());
    else if (a == "--project") project = next();
    else if (a == "--sr") sr = std::atoi(next());
    else if (a == "--mono") mono = true;
    else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
  }
  std::vector<int16_t> rec;
  int rate = 0, ch = 0;
  if (wavPath.empty() || start < 0 || !readWav(wavPath.c_str(), &rec, &rate, &ch) || ch != 2 || rate != sr) {
    std::fprintf(stderr, "need --wav (16-bit stereo at --sr) and --start-frame\n");
    return 2;
  }
  if (!project.empty() && !loadPgv(project.c_str(), g_loaded)) {
    std::fprintf(stderr, "cannot load %s\n", project.c_str());
    return 2;
  }
  const long long recFrames = (long long)(rec.size() / 2);
  long long first = -1;
  long long peak = 0;
  for (long long i = 0; i < recFrames; ++i) {
    const int l = std::abs(int(rec[size_t(2 * i)])), r = std::abs(int(rec[size_t(2 * i + 1)]));
    if ((l || r) && first < 0) first = i;
    if (l > peak) peak = l;
    if (r > peak) peak = r;
  }
  if (first < 0) {
    std::printf("recording is silent (%lld frames)\n", recFrames);
    return 1;
  }
  const long long firstAbs = start + first;
  const long long cmpFrames = (long long)(seconds * sr) / pg::kQuantum * pg::kQuantum;
  std::printf("recording: %lld frames from engine frame %lld, first sound at engine frame %lld, peak %.1f dBFS\n",
              recFrames, start, firstAbs, 20.0 * std::log10(double(peak) / 32767.0));
  long long bestQ = -1, bestDiff = -1;
  int bestMax = 0;
  const long long q0 = firstAbs - firstAbs % pg::kQuantum;
  for (long long q = q0; q >= 0 && q >= q0 - 8 * pg::kQuantum; q -= pg::kQuantum) {
    const long long end = q + cmpFrames;
    if (end > start + recFrames) {
      std::fprintf(stderr, "recording too short for %.2f s after the start\n", seconds);
      return 2;
    }
    g_inst.init(float(sr), mono);
    pg::Engine& eng = g_inst.engine();
    if (!project.empty()) {
      eng.suspend();
      std::memcpy(&g_inst.project(), &g_loaded, sizeof(pg::Project));
      eng.sanitizeProject(g_inst.project());
      eng.resume();
    } else {
      pg::demo::build(eng, g_inst.project(), demo);
      eng.sanitizeProject(g_inst.project());
    }
    std::vector<int16_t> ref(size_t(end) * 2);
    for (long long f = 0; f < q; f += pg::kQuantum) g_inst.process(ref.data() + 2 * f, pg::kQuantum);
    pg::Command play{};
    play.type = pg::CmdType::Play;
    eng.post(play);
    for (long long f = q; f < end; f += 256) {
      const int n = int(end - f < 256 ? end - f : 256);
      g_inst.process(ref.data() + 2 * f, n);   // n is a multiple of 16 except at the end
    }
    long long diff = 0;
    int maxd = 0;
    for (long long f = start; f < end; ++f)
      for (int c = 0; c < 2; ++c) {
        const int d = std::abs(int(ref[size_t(2 * f + c)]) - int(rec[size_t(2 * (f - start) + c)]));
        if (d) ++diff;
        if (d > maxd) maxd = d;
      }
    if (bestDiff < 0 || diff < bestDiff) {
      bestDiff = diff;
      bestQ = q;
      bestMax = maxd;
    }
    if (diff == 0) break;
  }
  std::printf("best start quantum: engine frame %lld (%lld frames after the first callback): %lld of %lld samples "
              "differ, max |diff| %d\n",
              bestQ, bestQ, bestDiff, (long long)((bestQ + cmpFrames - start) * 2), bestMax);
  if (bestDiff == 0) {
    std::printf("BIT-IDENTICAL to the offline engine for %.2f s of playback\n", seconds);
    return 0;
  }
  return 1;
}
