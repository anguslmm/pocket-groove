// Shared helpers for test_io_*: project generators and a tiny check macro.
#pragma once
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include "common/memfs.h"
#include "pg/io/project_io.h"
#include "pg/params.h"

namespace iot {

inline int g_fail = 0;
#define CHECK(c)                                                         \
  do {                                                                   \
    if (!(c)) {                                                          \
      ++iot::g_fail;                                                     \
      if (iot::g_fail < 30) std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); \
    }                                                                    \
  } while (0)
#define CHECK_ST(expr, want)                                                                         \
  do {                                                                                               \
    const pg::io::Status s_ = (expr);                                                                \
    if (s_ != (want)) {                                                                              \
      ++iot::g_fail;                                                                                 \
      if (iot::g_fail < 30)                                                                          \
        std::printf("FAIL %s:%d  %s -> %s (want %s)\n", __FILE__, __LINE__, #expr, pg::io::statusName(s_), \
                    pg::io::statusName(want));                                                       \
    }                                                                                                \
  } while (0)

inline int finish(const char* name) {
  if (g_fail) std::printf("%s: %d FAILURES\n", name, g_fail);
  else std::printf("%s: all passed\n", name);
  return g_fail ? 1 : 0;
}

struct Rng {
  uint32_t s;
  explicit Rng(uint32_t seed) : s(seed ? seed : 1) {}
  uint32_t next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
  uint32_t below(uint32_t n) { return next() % n; }
  uint8_t byte() { return uint8_t(next() >> 24); }
};

inline std::unique_ptr<pg::Project> newProject() { return std::unique_ptr<pg::Project>(new pg::Project); }

// Mirrors Engine::initProject (no engine dependency): plausible defaults.
inline void initProject(pg::Project& p) {
  using namespace pg;
  std::memset(&p, 0, sizeof(p));
  p.magic[0] = 'P'; p.magic[1] = 'G'; p.magic[2] = 'R'; p.magic[3] = 'V';
  p.version = kProjectVersion;
  std::memcpy(p.name, "INIT", 5);
  const uint8_t g[kGNumUsed] = {124, 0, 100, 1, 90, 10, 60, 64, 40, 5, 50, 64, 80, 70, 50, 64, 0, 20, 9, 1};
  std::memcpy(p.globals, g, sizeof(g));
  for (int t = 0; t < kNumTracks; ++t) {
    for (int i = 0; i < kSoundParams; ++i) p.tracks[t].params[i] = uint8_t((t * 37 + i * 11) % 128);
    p.tracks[t].params[kMixLevel] = 100;
    p.tracks[t].params[kMixPan] = 64;
    p.tracks[t].params[kMixDuck] = t >= kTrackBass ? 90 : 0;
  }
  for (int pt = 0; pt < kNumPatterns; ++pt) {
    p.patterns[pt].length = 16;
    for (int t = 0; t < kNumTracks; ++t) {
      p.patterns[pt].tracks[t].length = 16;
      for (int s = 0; s < kMaxSteps; ++s) p.patterns[pt].tracks[t].steps[s] = kDefaultStep;
    }
  }
}

inline pg::Step musicalStep(Rng& r, int track) {
  pg::Step s = pg::kDefaultStep;
  s.setFlags(pg::kStepOn);
  if (r.below(4) == 0) s.addFlags(pg::kStepAccent);
  if (track >= pg::kTrackBass && r.below(5) == 0) s.addFlags(pg::kStepSlide);
  if (r.below(10) == 0) s.addFlags(uint8_t(r.below(4) << pg::kStepRatchetShift));
  s.note = track >= pg::kTrackBass ? uint8_t(33 + r.below(24)) : 60;
  s.vel = uint8_t(60 + r.below(68));
  if (r.below(5) == 0) s.setProb(uint8_t(30 + r.below(70)));
  if (track == pg::kTrackLead && r.below(2)) s.setChord(uint8_t(r.below(pg::kChordNum)));
  if (r.below(8) == 0) { s.setLock(uint8_t(1 + r.below(32)), r.byte() & 0x7F); }
  return s;
}

// 8 patterns, ~40 % of the tracks used in each, 16-step tracks with musical content.
inline void typicalProject(pg::Project& p, uint32_t seed = 7) {
  initProject(p);
  Rng r(seed);
  std::memcpy(p.name, "ACIDJAM", 8);
  for (int pt = 0; pt < 8; ++pt)
    for (int t = 0; t < pg::kNumTracks; ++t) {
      if (r.below(100) >= 40) continue;
      pg::TrackPattern& tp = p.patterns[pt].tracks[t];
      for (int s = 0; s < 16; ++s)
        if (r.below(100) < (t == pg::kTrackHatClosed ? 90 : 40)) tp.steps[s] = musicalStep(r, t);
    }
  for (int i = 0; i < 8; ++i) { p.song[i].pattern = uint8_t(i); p.song[i].repeats = 2; }
  p.globals[pg::kGTempo] = 126;
  // instruments on most tracks, with a note's own macro value on some steps
  for (int t = 0; t < pg::kNumTracks; ++t) {
    if (t == pg::kTrackPerc) continue;   // one CLASSIC track
    pg::setInstrumentId(p.tracks[t], uint16_t(0x1234 + 0x0F0F * t));
    for (int m = 0; m < pg::kTrackMacros; ++m) p.tracks[t].macro[m] = r.byte();
    pg::Step& s = p.patterns[0].tracks[t].steps[3];
    s.addFlags(pg::kStepOn);
    s.setMacroValue(t % pg::kTrackMacros, r.byte());
  }
}

// Every pattern, every track, all 64 steps with content (unlikely in practice).
inline void denseProject(pg::Project& p, uint32_t seed = 11) {
  initProject(p);
  Rng r(seed);
  for (int pt = 0; pt < pg::kNumPatterns; ++pt) {
    p.patterns[pt].length = 64;
    for (int t = 0; t < pg::kNumTracks; ++t) {
      p.patterns[pt].tracks[t].length = 64;
      for (int s = 0; s < pg::kMaxSteps; ++s) p.patterns[pt].tracks[t].steps[s] = musicalStep(r, t);
    }
  }
}

// Fully random bytes, except the magic/version the saver insists on.
inline void randomProject(pg::Project& p, uint32_t seed = 13) {
  Rng r(seed);
  uint8_t* b = reinterpret_cast<uint8_t*>(&p);
  for (size_t i = 0; i < sizeof(p); ++i) b[i] = r.byte();
  p.magic[0] = 'P'; p.magic[1] = 'G'; p.magic[2] = 'R'; p.magic[3] = 'V';
  p.version = pg::kProjectVersion;
}

inline bool same(const pg::Project& a, const pg::Project& b) { return std::memcmp(&a, &b, sizeof(a)) == 0; }
inline bool allZero(const pg::Project& a) {
  const uint8_t* b = reinterpret_cast<const uint8_t*>(&a);
  for (size_t i = 0; i < sizeof(a); ++i)
    if (b[i]) return false;
  return true;
}
inline void sentinel(pg::Project& p) { std::memset(&p, 0xA5, sizeof(p)); }
inline bool isSentinel(const pg::Project& a) {
  const uint8_t* b = reinterpret_cast<const uint8_t*>(&a);
  for (size_t i = 0; i < sizeof(a); ++i)
    if (b[i] != 0xA5) return false;
  return true;
}

inline std::vector<uint8_t> encode(const pg::Project& p, const char* stem = "TEST") {
  std::vector<uint8_t> out(sizeof(pg::Project) + 2048);
  size_t n = 0;
  if (pg::io::encodeToBuffer(p, stem, out.data(), out.size(), &n) != pg::io::Status::Ok) return {};
  out.resize(n);
  return out;
}

inline double nowMs() {
  using namespace std::chrono;
  return double(duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count()) * 1e-6;
}

}  // namespace iot
