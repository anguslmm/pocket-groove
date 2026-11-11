// Stem bounce: renders a Project through the real instrument (stereo, the
// host build's engine) and hands out, block by block, the master output plus
// the engine's stem tap (pg/engine/stem_tap.h): each track's contribution to
// the bus, the delay and reverb returns and the pre-master bus.
//
// Used by host/tools/bounce.cpp (the CLI that writes the WAV files) and by
// host/itests/itest_bounce.cpp. Host code: heap and the standard library are
// fine here.
//
// STRUCTURE. A render pass is: load the project into the instrument, then
// play a TIMELINE of engine Commands, each due at an engine frame, through
// Engine::post, rendering in between. Today the timeline is just "queue the
// pattern, Play" at frame 0, and the engine itself ends the song
// (setEndAfterPatternPlays). A recorded performance (the timestamped Command
// log the UI sends through the one queue) can later be replayed by appending
// its commands to Pass::timeline; nothing else here needs to change.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "pg/engine/engine_hooks.h"
#include "pg/engine/stem_tap.h"
#include "pg/instrument/instrument.h"

namespace bounce {

constexpr int kTracks = pg::kNumTracks;

// What to render.
struct Options {
  int pattern = -1;        // -1: the song chain if the project has one, else pattern 0
  int loops = 1;           // pattern mode: how many times the pattern plays
  float tailSec = 8.0f;    // maximum FX tail after the song ends (trimmed once it has decayed)
  float sampleRate = 44100.0f;
  uint32_t seed = 0;       // step-probability RNG seed, 0 = the engine's default (as on the device)
};

struct TimedCommand {
  long frame;              // engine frame (from the start of the pass) at which to post it
  pg::Command cmd;
};

// One render of the project. Every pass of a bounce plays the same timeline
// from a freshly initialised engine with the same seed, so sequencing
// (probability, ratchets, ducker triggers) is identical in all of them.
struct Pass {
  bool noDuck = false;     // ducker source Off (no track or FX ducking)
  uint32_t sendMask = 0xFFu;   // tracks that feed the delay / reverb (stem_tap.h)
};

// One block of output from a pass. Pointers are valid during the callback only.
struct Block {
  int pass;
  long frame;              // first frame of this block
  int n;                   // frames
  const int16_t* mix;      // interleaved stereo int16: the instrument's output
  const float* track[kTracks][2];
  const float* delay[2];
  const float* reverb[2];
  const float* pre[2];     // pre-master bus
};

struct Plan {
  bool song = false;
  int pattern = 0;         // pattern mode: the pattern; song mode: unused
  int plays = 0;           // pattern plays until the end (song: sum of slot repeats)
  long steps = 0;          // 16th steps until the end
  int bpm = 120;
  long songFrames = 0;     // estimate from the tempo (the engine reports the exact end)
  std::vector<TimedCommand> timeline;
};

// Decides song vs. pattern and builds the timeline. false + err if there is
// nothing to play.
inline bool makePlan(const pg::Project& p, const Options& o, Plan& plan, std::string* err) {
  plan = Plan{};
  plan.bpm = p.globals[pg::kGTempo];
  if (plan.bpm < 1) plan.bpm = 120;
  if (o.pattern >= pg::kNumPatterns) {
    if (err) *err = "pattern out of range (1.." + std::to_string(pg::kNumPatterns) + ")";
    return false;
  }
  auto patLen = [&](int pt) { return long(p.patterns[pt].length ? p.patterns[pt].length : 16); };
  if (o.pattern < 0 && p.song[0].repeats > 0) {
    plan.song = true;
    for (int s = 0; s < pg::kSongSlots && p.song[s].repeats; ++s) {
      const int pt = p.song[s].pattern < pg::kNumPatterns ? p.song[s].pattern : pg::kNumPatterns - 1;
      plan.plays += p.song[s].repeats;
      plan.steps += patLen(pt) * p.song[s].repeats;
    }
  } else {
    plan.song = false;
    plan.pattern = o.pattern < 0 ? 0 : o.pattern;
    if (o.loops < 1) {
      if (err) *err = "--loops must be >= 1";
      return false;
    }
    plan.plays = o.loops;
    plan.steps = patLen(plan.pattern) * o.loops;
  }
  plan.songFrames = long(std::ceil(double(plan.steps) * double(o.sampleRate) * 15.0 / double(plan.bpm)));
  pg::Command c{};
  c.type = pg::CmdType::SetSongMode;
  c.a = 0;
  c.c = plan.song ? 1 : 0;
  plan.timeline.push_back({0, c});
  if (!plan.song) {
    c = pg::Command{};
    c.type = pg::CmdType::QueuePattern;   // stopped: switches at once
    c.a = uint8_t(plan.pattern);
    plan.timeline.push_back({0, c});
  }
  c = pg::Command{};
  c.type = pg::CmdType::Play;
  plan.timeline.push_back({0, c});
  return true;
}

struct PassResult {
  long endFrame = -1;      // where the sequencer stopped (engine-reported)
  long frames = 0;         // frames rendered (endFrame + tail)
  uint32_t faults = 0;
  uint64_t hash = 1469598103934665603ull;   // FNV-1a over the int16 mix and every tap value
  bool overflow = false;
};

// Renders one pass: song + up to tailSec seconds. `inst` must be the
// program's only Instrument. Calls onBlock for every block.
inline bool renderPass(pg::Instrument& inst, const pg::Project& project, const Options& o, const Plan& plan,
                       const Pass& pass, int passIndex, const std::function<void(const Block&)>& onBlock,
                       PassResult& res, std::string* err) {
  using namespace pg;
  res = PassResult{};
  inst.init(o.sampleRate, /*monoOutput=*/false);
  Engine& eng = inst.engine();
  eng.suspend();                          // not running yet: returns at once
  std::memcpy(&inst.project(), &project, sizeof(Project));
  if (pass.noDuck) inst.project().globals[kGDuckSource] = 0;   // Off: the ducker never triggers
  eng.sanitizeProject(inst.project());
  eng.resume();
  if (o.seed) engine_detail::setRngSeed(o.seed);
  engine_detail::setEndAfterPatternPlays(plan.plays);

  constexpr int kBlock = 256;
  static_assert(kBlock % kQuantum == 0, "block must be whole quanta");
  std::vector<float> buf(size_t(kBlock) * (2 * kTracks + 6), 0.0f);
  engine_detail::StemTap tap;
  float* b = buf.data();
  for (int t = 0; t < kTracks; ++t)
    for (int c = 0; c < 2; ++c) { tap.track[t][c] = b; b += kBlock; }
  for (int c = 0; c < 2; ++c) { tap.delayRet[c] = b; b += kBlock; }
  for (int c = 0; c < 2; ++c) { tap.reverbRet[c] = b; b += kBlock; }
  for (int c = 0; c < 2; ++c) { tap.preMaster[c] = b; b += kBlock; }
  tap.capacity = kBlock;
  tap.sendMask = pass.sendMask;
  engine_detail::setStemTap(&tap);

  const long tailFrames = long(std::ceil(double(o.tailSec) * double(o.sampleRate)));
  // Safety stop if the engine never reports the end (it always should).
  const long hardCap = plan.songFrames + plan.songFrames / 8 + long(o.sampleRate) * 2 + tailFrames;
  std::vector<int16_t> pcm(size_t(kBlock) * 2);
  size_t next = 0;
  long f = 0;
  auto hashBytes = [&](const void* p, size_t n) {
    const uint8_t* x = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; ++i) { res.hash ^= x[i]; res.hash *= 1099511628211ull; }
  };
  for (;;) {
    while (next < plan.timeline.size() && plan.timeline[next].frame <= f) eng.post(plan.timeline[next++].cmd);
    const long end = engine_detail::endedAtFrame();
    if (end >= 0 && f >= end + tailFrames) break;
    if (f >= hardCap) {
      engine_detail::setStemTap(nullptr);
      engine_detail::setEndAfterPatternPlays(-1);
      if (err) *err = "the sequencer did not reach the end of the song";
      return false;
    }
    // Stop the block at the next timeline command (quantum-aligned) or the end of the tail.
    long n = kBlock;
    if (next < plan.timeline.size()) {
      const long due = (plan.timeline[next].frame + kQuantum - 1) / kQuantum * kQuantum;
      if (due > f) n = std::min(n, due - f);
    }
    if (end >= 0) n = std::min(n, (end + tailFrames + kQuantum - 1) / kQuantum * kQuantum - f);
    if (n <= 0) n = kQuantum;
    tap.pos = 0;
    inst.process(pcm.data(), int(n));
    Block blk{};
    blk.pass = passIndex;
    blk.frame = f;
    blk.n = int(n);
    blk.mix = pcm.data();
    for (int t = 0; t < kTracks; ++t)
      for (int c = 0; c < 2; ++c) blk.track[t][c] = tap.track[t][c];
    for (int c = 0; c < 2; ++c) {
      blk.delay[c] = tap.delayRet[c];
      blk.reverb[c] = tap.reverbRet[c];
      blk.pre[c] = tap.preMaster[c];
    }
    hashBytes(pcm.data(), size_t(n) * 2 * sizeof(int16_t));
    hashBytes(buf.data(), buf.size() * sizeof(float));
    onBlock(blk);
    f += n;
  }
  engine_detail::setStemTap(nullptr);
  engine_detail::setEndAfterPatternPlays(-1);
  res.endFrame = engine_detail::endedAtFrame();
  res.frames = f;
  res.faults = engine_detail::faultCount();
  res.overflow = tap.overflow;
  return true;
}

// ---- stem layout -------------------------------------------------------------

enum class Bits { k16, k24, k32f };

struct Request {
  Options opt;
  bool wet = false;        // each track stem carries its own delay/reverb share; no return files
  bool noDuck = false;     // stems (not the mix) rendered with the ducker off
  Bits bits = Bits::k32f;
};

// One output file's contents: which pass, and which tap signals summed.
struct OutputSpec {
  std::string label;       // "kick", ..., "delay", "reverb", "mix"
  int pass = 0;
  bool mix = false;        // the int16 instrument output
  int track = -1;          // track contribution, or -1
  bool delay = false, reverb = false;
};

// Passes and outputs for a request. Pass 0 is always the real project (mix).
inline void layout(const Request& rq, std::vector<Pass>& passes, std::vector<OutputSpec>& outs) {
  static const char* kNames[kTracks] = {"kick", "snare", "clap", "closedhat", "openhat", "perc", "bass", "lead"};
  passes.clear();
  outs.clear();
  passes.push_back(Pass{});                       // 0: as the instrument plays it
  int stemPass = 0;
  if (rq.noDuck && !rq.wet) {
    Pass p;
    p.noDuck = true;
    passes.push_back(p);
    stemPass = 1;
  }
  for (int t = 0; t < kTracks; ++t) {
    OutputSpec o;
    o.label = kNames[t];
    o.track = t;
    if (rq.wet) {
      Pass p;
      p.noDuck = rq.noDuck;
      p.sendMask = 1u << t;
      passes.push_back(p);
      o.pass = int(passes.size()) - 1;
      o.delay = o.reverb = true;
    } else {
      o.pass = stemPass;
    }
    outs.push_back(o);
  }
  if (!rq.wet) {
    OutputSpec d;
    d.label = "delay";
    d.pass = stemPass;
    d.delay = true;
    outs.push_back(d);
    OutputSpec r;
    r.label = "reverb";
    r.pass = stemPass;
    r.reverb = true;
    outs.push_back(r);
  }
  OutputSpec m;
  m.label = "mix";
  m.pass = 0;
  m.mix = true;
  outs.push_back(m);
}

// Fills l/r (n frames) with an output's samples from a block of its pass.
inline void extract(const OutputSpec& o, const Block& b, float* l, float* r) {
  if (o.mix) {
    for (int i = 0; i < b.n; ++i) {
      l[i] = float(b.mix[2 * i]) * (1.0f / 32768.0f);
      r[i] = float(b.mix[2 * i + 1]) * (1.0f / 32768.0f);
    }
    return;
  }
  for (int i = 0; i < b.n; ++i) { l[i] = 0.0f; r[i] = 0.0f; }
  if (o.track >= 0)
    for (int i = 0; i < b.n; ++i) { l[i] += b.track[o.track][0][i]; r[i] += b.track[o.track][1][i]; }
  if (o.delay)
    for (int i = 0; i < b.n; ++i) { l[i] += b.delay[0][i]; r[i] += b.delay[1][i]; }
  if (o.reverb)
    for (int i = 0; i < b.n; ++i) { l[i] += b.reverb[0][i]; r[i] += b.reverb[1][i]; }
}

// ---- streaming WAV writer ----------------------------------------------------

class WavWriter {
 public:
  ~WavWriter() { if (f_) std::fclose(f_); }
  // frames must be known up front (the bounce measures first), so the header
  // is final when written.
  bool open(const std::string& path, int sampleRate, Bits bits, long frames) {
    f_ = std::fopen(path.c_str(), "wb");
    if (!f_) return false;
    bits_ = bits;
    const uint16_t ch = 2;
    const uint16_t bps = bits == Bits::k16 ? 16 : bits == Bits::k24 ? 24 : 32;
    const uint16_t fmtTag = bits == Bits::k32f ? 3 : 1;   // IEEE float / PCM
    const uint16_t align = uint16_t(ch * bps / 8);
    const uint32_t data = uint32_t(frames) * align;
    const bool fact = bits == Bits::k32f;
    const uint32_t fmtLen = fact ? 18 : 16;
    const uint32_t riff = 4 + (8 + fmtLen) + (fact ? 12 : 0) + 8 + data;
    put("RIFF"); put32(riff); put("WAVE");
    put("fmt "); put32(fmtLen); put16(fmtTag); put16(ch); put32(uint32_t(sampleRate));
    put32(uint32_t(sampleRate) * align); put16(align); put16(bps);
    if (fact) { put16(0); put("fact"); put32(4); put32(uint32_t(frames)); }
    put("data"); put32(data);
    return std::ferror(f_) == 0;
  }
  void write(const float* l, const float* r, int n, float gain) {
    std::vector<uint8_t> out;
    out.reserve(size_t(n) * 8);
    for (int i = 0; i < n; ++i) {
      sample(l[i] * gain, out);
      sample(r[i] * gain, out);
    }
    std::fwrite(out.data(), 1, out.size(), f_);
  }
  bool close() {
    if (!f_) return false;
    const bool ok = std::ferror(f_) == 0;
    const bool c = std::fclose(f_) == 0;
    f_ = nullptr;
    return ok && c;
  }

 private:
  void put(const char* s) { std::fwrite(s, 1, 4, f_); }
  void put16(uint32_t v) { const uint8_t b[2] = {uint8_t(v), uint8_t(v >> 8)}; std::fwrite(b, 1, 2, f_); }
  void put32(uint32_t v) {
    const uint8_t b[4] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)};
    std::fwrite(b, 1, 4, f_);
  }
  // TPDF dither (deterministic) for 16-bit only; 24-bit rounds; float is exact.
  float tpdf() {
    rng_ ^= rng_ << 13; rng_ ^= rng_ >> 17; rng_ ^= rng_ << 5;
    const float a = float(rng_ & 0xFFFF) * (1.0f / 65536.0f);
    rng_ ^= rng_ << 13; rng_ ^= rng_ >> 17; rng_ ^= rng_ << 5;
    const float b = float(rng_ & 0xFFFF) * (1.0f / 65536.0f);
    return a - b;
  }
  void sample(float x, std::vector<uint8_t>& out) {
    if (bits_ == Bits::k32f) {
      uint32_t u;
      std::memcpy(&u, &x, 4);
      for (int k = 0; k < 4; ++k) out.push_back(uint8_t(u >> (8 * k)));
      return;
    }
    const bool b16 = bits_ == Bits::k16;
    const float full = b16 ? 32768.0f : 8388608.0f;
    float v = x * full + (b16 ? tpdf() : 0.0f);
    v = std::floor(v + 0.5f);
    if (v > full - 1.0f) v = full - 1.0f;
    if (v < -full) v = -full;
    const int32_t s = int32_t(v);
    out.push_back(uint8_t(s));
    out.push_back(uint8_t(s >> 8));
    if (!b16) out.push_back(uint8_t(s >> 16));
  }
  FILE* f_ = nullptr;
  Bits bits_ = Bits::k32f;
  uint32_t rng_ = 0x12345678u;
};

}  // namespace bounce
