// Pocket Groove -- hardware smoke test for the M5Stack Cardputer-Adv.
//
// One flash answers the open hardware questions (docs/hardware-notes.md
// section 10) using the same platform layer (src/platform) the instrument
// will use. Five pages, every result also printed to USB serial (115200).
//
//   Fn+1..5  go to page        G0 (or Tab outside the Keys page)  next page
//   Fn+Enter print the full report to serial (Enter on the Report page too)
//   Serial:  type a key's character to press it, '>' next page, '!' report
//
// Pages: 1 KEYS  2 AUDIO  3 CPU  4 SYSTEM  5 REPORT. See docs/BRINGUP.md.
//
// Threads:  UI = Arduino loop() on core 1, priority 1 (all I2C, files, display)
//           audio = plat::audio task (core 0 by default, priority 20): render()
//           keys  = plat::keys task (core 1, priority 10): onKeyListener()
//           jobs  = short-lived tasks on core 1, priority 1 (file tests, scans)
// This file includes only platform headers: no Arduino / M5 / ESP-IDF.
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "../platform/audio_out.h"
#include "../platform/display.h"
#include "../platform/keys.h"
#include "../platform/storage.h"
#include "../platform/system.h"
#include "dsp_bench.h"
#include "gfx.h"
#include "pg/platform.h"

using namespace smoke;
namespace audio = plat::audio;
namespace keys = plat::keys;
namespace sys = plat::sys;
namespace disp = plat::display;
namespace storage = plat::storage;

namespace {

constexpr const char* kVersion = "smoke-1";

// ============================================================================
// Test signals (rendered on the audio task)
// ============================================================================
enum Signal : uint8_t {
  kSilence = 0, kSine440, kSineLeft, kSineRight, kSweep, kSine55, kSquare1k, kClick120, kBlip, kNumSignals
};
const char* signalName(uint8_t s) {
  static const char* const n[kNumSignals] = {
    "silence", "440Hz sine L+R", "440Hz LEFT slot only", "440Hz RIGHT slot only",
    "log sweep 20Hz-20kHz/10s", "55Hz sine L+R", "1kHz square -12dBFS", "click 120BPM", "blip on SPACE"};
  return s < kNumSignals ? n[s] : "?";
}

constexpr float kLevel = 0.25118864f;   // -12 dBFS
constexpr int kLutBits = 10, kLutSize = 1 << kLutBits;
float g_sinLut[kLutSize + 1];

std::atomic<uint8_t> g_signal{kSilence};
volatile float g_fs = 44100.0f;          // rate the generator renders at (set before audio::begin)
volatile float g_sweepHz = 0.0f;         // current sweep frequency, for the UI

// blip latency (keys task -> audio task)
std::atomic<uint32_t> g_blipReqSeq{0};
volatile uint32_t g_blipReqTUs = 0;      // INT time of the key event
volatile uint32_t g_blipLastUs = 0, g_blipMaxUs = 0, g_blipCount = 0;
volatile float    g_blipSumUs = 0.0f;
volatile uint32_t g_blipMinUs = 0xFFFFFFFFu;

inline float lutSin(float ph) {          // ph in [0, 1)
  const float x = ph * static_cast<float>(kLutSize);
  const int i = static_cast<int>(x);
  const float f = x - static_cast<float>(i);
  return g_sinLut[i] + f * (g_sinLut[i + 1] - g_sinLut[i]);
}
inline int16_t toS16(float v) {
  v *= 32767.0f;
  if (v > 32767.0f) v = 32767.0f;
  if (v < -32768.0f) v = -32768.0f;
  return static_cast<int16_t>(v);
}

struct Gen {
  uint8_t  mode = 0xFF;
  float    fs = 0;
  float    ph = 0, inc = 0, sweepMul = 1, sweepTop = 1, sweepStart = 0;
  uint32_t clickPeriod = 1, clickPos = 0, beat = 0;
  float    env = 0, envMul = 1, clickInc = 0, clickPh = 0;
  uint32_t blipSeq = 0, blipLeft = 0, blipLen = 1;
  float    blipPh = 0, blipInc = 0;
};
Gen g_gen;   // audio task only

void render(int16_t* out, int frames, void*) {
  Gen& g = g_gen;
  const float fs = g_fs;
  const uint8_t mode = g_signal.load(std::memory_order_relaxed);
  if (mode != g.mode || fs != g.fs) {    // (re)start the signal cleanly (new signal or new rate)
    g.mode = mode;
    g.fs = fs;
    g.ph = 0;
    g.inc = (mode == kSine55 ? 55.0f : mode == kSquare1k ? 1000.0f : 440.0f) / fs;
    g.sweepStart = 20.0f / fs;
    g.sweepTop = 20000.0f / fs;
    g.sweepMul = expf(logf(1000.0f) / (10.0f * fs));
    if (mode == kSweep) g.inc = g.sweepStart;
    g.clickPeriod = static_cast<uint32_t>(fs * 0.5f);
    g.clickPos = 0;
    g.beat = 0;
    g.env = 0;
    g.envMul = expf(-1.0f / (0.004f * fs));
    g.blipLeft = 0;
    g.blipLen = static_cast<uint32_t>(0.05f * fs);
    g.blipInc = 1000.0f / fs;
    g.blipSeq = g_blipReqSeq.load(std::memory_order_acquire);
  }

  bench::process(frames);                // synthetic CPU load (no output)

  switch (mode) {
    case kSine440: case kSineLeft: case kSineRight: case kSine55: {
      const bool l = (mode != kSineRight), r = (mode != kSineLeft);
      for (int i = 0; i < frames; ++i) {
        const int16_t v = toS16(kLevel * lutSin(g.ph));
        g.ph += g.inc;
        if (g.ph >= 1.0f) g.ph -= 1.0f;
        out[2 * i] = l ? v : 0;
        out[2 * i + 1] = r ? v : 0;
      }
      break;
    }
    case kSweep:
      for (int i = 0; i < frames; ++i) {
        const int16_t v = toS16(kLevel * lutSin(g.ph));
        g.ph += g.inc;
        if (g.ph >= 1.0f) g.ph -= 1.0f;
        g.inc *= g.sweepMul;
        if (g.inc > g.sweepTop) g.inc = g.sweepStart;
        out[2 * i] = out[2 * i + 1] = v;
      }
      g_sweepHz = g.inc * fs;
      break;
    case kSquare1k:
      for (int i = 0; i < frames; ++i) {
        const int16_t v = toS16(g.ph < 0.5f ? kLevel : -kLevel);
        g.ph += g.inc;
        if (g.ph >= 1.0f) g.ph -= 1.0f;
        out[2 * i] = out[2 * i + 1] = v;
      }
      break;
    case kClick120:
      for (int i = 0; i < frames; ++i) {
        if (g.clickPos == 0) {
          const bool accent = (g.beat % 4) == 0;
          g.env = accent ? 0.5f : 0.3f;
          g.clickInc = (accent ? 3000.0f : 2000.0f) / fs;
          g.clickPh = 0;
          ++g.beat;
        }
        if (++g.clickPos >= g.clickPeriod) g.clickPos = 0;
        float v = 0;
        if (g.env > 1e-4f) {
          v = g.env * lutSin(g.clickPh);
          g.clickPh += g.clickInc;
          if (g.clickPh >= 1.0f) g.clickPh -= 1.0f;
          g.env *= g.envMul;
        }
        out[2 * i] = out[2 * i + 1] = toS16(v);
      }
      break;
    case kBlip: {
      const uint32_t seq = g_blipReqSeq.load(std::memory_order_acquire);
      if (seq != g.blipSeq) {
        g.blipSeq = seq;
        g.blipLeft = g.blipLen;
        g.blipPh = 0;
        const uint32_t lat = static_cast<uint32_t>(sys::micros()) - g_blipReqTUs;
        g_blipLastUs = lat;
        if (lat > g_blipMaxUs) g_blipMaxUs = lat;
        if (lat < g_blipMinUs) g_blipMinUs = lat;
        g_blipSumUs = g_blipSumUs + static_cast<float>(lat);
        g_blipCount = g_blipCount + 1;
      }
      for (int i = 0; i < frames; ++i) {
        float v = 0;
        if (g.blipLeft) {
          const uint32_t done = g.blipLen - g.blipLeft;
          float e = 1.0f;
          if (done < 24) e = static_cast<float>(done) / 24.0f;              // ~0.5 ms attack
          if (g.blipLeft < 96) e = static_cast<float>(g.blipLeft) / 96.0f;   // ~2 ms release
          v = kLevel * e * lutSin(g.blipPh);
          g.blipPh += g.blipInc;
          if (g.blipPh >= 1.0f) g.blipPh -= 1.0f;
          --g.blipLeft;
        }
        out[2 * i] = out[2 * i + 1] = toS16(v);
      }
      break;
    }
    default:
      memset(out, 0, static_cast<size_t>(frames) * 2 * sizeof(int16_t));
      break;
  }
}

// ============================================================================
// Key ids (pg::Key::id = row * 14 + col)
// ============================================================================
constexpr uint8_t kKeyDel = 13, kKeyTab = 14, kKeyFn = 28, kKeyShift = 29, kKeyEnter = 41,
                  kKeyCtrl = 42, kKeySpace = 55;

// keys task: tiny and non-blocking
void onKeyListener(const keys::Event& e, void*) {
  if (e.ev.down && e.ev.key == kKeySpace && g_signal.load(std::memory_order_relaxed) == kBlip) {
    g_blipReqTUs = e.tIntUs;
    g_blipReqSeq.fetch_add(1, std::memory_order_release);
  }
}

char keyChar(uint8_t id) {
  const char* l = keys::legend(id);
  if (id == kKeySpace) return ' ';
  if (id == kKeyEnter) return '\n';
  if (id == kKeyTab) return '\t';
  if (id == kKeyDel) return '\b';
  return (l[0] && !l[1]) ? l[0] : 0;
}

// ============================================================================
// App state (UI thread unless noted)
// ============================================================================
enum Page : uint8_t { kPageKeys = 0, kPageAudio, kPageCpu, kPageSystem, kPageReport, kNumPages };
const char* const kPageName[kNumPages] = {"KEYS", "AUDIO", "CPU", "SYSTEM", "REPORT"};

pg::Framebuffer g_fb;
Canvas g_cv(&g_fb);
uint8_t g_page = kPageKeys;
bool g_fullRate = false;
uint32_t g_lastFrameMs = 0;
char g_msg[48] = "";
uint32_t g_msgUntilMs = 0;

sys::BootInfo g_boot;
sys::ChipInfo g_chip;
sys::HeapInfo g_heapBoot;

// ---- audio ------------------------------------------------------------------
audio::Config g_acfg;
bool g_audioOk = false;
char g_audioErr[64] = "";

struct RunRecord {
  char     label[28];
  uint32_t seconds, blocks, underruns, late, writeErrors, overloadYields;
  float    avgUs, maxUs, maxGapUs, cpu;
  bool     underrunsValid;
};
constexpr int kMaxRuns = 12;
RunRecord g_runs[kMaxRuns];
int g_numRuns = 0;
uint32_t g_runStartMs = 0;

void configLabel(char* buf, size_t n, const audio::Config& c) {
  snprintf(buf, n, "%s %lu c%u dma%u", c.backend == audio::Backend::DirectI2S ? "direct" : "m5spk",
           static_cast<unsigned long>(c.sampleRate), c.core, c.dmaBufCount);
}

// Saves the stats of the configuration that just ran (if it ran long enough).
void archiveRun(const char* suffix = nullptr) {
  if (!g_audioOk) return;
  const audio::Stats st = audio::stats();
  if (st.blocks < 50) return;
  if (g_numRuns == kMaxRuns) { memmove(g_runs, g_runs + 1, sizeof(RunRecord) * (kMaxRuns - 1)); --g_numRuns; }
  RunRecord& r = g_runs[g_numRuns++];
  char lab[28];
  configLabel(lab, sizeof(lab), g_acfg);
  snprintf(r.label, sizeof(r.label), "%s%s%s", lab, suffix ? " " : "", suffix ? suffix : "");
  r.seconds = (sys::millis() - g_runStartMs) / 1000;
  r.blocks = st.blocks; r.underruns = st.underruns; r.late = st.lateBlocks;
  r.writeErrors = st.writeErrors; r.overloadYields = st.overloadYields;
  r.avgUs = st.avgUs; r.maxUs = st.maxUs; r.maxGapUs = st.maxPeriodUs; r.cpu = st.cpuPercent;
  r.underrunsValid = st.underrunsValid;
}

void resetAudioStats() {
  audio::resetStats();
  g_runStartMs = sys::millis();
}

// After a benchmark change the next audio block recomputes coefficients
// (~1 ms once); reset a little later so that one block does not end up as "max".
uint32_t g_resetDueMs = 0;
void resetAudioStatsSoon() { g_resetDueMs = (sys::millis() + 150) | 1u; }

void flash(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void flash(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(g_msg, sizeof(g_msg), fmt, ap);
  va_end(ap);
  g_msgUntilMs = sys::millis() + 2500;
}

// serial printf (never blocks for long, see sys::serialWrite)
void sp(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void sp(const char* fmt, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  sys::serialWrite(buf);
}

bool startAudio(const audio::Config& cfg) {
  archiveRun();
  g_fs = static_cast<float>(cfg.sampleRate);
  // the benchmark keeps its kernel; re-derive its coefficients for the new rate
  bench::configure(bench::activeKind(), bench::activeInstances(), g_fs);
  if (audio::running()) g_audioOk = audio::reconfigure(cfg);
  else g_audioOk = audio::begin(cfg, render, nullptr);
  g_acfg = audio::config();
  strncpy(g_audioErr, audio::lastError(), sizeof(g_audioErr) - 1);
  g_audioErr[sizeof(g_audioErr) - 1] = 0;
  g_runStartMs = sys::millis();
  char lab[28];
  configLabel(lab, sizeof(lab), g_acfg);
  sp("[audio] start %s vol=0x%02X -> %s%s%s codec=%s\n", lab, g_acfg.volumeReg, g_audioOk ? "OK" : "FAILED",
     g_audioErr[0] ? " / " : "", g_audioErr, audio::codecPresent() ? "ACK" : "NO-ACK");
  return g_audioOk;
}

// ---- keys -------------------------------------------------------------------
uint64_t g_held = 0, g_seenDown = 0, g_seenUp = 0;
int g_maxHeld = 0;
uint32_t g_keyEvents = 0;
keys::Event g_lastEv = {};
bool g_haveLastEv = false;
uint32_t g_lastDownTInt = 0;
uint8_t g_lastDownKey = 0xFF;
// matrix-rectangle (ghosting) observations
uint32_t g_rectCount = 0, g_ghostSuspects = 0;
uint8_t g_lastRect[4] = {0xFF, 0xFF, 0xFF, 0xFF};
bool g_lastRectSuspect = false;

inline bool isHeld(uint64_t m, uint8_t k) { return (m >> k) & 1ull; }
int popcount64(uint64_t v) { return __builtin_popcountll(v); }

// After key `k` went down: does it complete a rectangle in the TCA8418 7x8
// matrix with three other held keys? If yes, one of the four may be a ghost.
void checkRectangle(uint8_t k, uint32_t tInt) {
  if (k >= pg::kNumKeys) return;
  const uint8_t kr = keys::tcaRowOf(k), kc = keys::tcaColOf(k);
  for (uint8_t a = 0; a < pg::kNumKeys; ++a) {
    if (a == k || !isHeld(g_held, a)) continue;
    if (keys::tcaRowOf(a) != kr) continue;                 // a shares k's scan row
    const uint8_t ac = keys::tcaColOf(a);
    for (uint8_t b = 0; b < pg::kNumKeys; ++b) {
      if (b == k || b == a || !isHeld(g_held, b)) continue;
      if (keys::tcaColOf(b) != kc) continue;               // b shares k's column
      const uint8_t d = keys::keyFromTca(keys::tcaRowOf(b), ac);
      if (d < pg::kNumKeys && d != k && isHeld(g_held, d)) {
        ++g_rectCount;
        g_lastRect[0] = a; g_lastRect[1] = b; g_lastRect[2] = d; g_lastRect[3] = k;
        // Arrived in the same controller batch as the previous key-down: the
        // classic signature of a ghost (no finger can be that simultaneous).
        g_lastRectSuspect = (tInt == g_lastDownTInt) && (g_lastDownKey != k);
        if (g_lastRectSuspect) ++g_ghostSuspects;
        sp("[keys] matrix rectangle held: %s %s %s %s%s\n", keys::legend(a), keys::legend(b), keys::legend(d),
           keys::legend(k), g_lastRectSuspect ? "  <-- last key arrived in the same batch: GHOST SUSPECT" : "");
        return;
      }
    }
  }
}

// ---- jobs (background tasks on core 1) ---------------------------------------
std::atomic<bool> g_jobBusy{false};
char g_jobName[24] = "";

struct FsResult {
  bool ran = false, ok = false, verified = false;
  uint32_t bytes = 0;
  float writeMs = 0, readMs = 0;
  char err[40] = "";
};
FsResult g_fsInt, g_fsSd;

struct StressResult {
  bool ran = false;
  float maxUs = 0, maxGapUs = 0, blockPeriodUs = 0;
  uint32_t underruns = 0, late = 0, blocks = 0;
  bool underrunsValid = false;
  char what[32] = "";
};
StressResult g_stressFlash, g_stressSd, g_stressDisplay;

struct I2cResult {
  bool ran = false;
  uint8_t addr[16];
  int count = 0;
  bool codec = false, kbd = false;
  int imuAddr = -1;
  uint8_t codecId[3] = {0, 0, 0};   // ES8311 regs 0xFD, 0xFE, 0xFF
  bool codecIdOk = false;
} g_i2c;

struct AllocResult {
  bool ran = false;
  bool ok[4] = {false, false, false, false};
  sys::HeapInfo before = {}, during = {}, after = {};
} g_alloc;
constexpr uint32_t kAllocSizes[4] = {70u * 1024, 55u * 1024, 32u * 1024, 28u * 1024};

uint32_t lcg(uint32_t& s) { s = s * 1664525u + 1013904223u; return s; }

void fsTest(const char* path, FsResult* r) {
  *r = FsResult();
  r->ran = true;
  pg::FileSystem& fs = storage::fs();
  uint32_t size = 64 * 1024;
  uint8_t* buf = static_cast<uint8_t*>(sys::allocInternal(size));
  if (!buf) { size = 32 * 1024; buf = static_cast<uint8_t*>(sys::allocInternal(size)); }
  if (!buf) { snprintf(r->err, sizeof(r->err), "no RAM for buffer"); return; }
  r->bytes = size;
  uint32_t seed = 0x5EED1234u;
  for (uint32_t i = 0; i < size; i += 4) { const uint32_t v = lcg(seed); memcpy(buf + i, &v, 4); }
  const uint64_t t0 = sys::micros();
  const bool w = fs.write(path, buf, size);
  const uint64_t t1 = sys::micros();
  r->writeMs = static_cast<float>(t1 - t0) / 1000.0f;
  if (!w) { snprintf(r->err, sizeof(r->err), "write failed"); sys::freeInternal(buf); return; }
  memset(buf, 0, size);
  size_t got = 0;
  const uint64_t t2 = sys::micros();
  const bool rd = fs.read(path, buf, size, &got);
  const uint64_t t3 = sys::micros();
  r->readMs = static_cast<float>(t3 - t2) / 1000.0f;
  if (!rd || got != size) {
    snprintf(r->err, sizeof(r->err), "read failed (%u bytes)", static_cast<unsigned>(got));
  } else {
    seed = 0x5EED1234u;
    bool same = true;
    for (uint32_t i = 0; i < size && same; i += 4) {
      const uint32_t v = lcg(seed);
      same = memcmp(buf + i, &v, 4) == 0;
    }
    r->verified = same;
    if (!same) snprintf(r->err, sizeof(r->err), "VERIFY MISMATCH");
  }
  fs.remove(path);
  sys::freeInternal(buf);
  storage::refreshUsage();
  r->ok = w && rd && r->verified;
}

void printFs(const char* tag, const FsResult& r) {
  sp("[%s] %s: %u bytes write %.1f ms (%.0f KB/s) read %.1f ms (%.0f KB/s) verify %s %s\n", tag,
     r.ok ? "OK" : "FAIL", static_cast<unsigned>(r.bytes), r.writeMs,
     r.writeMs > 0 ? r.bytes / 1.024f / r.writeMs : 0.0f, r.readMs,
     r.readMs > 0 ? r.bytes / 1.024f / r.readMs : 0.0f, r.verified ? "OK" : "FAIL", r.err);
}

struct JobArg { int kind; };
enum JobKind { kJobFsInt = 0, kJobFsSd, kJobFlashStress, kJobSdStress, kJobI2c };
JobArg g_jobArg;

void captureStress(StressResult* s, const char* what) {
  const audio::Stats st = audio::stats();
  s->ran = true;
  s->maxUs = st.maxUs;
  s->maxGapUs = st.maxPeriodUs;
  s->blockPeriodUs = st.blockPeriodUs;
  s->underruns = st.underruns;
  s->underrunsValid = st.underrunsValid;
  s->late = st.lateBlocks;
  s->blocks = st.blocks;
  configLabel(s->what, sizeof(s->what), g_acfg);
  (void)what;
}

void printStress(const char* tag, const StressResult& s) {
  sp("[%s] during test (%s): max render %.0f us, max gap between blocks %.0f us (period %.0f), underruns %lu%s, "
     "late %lu, blocks %lu\n",
     tag, s.what, s.maxUs, s.maxGapUs, s.blockPeriodUs, static_cast<unsigned long>(s.underruns),
     s.underrunsValid ? "" : " (n/a on m5spk)", static_cast<unsigned long>(s.late),
     static_cast<unsigned long>(s.blocks));
}

void i2cScan() {
  I2cResult r;
  r.ran = true;
  r.count = sys::i2cScan(r.addr, 16);
  for (int i = 0; i < r.count && i < 16; ++i) {
    if (r.addr[i] == 0x18) r.codec = true;
    if (r.addr[i] == 0x34) r.kbd = true;
    if (r.addr[i] == 0x68 || r.addr[i] == 0x69) r.imuAddr = r.addr[i];
  }
  if (r.codec) {
    r.codecIdOk = sys::i2cRead8(0x18, 0xFD, &r.codecId[0]) && sys::i2cRead8(0x18, 0xFE, &r.codecId[1]) &&
                  sys::i2cRead8(0x18, 0xFF, &r.codecId[2]);
  }
  g_i2c = r;
  char line[96];
  int n = snprintf(line, sizeof(line), "[i2c] internal bus (SDA 8 / SCL 9): %d device(s):", r.count);
  for (int i = 0; i < r.count && i < 16 && n < 90; ++i) n += snprintf(line + n, sizeof(line) - n, " 0x%02X", r.addr[i]);
  sp("%s\n", line);
  sp("[i2c] codec 0x18 %s, keyboard 0x34 %s, IMU %s (M5Unified says: %s); ES8311 id regs FD/FE/FF = %02X %02X %02X\n",
     r.codec ? "present" : "MISSING", r.kbd ? "present" : "MISSING",
     r.imuAddr >= 0 ? (r.imuAddr == 0x69 ? "0x69" : "0x68") : "not found", sys::imuName(), r.codecId[0],
     r.codecId[1], r.codecId[2]);
}

void jobMain(void* p) {
  const int kind = static_cast<JobArg*>(p)->kind;
  switch (kind) {
    case kJobFsInt:
      fsTest("/int/pg_smoke.bin", &g_fsInt);
      printFs("fs /int", g_fsInt);
      break;
    case kJobFsSd:
      fsTest("/sd/pg_smoke.bin", &g_fsSd);
      printFs("fs /sd", g_fsSd);
      break;
    case kJobFlashStress:
    case kJobSdStress: {
      const bool sd = (kind == kJobSdStress);
      FsResult tmp;
      for (int i = 0; i < 3; ++i) fsTest(sd ? "/sd/pg_stress.bin" : "/int/pg_stress.bin", &tmp);   // 3 x 64 KB
      StressResult* s = sd ? &g_stressSd : &g_stressFlash;
      captureStress(s, sd ? "sd" : "flash");
      printFs(sd ? "stress /sd" : "stress /int", tmp);
      printStress(sd ? "stress sd-write" : "stress flash-write", *s);
      break;
    }
    case kJobI2c:
      i2cScan();
      break;
  }
  g_jobBusy.store(false);
}

bool startJob(int kind, const char* name) {
  bool expected = false;
  if (!g_jobBusy.compare_exchange_strong(expected, true)) { flash("busy: %s", g_jobName); return false; }
  strncpy(g_jobName, name, sizeof(g_jobName) - 1);
  g_jobArg.kind = kind;
  if (!sys::runTask(jobMain, &g_jobArg, "pg_job", 8192, 1, 1)) {
    g_jobBusy.store(false);
    flash("could not start job");
    return false;
  }
  return true;
}

void allocTest() {
  g_alloc = AllocResult();
  g_alloc.ran = true;
  g_alloc.before = sys::heap();
  void* p[4] = {nullptr, nullptr, nullptr, nullptr};
  for (int i = 0; i < 4; ++i) {
    p[i] = sys::allocInternal(kAllocSizes[i]);
    g_alloc.ok[i] = p[i] != nullptr;
    if (p[i]) memset(p[i], 0xA5, kAllocSizes[i]);      // touch it
  }
  g_alloc.during = sys::heap();
  for (int i = 3; i >= 0; --i) sys::freeInternal(p[i]);
  g_alloc.after = sys::heap();
  sp("[alloc] instrument footprint 70K+55K+32K+28K: %s %s %s %s; while held: free %lu, largest %lu; "
     "before free %lu largest %lu; after free %lu\n",
     g_alloc.ok[0] ? "OK" : "FAIL", g_alloc.ok[1] ? "OK" : "FAIL", g_alloc.ok[2] ? "OK" : "FAIL",
     g_alloc.ok[3] ? "OK" : "FAIL", static_cast<unsigned long>(g_alloc.during.freeBytes),
     static_cast<unsigned long>(g_alloc.during.largestBlock), static_cast<unsigned long>(g_alloc.before.freeBytes),
     static_cast<unsigned long>(g_alloc.before.largestBlock), static_cast<unsigned long>(g_alloc.after.freeBytes));
}

// ---- CPU benchmark -----------------------------------------------------------
bench::Kind g_benchKind = bench::kOff;
int g_benchN = 0;
const int kNSteps[] = {0, 1, 2, 4, 8, 12, 16, 24, 32, 48, 64, 96, 128};

struct AutoEntry { bool done; int n; float avgUs, maxUs, cycPerInstSample; };
AutoEntry g_auto[bench::kNumKinds];
bool g_autoRan = false;
float g_autoBaseUs = 0;
char g_autoLabel[28] = "";
struct AutoRun { bool active = false; int kind = 0; int phase = 0; uint32_t t0 = 0; } g_ar;

bench::Primitives g_prim;
bool g_primRan = false;

void setBench(bench::Kind k, int n) {
  if (n > bench::maxInstances(k)) n = bench::maxInstances(k);
  if (k == bench::kOff) n = 0;
  g_benchKind = k;
  g_benchN = n;
  bench::configure(k, n, g_fs);
}

float perInstSampleCycles(float avgUs, float baseUs, int n) {
  if (n <= 0) return 0;
  const float net = avgUs - baseUs;
  return (net > 0 ? net : 0) * static_cast<float>(sys::cyclesPerUs()) / (static_cast<float>(n) * audio::kBlockFrames);
}

void printAuto() {
  sp("[cpu-auto] %s, cycles per instance per sample (render avg minus empty baseline %.1f us/block), "
     "budget %.0f cycles/sample:\n",
     g_autoLabel, g_autoBaseUs, static_cast<float>(sys::cyclesPerUs()) * 1.0e6f / g_fs);
  for (int k = 1; k < bench::kNumKinds; ++k) {
    const AutoEntry& e = g_auto[k];
    if (!e.done) continue;
    sp("[cpu-auto]   %-14s N=%-3d avg %7.1f us max %7.1f us  -> %7.1f cyc/inst/sample\n",
       bench::name(static_cast<bench::Kind>(k)), e.n, e.avgUs, e.maxUs, e.cycPerInstSample);
  }
}

void stepAutoRun() {
  if (!g_ar.active) return;
  const uint32_t now = sys::millis();
  if (g_ar.phase == 0) {                      // configure
    const bench::Kind k = static_cast<bench::Kind>(g_ar.kind);
    bench::configure(k, k == bench::kOff ? 0 : bench::autoInstances(k), g_fs);
    g_ar.t0 = now;
    g_ar.phase = 1;
  } else if (g_ar.phase == 1 && now - g_ar.t0 >= 150) {   // settle, then measure
    audio::resetStats();
    g_ar.t0 = now;
    g_ar.phase = 2;
  } else if (g_ar.phase == 2 && now - g_ar.t0 >= 700) {
    const audio::Stats st = audio::stats();
    AutoEntry& e = g_auto[g_ar.kind];
    e.done = true;
    e.n = g_ar.kind == 0 ? 0 : bench::autoInstances(static_cast<bench::Kind>(g_ar.kind));
    e.avgUs = st.avgUs;
    e.maxUs = st.maxUs;
    if (g_ar.kind == 0) g_autoBaseUs = st.avgUs;
    e.cycPerInstSample = perInstSampleCycles(st.avgUs, g_autoBaseUs, e.n);
    if (++g_ar.kind >= bench::kNumKinds) {
      g_ar.active = false;
      g_autoRan = true;
      setBench(g_benchKind, g_benchN);        // back to what the user had
      resetAudioStats();
      printAuto();
      flash("auto-run done");
    } else {
      g_ar.phase = 0;
    }
  }
}

void startAutoRun() {
  if (!g_audioOk) { flash("audio not running"); return; }
  archiveRun();
  for (auto& e : g_auto) e = AutoEntry{false, 0, 0, 0, 0};
  configLabel(g_autoLabel, sizeof(g_autoLabel), g_acfg);
  g_ar = AutoRun();
  g_ar.active = true;
  flash("auto-run: ~9 s");
  sp("[cpu-auto] running every kernel for 0.7 s on %s ...\n", g_autoLabel);
}

void runPrimitives() {
  bench::measurePrimitives(&sys::cycles, &g_prim);
  g_primRan = true;
  const bench::Primitives& p = g_prim;
  sp("[cpu-prim] cycles/op on core %d @ %lu MHz (dependent chains, min of 7):\n", sys::currentCore(),
     static_cast<unsigned long>(g_chip.cpuMhz));
  sp("[cpu-prim]   float add %.1f  mul-add %.1f  div %.1f  f->int16 clamp %.1f  lerp LUT %.1f\n", p.fadd, p.fmadd,
     p.fdiv, p.f2i16, p.lerpLut);
  sp("[cpu-prim]   sinf %.0f  expf %.0f  tanhf %.0f  tanh-rational %.1f\n", p.sinf_, p.expf_, p.tanhf_, p.tanhRat);
  sp("[cpu-prim]   DOUBLE mul-add %.1f  DOUBLE div %.0f  sin(double) %.0f\n", p.dmadd, p.ddiv, p.sinD);
}

// ============================================================================
// Report
// ============================================================================
// Streamed in ~1 KB chunks so the report costs no big static buffer (the RAM
// numbers this test reports should be close to what the instrument will see).
constexpr size_t kReportCap = 1536, kReportFlushAt = 1024;
char g_report[kReportCap];
size_t g_reportLen = 0;
void rpFlush() {
  if (g_reportLen) sys::serialWrite(g_report, g_reportLen);
  g_reportLen = 0;
}
void rp(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void rp(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  const int n = vsnprintf(g_report + g_reportLen, kReportCap - g_reportLen, fmt, ap);
  va_end(ap);
  if (n > 0) g_reportLen += static_cast<size_t>(n);
  if (g_reportLen > kReportCap - 1) g_reportLen = kReportCap - 1;   // truncated line (should not happen)
  if (g_reportLen >= kReportFlushAt) rpFlush();
}

void listUnseenKeys(char* buf, size_t cap) {
  size_t n = 0;
  buf[0] = 0;
  for (uint8_t k = 0; k < keys::kNumIds && n + 6 < cap; ++k) {
    if (isHeld(g_seenDown, k) && isHeld(g_seenUp, k)) continue;
    n += snprintf(buf + n, cap - n, "%s%s", n ? " " : "", keys::legend(k));
  }
}

void printReport() {
  g_reportLen = 0;
  const sys::HeapInfo h = sys::heap();
  const sys::Battery b = sys::battery();
  const disp::Stats ds = disp::stats();
  const keys::Stats ks = keys::stats();
  const storage::Info si = storage::info();
  rp("\n===== POCKET GROOVE SMOKE REPORT (%s, built %s %s) =====\n", kVersion, __DATE__, __TIME__);
  rp("uptime_s: %lu\n", static_cast<unsigned long>(sys::millis() / 1000));
  rp("[board] %s (m5 board id %d), display %s\n", g_boot.boardName, g_boot.boardId,
     g_boot.displayOk ? "OK" : "NOT DETECTED");
  rp("[chip] %s rev %u.%u, %u cores, %lu MHz, flash %lu MB, PSRAM %s (%lu bytes), IDF %s, arduino %s, reset %s\n",
     g_chip.model, g_chip.revMajor, g_chip.revMinor, g_chip.cores, static_cast<unsigned long>(g_chip.cpuMhz),
     static_cast<unsigned long>(g_chip.flashBytes >> 20), g_chip.psramFound ? "FOUND" : "none",
     static_cast<unsigned long>(g_chip.psramBytes), g_chip.idfVersion, g_chip.coreVersion, g_chip.resetReason);
  rp("[heap] at boot (before audio/keys): free %lu largest %lu | now: free %lu largest %lu min-ever %lu total %lu\n",
     static_cast<unsigned long>(g_heapBoot.freeBytes), static_cast<unsigned long>(g_heapBoot.largestBlock),
     static_cast<unsigned long>(h.freeBytes), static_cast<unsigned long>(h.largestBlock),
     static_cast<unsigned long>(h.minEverFree), static_cast<unsigned long>(h.totalBytes));
  if (g_alloc.ran)
    rp("[alloc] 70K %s 55K %s 32K %s 28K %s | held: free %lu largest %lu | before: free %lu largest %lu\n",
       g_alloc.ok[0] ? "OK" : "FAIL", g_alloc.ok[1] ? "OK" : "FAIL", g_alloc.ok[2] ? "OK" : "FAIL",
       g_alloc.ok[3] ? "OK" : "FAIL", static_cast<unsigned long>(g_alloc.during.freeBytes),
       static_cast<unsigned long>(g_alloc.during.largestBlock), static_cast<unsigned long>(g_alloc.before.freeBytes),
       static_cast<unsigned long>(g_alloc.before.largestBlock));
  rp("[battery] %d %% %d mV\n", b.percent, b.millivolts);
  rp("[display] full-frame present avg %.0f us max %.0f us, %.1f fps now, %lu frames, brightness %u\n", ds.avgUs,
     ds.maxUs, ds.fps, static_cast<unsigned long>(ds.frames), disp::brightness());
  rp("[fs /int] mounted %s%s in %lu ms, %lu/%lu bytes used\n", si.intMounted ? "yes" : "NO",
     si.intFormatted ? " (FORMATTED at boot)" : "", static_cast<unsigned long>(si.intMountMs),
     static_cast<unsigned long>(si.intUsedBytes), static_cast<unsigned long>(si.intTotalBytes));
  if (g_fsInt.ran)
    rp("[fs /int] test %s: %lu B write %.1f ms read %.1f ms verify %s %s\n", g_fsInt.ok ? "OK" : "FAIL",
       static_cast<unsigned long>(g_fsInt.bytes), g_fsInt.writeMs, g_fsInt.readMs, g_fsInt.verified ? "OK" : "FAIL",
       g_fsInt.err);
  rp("[fs /sd] %s (attempt took %lu ms), type %s, card %llu MB, fs %llu/%llu MB\n", storage::sdStateName(si.sd),
     static_cast<unsigned long>(si.sdMountMs), si.sdType, static_cast<unsigned long long>(si.sdCardBytes >> 20),
     static_cast<unsigned long long>(si.sdUsedBytes >> 20), static_cast<unsigned long long>(si.sdTotalBytes >> 20));
  if (g_fsSd.ran)
    rp("[fs /sd] test %s: %lu B write %.1f ms read %.1f ms verify %s %s\n", g_fsSd.ok ? "OK" : "FAIL",
       static_cast<unsigned long>(g_fsSd.bytes), g_fsSd.writeMs, g_fsSd.readMs, g_fsSd.verified ? "OK" : "FAIL",
       g_fsSd.err);
  if (g_i2c.ran) {
    rp("[i2c] found:");
    for (int i = 0; i < g_i2c.count && i < 16; ++i) rp(" 0x%02X", g_i2c.addr[i]);
    rp(" | codec %s kbd %s imu %s (M5: %s) | ES8311 FD/FE/FF %02X %02X %02X\n", g_i2c.codec ? "Y" : "N",
       g_i2c.kbd ? "Y" : "N", g_i2c.imuAddr == 0x69 ? "0x69" : g_i2c.imuAddr == 0x68 ? "0x68" : "none",
       sys::imuName(), g_i2c.codecId[0], g_i2c.codecId[1], g_i2c.codecId[2]);
  }
  // keys
  char unseen[300];
  listUnseenKeys(unseen, sizeof(unseen));
  const int seen = popcount64(g_seenDown & g_seenUp);
  rp("[keys] TCA8418 %s, events %lu, keys seen down+up %d/%d, max simultaneous %d, fifo overflows %lu, "
     "queue drops %lu, unknown codes %lu, i2c errors %lu, INT->read max %lu us\n",
     ks.controllerPresent ? "OK" : "NOT FOUND", static_cast<unsigned long>(g_keyEvents), seen, keys::kNumIds,
     g_maxHeld, static_cast<unsigned long>(ks.fifoOverflows), static_cast<unsigned long>(ks.queueDrops),
     static_cast<unsigned long>(ks.unknownCodes), static_cast<unsigned long>(ks.i2cErrors),
     static_cast<unsigned long>(ks.maxIntToReadUs));
  rp("[keys] never seen (down+up): %s\n", unseen[0] ? unseen : "(none - all keys work)");
  rp("[keys] matrix rectangles held %lu, ghost suspects (4th key in same batch) %lu", static_cast<unsigned long>(g_rectCount),
     static_cast<unsigned long>(g_ghostSuspects));
  if (g_rectCount)
    rp(", last: %s %s %s %s%s", keys::legend(g_lastRect[0]), keys::legend(g_lastRect[1]), keys::legend(g_lastRect[2]),
       keys::legend(g_lastRect[3]), g_lastRectSuspect ? " (suspect)" : "");
  rp("\n");
  // audio
  char lab[28];
  configLabel(lab, sizeof(lab), g_acfg);
  const audio::Stats st = audio::stats();
  rp("[audio] %s: %s%s%s, codec %s, vol reg 0x%02X (%.1f dB), latency %lu frames = %.1f ms, task stack free %lu\n", lab,
     g_audioOk ? "RUNNING" : "FAILED", g_audioErr[0] ? " / " : "", g_audioErr, audio::codecPresent() ? "ACK" : "NO-ACK",
     audio::volumeReg(), audio::volumeDb(), static_cast<unsigned long>(audio::latencyFrames()),
     1000.0f * audio::latencyFrames() / g_fs, static_cast<unsigned long>(st.stackFreeMin));
  rp("[audio] current run %lus: blocks %lu underruns %lu%s late %lu write-errors %lu overload-yields %lu | render avg "
     "%.1f max %.1f us, max gap %.0f us (period %.0f), cpu %.2f %% of core %d\n",
     static_cast<unsigned long>((sys::millis() - g_runStartMs) / 1000), static_cast<unsigned long>(st.blocks),
     static_cast<unsigned long>(st.underruns), st.underrunsValid ? "" : "(n/a)", static_cast<unsigned long>(st.lateBlocks),
     static_cast<unsigned long>(st.writeErrors), static_cast<unsigned long>(st.overloadYields), st.avgUs, st.maxUs,
     st.maxPeriodUs, st.blockPeriodUs, st.cpuPercent, st.taskCore);
  for (int i = 0; i < g_numRuns; ++i) {
    const RunRecord& r = g_runs[i];
    rp("[audio-run] %-26s %4lus blocks %7lu underruns %lu%s late %lu err %lu yields %lu avg %.1f max %.1f gap %.0f us cpu %.2f%%\n",
       r.label, static_cast<unsigned long>(r.seconds), static_cast<unsigned long>(r.blocks),
       static_cast<unsigned long>(r.underruns), r.underrunsValid ? "" : "(n/a)", static_cast<unsigned long>(r.late),
       static_cast<unsigned long>(r.writeErrors), static_cast<unsigned long>(r.overloadYields), r.avgUs, r.maxUs,
       r.maxGapUs, r.cpu);
  }
  if (g_blipCount)
    rp("[latency] SPACE->blip render: n %lu min %.2f avg %.2f max %.2f ms; + buffered %.1f ms => key->DAC est. %.1f ms avg\n",
       static_cast<unsigned long>(g_blipCount), g_blipMinUs / 1000.0f, g_blipSumUs / g_blipCount / 1000.0f,
       g_blipMaxUs / 1000.0f, 1000.0f * audio::latencyFrames() / g_fs,
       g_blipSumUs / g_blipCount / 1000.0f + 1000.0f * audio::latencyFrames() / g_fs);
  if (g_autoRan) {
    rp("[cpu-auto] %s, baseline %.1f us/block, budget %.0f cyc/sample\n", g_autoLabel, g_autoBaseUs,
       static_cast<float>(sys::cyclesPerUs()) * 1.0e6f / g_fs);
    for (int k = 1; k < bench::kNumKinds; ++k)
      if (g_auto[k].done)
        rp("[cpu-auto]   %-14s N=%-3d avg %7.1f max %7.1f us -> %7.1f cyc/inst/sample\n",
           bench::name(static_cast<bench::Kind>(k)), g_auto[k].n, g_auto[k].avgUs, g_auto[k].maxUs,
           g_auto[k].cycPerInstSample);
  }
  if (g_primRan) {
    const bench::Primitives& p = g_prim;
    rp("[cpu-prim] fadd %.1f fmadd %.1f fdiv %.1f f2i16 %.1f lerpLUT %.1f sinf %.0f expf %.0f tanhf %.0f tanhRat %.1f "
       "| double madd %.1f div %.0f sin %.0f (cycles/op)\n",
       p.fadd, p.fmadd, p.fdiv, p.f2i16, p.lerpLut, p.sinf_, p.expf_, p.tanhf_, p.tanhRat, p.dmadd, p.ddiv, p.sinD);
  }
  const StressResult* ss[3] = {&g_stressFlash, &g_stressSd, &g_stressDisplay};
  const char* sn[3] = {"flash-write 3x64K", "sd-write 3x64K", "display full-rate"};
  for (int i = 0; i < 3; ++i)
    if (ss[i]->ran)
      rp("[stress] %-18s (%s): max render %.0f us, max block gap %.0f us (period %.0f), underruns %lu%s, late %lu\n",
         sn[i], ss[i]->what, ss[i]->maxUs, ss[i]->maxGapUs, ss[i]->blockPeriodUs,
         static_cast<unsigned long>(ss[i]->underruns), ss[i]->underrunsValid ? "" : "(n/a)",
         static_cast<unsigned long>(ss[i]->late));
  // codec register dump (UI thread, I2C): lets us debug a silent codec remotely
  if (audio::codecPresent() || g_i2c.codec) {
    rp("[codec] ES8311 regs 00-45:");
    for (uint8_t r = 0; r <= 0x45; ++r) {
      uint8_t v = 0;
      if ((r & 15) == 0) rp("\n[codec]   %02X:", r);
      if (audio::codecRead(r, &v)) rp(" %02X", v); else rp(" --");
    }
    rp("\n");
  }
  rp("===== END REPORT =====\n\n");
  rpFlush();
  flash("report printed to serial");
}

// ============================================================================
// Drawing
// ============================================================================
uint8_t okColor(bool ok) { return ok ? kGreen : kRed; }

// ADC reads are not free: sample the battery at most once a second.
sys::Battery battery() {
  static sys::Battery b = {-1, -1};
  static uint32_t lastMs = 0;
  const uint32_t now = sys::millis();
  if (lastMs == 0 || now - lastMs >= 1000) { b = sys::battery(); lastMs = now | 1u; }
  return b;
}

void drawHeader() {
  char right[24];
  const audio::Stats st = audio::stats();
  if (g_audioOk) snprintf(right, sizeof(right), "u%lu %.0f%%", static_cast<unsigned long>(st.underruns), st.cpuPercent);
  else snprintf(right, sizeof(right), "NO AUDIO");
  char left[32];
  snprintf(left, sizeof(left), "%u/5 %s", g_page + 1, kPageName[g_page]);
  g_cv.title(left, right);
}

void drawFooter(const char* help1, const char* help2) {
  if (g_msg[0] && static_cast<int32_t>(sys::millis() - g_msgUntilMs) < 0) {
    g_cv.fillRect(0, 15 * kCellH, pg::kScreenW, kCellH, kDkGreen);
    g_cv.line(15, kWhite, "%s", g_msg);
    if (help1) g_cv.line(14, kGrey, "%s", help1);
    return;
  }
  if (help1) g_cv.line(14, kGrey, "%s", help1);
  if (help2) g_cv.line(15, kGrey, "%s", help2);
}

void drawKeys() {
  // 4 x 14 map. Cell 17 x 15 px starting at y = 10.
  constexpr int cw = 17, ch = 15, y0 = 10;
  for (uint8_t k = 0; k < pg::kNumKeys; ++k) {
    const int r = keys::rowOf(k), c = keys::colOf(k);
    const int x = c * cw + 1, y = y0 + r * ch;
    const bool held = isHeld(g_held, k);
    const bool seen = isHeld(g_seenDown, k) && isHeld(g_seenUp, k);
    const uint8_t bg = held ? kYellow : seen ? kDkGreen : kDark;
    g_cv.fillRect(x, y, cw - 1, ch - 1, bg);
    const char* l = keys::legend(k);
    const int len = static_cast<int>(strlen(l));
    const int spacing = len >= 3 ? 0 : 1;
    const int w = len * (5 + spacing) - spacing;
    g_cv.text(x + (cw - 1 - w) / 2, y + 4, l, held ? kBlack : kWhite, -1, spacing);
  }
  int ln = 9;   // grid ends at y = 70; text line 9 starts at y = 73
  const keys::Stats ks = keys::stats();
  const int seen = popcount64(g_seenDown & g_seenUp);
  const bool g0 = isHeld(g_held, keys::kKeyG0);
  g_cv.line(ln++, kWhite, "held %d  max %d  seen %d/57  G0 %s", popcount64(g_held), g_maxHeld, seen, g0 ? "DOWN" : "up");
  if (g_haveLastEv)
    g_cv.line(ln++, kCyan, "last: %s %-3s id%-2u raw %02X  %lu ev", g_lastEv.ev.down ? "DOWN" : "UP  ",
              keys::legend(g_lastEv.ev.key), g_lastEv.ev.key, g_lastEv.raw, static_cast<unsigned long>(g_keyEvents));
  else
    g_cv.line(ln++, kCyan, "press every key once (incl fn sft ctl)");
  if (!ks.controllerPresent) g_cv.line(ln++, kRed, "TCA8418 NOT FOUND at 0x34 (G0 still works)");
  else if (g_rectCount)
    g_cv.line(ln++, g_lastRectSuspect ? kRed : kOrange, "rect %s %s %s %s%s n=%lu", keys::legend(g_lastRect[0]),
              keys::legend(g_lastRect[1]), keys::legend(g_lastRect[2]), keys::legend(g_lastRect[3]),
              g_lastRectSuspect ? " GHOST?" : "", static_cast<unsigned long>(g_rectCount));
  else
    g_cv.line(ln++, kGrey, "ovf %lu  i2cerr %lu  INT->read max %luus", static_cast<unsigned long>(ks.fifoOverflows),
              static_cast<unsigned long>(ks.i2cErrors), static_cast<unsigned long>(ks.maxIntToReadUs));
  g_cv.line(ln++, kGrey, "chords: a+s+d (f lit?)  2+w+4 (r?)");
  g_cv.line(ln++, kGrey, "then 4: a+s+d+f  q+w+e+r  z+x+c+v");
  drawFooter("Fn+1..5 page  G0 next  Fn+Enter report", nullptr);
}

void drawAudio() {
  const audio::Stats st = audio::stats();
  int ln = 1;
  char lab[28];
  configLabel(lab, sizeof(lab), g_acfg);
  g_cv.line(ln++, g_audioOk ? kGreen : kRed, "%s %s", lab, g_audioOk ? "RUNNING" : "FAILED");
  const uint8_t sig = g_signal.load();
  if (sig == kSweep) g_cv.line(ln++, kYellow, "sig: sweep now %5.0f Hz", g_sweepHz);
  else g_cv.line(ln++, kYellow, "sig: %s", signalName(sig));
  const float db = audio::volumeDb();
  g_cv.line(ln++, db > 0 ? kRed : kWhite, "vol reg 0x%02X = %+.1f dB  codec %s", audio::volumeReg(), db,
            audio::codecPresent() ? "ACK" : "NO-ACK");
  g_cv.line(ln++, kWhite, "render last %.0f avg %.1f max %.0f us", st.lastUs, st.avgUs, st.maxUs);
  g_cv.line(ln++, kWhite, "cpu %.2f%% core%d  gap max %.0f/%.0fus", st.cpuPercent, st.taskCore, st.maxPeriodUs,
            st.blockPeriodUs);
  g_cv.line(ln++, st.underruns || st.writeErrors ? kRed : kGreen, "blk %lu undr %lu%s late %lu err %lu",
            static_cast<unsigned long>(st.blocks), static_cast<unsigned long>(st.underruns),
            st.underrunsValid ? "" : "?", static_cast<unsigned long>(st.lateBlocks),
            static_cast<unsigned long>(st.writeErrors));
  g_cv.line(ln++, kWhite, "buffered %lu fr = %.1f ms", static_cast<unsigned long>(audio::latencyFrames()),
            1000.0f * audio::latencyFrames() / g_fs);
  if (g_blipCount)
    g_cv.line(ln++, kCyan, "key->blip %.1f avg %.1f max %.1f ms", g_blipLastUs / 1000.0f,
              g_blipSumUs / g_blipCount / 1000.0f, g_blipMaxUs / 1000.0f);
  else
    g_cv.line(ln++, kGrey, "8 then SPACE: key->sound latency");
  if (g_audioErr[0]) g_cv.line(ln++, kRed, "%s", g_audioErr);
  g_cv.line(11, kGrey, "0 off 1 sine 2 L 3 R 4 sweep 5 55Hz");
  g_cv.line(12, kGrey, "6 square 7 click 8 blip  -= vol []fine");
  g_cv.line(13, kGrey, "s 44k/32k b backend c core d dma");
  drawFooter("w reg09 slot/len  i codec-init  r reset", nullptr);
}

void drawCpu() {
  const audio::Stats st = audio::stats();
  int ln = 1;
  if (g_ar.active) {
    g_cv.line(ln++, kYellow, "AUTO-RUN %d/%d: %s", g_ar.kind, bench::kNumKinds - 1,
              bench::name(static_cast<bench::Kind>(g_ar.kind)));
  } else {
    g_cv.line(ln++, kYellow, "bench: %s x%d  (core %d)", bench::name(g_benchKind), g_benchN, st.taskCore);
  }
  g_cv.line(ln++, st.cpuPercent > 90 ? kRed : kWhite, "render avg %.1f max %.0f us = %.1f%%", st.avgUs, st.maxUs,
            st.cpuPercent);
  const float budget = static_cast<float>(sys::cyclesPerUs()) * 1.0e6f / g_fs;
  if (!g_ar.active && g_benchN > 0 && g_autoRan)
    g_cv.line(ln++, kCyan, "~%.0f cyc/inst/smp (base %.0fus)", perInstSampleCycles(st.avgUs, g_autoBaseUs, g_benchN),
              g_autoBaseUs);
  else
    g_cv.line(ln++, kGrey, "budget %.0f cyc/sample @ %.0f Hz", budget, g_fs);
  g_cv.line(ln++, st.underruns ? kRed : kGreen, "undr %lu late %lu yld %lu gap %.0fus",
            static_cast<unsigned long>(st.underruns), static_cast<unsigned long>(st.lateBlocks),
            static_cast<unsigned long>(st.overloadYields), st.maxPeriodUs);
  const disp::Stats ds = disp::stats();
  g_cv.line(ln++, g_fullRate ? kOrange : kWhite, "display %s %.1fms %.0ffps", g_fullRate ? "FULL-RATE" : "30fps",
            ds.avgUs / 1000.0f, ds.fps);
  if (g_stressFlash.ran)
    g_cv.line(ln++, g_stressFlash.underruns ? kRed : kGreen, "flash wr: gap %.0fus undr %lu",
              g_stressFlash.maxGapUs, static_cast<unsigned long>(g_stressFlash.underruns));
  // results table: auto-run (2 per line) or primitives
  if (g_autoRan) {
    for (int k = 1; k < bench::kNumKinds && ln < 13; k += 2) {
      char a[24] = "", b[24] = "";
      snprintf(a, sizeof(a), "%.6s %5.0f", bench::name(static_cast<bench::Kind>(k)), g_auto[k].cycPerInstSample);
      if (k + 1 < bench::kNumKinds)
        snprintf(b, sizeof(b), "%.6s %5.0f", bench::name(static_cast<bench::Kind>(k + 1)), g_auto[k + 1].cycPerInstSample);
      g_cv.line(ln++, kCyan, "%-19s %s", a, b);
    }
  } else if (g_primRan) {
    g_cv.line(ln++, kCyan, "madd %.1f div %.1f i16 %.1f lut %.1f", g_prim.fmadd, g_prim.fdiv, g_prim.f2i16, g_prim.lerpLut);
    g_cv.line(ln++, kCyan, "sinf %.0f expf %.0f tanhf %.0f rat %.0f", g_prim.sinf_, g_prim.expf_, g_prim.tanhf_,
              g_prim.tanhRat);
    g_cv.line(ln++, kCyan, "double: madd %.1f div %.0f sin %.0f", g_prim.dmadd, g_prim.ddiv, g_prim.sinD);
  }
  g_cv.line(13, kGrey, "1saw 2svf 3sat 4ladr 5fdn 6div 7sin 8exp");
  g_cv.line(14, kGrey, "9tanh 0off -=N a auto p prim c core");
  drawFooter(nullptr, "f fullrate w flash-wr e sd-wr r reset");
}

void drawSystem() {
  const sys::HeapInfo h = sys::heap();
  const sys::Battery b = battery();
  const disp::Stats ds = disp::stats();
  const storage::Info si = storage::info();
  int ln = 1;
  g_cv.line(ln++, kWhite, "%s r%u.%u %ucore %luMHz fl %luMB", g_chip.model, g_chip.revMajor, g_chip.revMinor,
            g_chip.cores, static_cast<unsigned long>(g_chip.cpuMhz), static_cast<unsigned long>(g_chip.flashBytes >> 20));
  g_cv.line(ln++, kWhite, "PSRAM %s  IDF %.10s  %s", g_chip.psramFound ? "FOUND" : "none", g_chip.idfVersion,
            g_boot.isCardputerAdv ? "ADV" : g_boot.boardName);
  g_cv.line(ln++, kWhite, "heap %luk big %luk min %luk", static_cast<unsigned long>(h.freeBytes / 1024),
            static_cast<unsigned long>(h.largestBlock / 1024), static_cast<unsigned long>(h.minEverFree / 1024));
  if (g_alloc.ran) {
    const bool all = g_alloc.ok[0] && g_alloc.ok[1] && g_alloc.ok[2] && g_alloc.ok[3];
    g_cv.line(ln++, okColor(all), "alloc 70+55+32+28k %s left %luk", all ? "OK" : "FAIL",
              static_cast<unsigned long>(g_alloc.during.freeBytes / 1024));
  }
  g_cv.line(ln++, kWhite, "batt %d%% %dmV  reset %s", b.percent, b.millivolts, g_chip.resetReason);
  g_cv.line(ln++, kWhite, "disp frame %.1fms max %.1f %.0ffps", ds.avgUs / 1000.0f, ds.maxUs / 1000.0f, ds.fps);
  if (g_fsInt.ran)
    g_cv.line(ln++, okColor(g_fsInt.ok), "/int %s w%.0fms r%.0fms%s", g_fsInt.ok ? "OK" : "FAIL", g_fsInt.writeMs,
              g_fsInt.readMs, si.intFormatted ? " fmt" : "");
  else
    g_cv.line(ln++, okColor(si.intMounted), "/int %s", si.intMounted ? "mounted" : "NOT MOUNTED");
  if (g_fsSd.ran && si.sd == storage::SdState::Mounted)
    g_cv.line(ln++, okColor(g_fsSd.ok), "/sd %s %lluMB w%.0fms r%.0fms", si.sdType,
              static_cast<unsigned long long>(si.sdCardBytes >> 20), g_fsSd.writeMs, g_fsSd.readMs);
  else
    g_cv.line(ln++, si.sd == storage::SdState::Mounted ? kGreen : kGrey, "/sd %s (%lums)", storage::sdStateName(si.sd),
              static_cast<unsigned long>(si.sdMountMs));
  if (g_i2c.ran) {
    char buf[48];
    int n = snprintf(buf, sizeof(buf), "i2c:");
    for (int i = 0; i < g_i2c.count && i < 8 && n < 40; ++i) n += snprintf(buf + n, sizeof(buf) - n, " %02X", g_i2c.addr[i]);
    g_cv.line(ln++, okColor(g_i2c.codec && g_i2c.kbd), "%s", buf);
    g_cv.line(ln++, kWhite, "codec %s kbd %s imu %s id %02X%02X", g_i2c.codec ? "Y" : "N", g_i2c.kbd ? "Y" : "N",
              g_i2c.imuAddr == 0x69 ? "69" : g_i2c.imuAddr == 0x68 ? "68" : "-", g_i2c.codecId[0], g_i2c.codecId[1]);
  }
  if (g_jobBusy.load()) g_cv.line(13, kYellow, "running: %s ...", g_jobName);
  drawFooter("a alloc l /int test s sd i i2c", "b brightness  Fn+Enter report");
}

void drawReport() {
  const audio::Stats st = audio::stats();
  const keys::Stats ks = keys::stats();
  const storage::Info si = storage::info();
  int ln = 1;
  const int seen = popcount64(g_seenDown & g_seenUp);
  g_cv.line(ln++, kWhite, "%s %luMHz PSRAM %s heap %luk", g_chip.model, static_cast<unsigned long>(g_chip.cpuMhz),
            g_chip.psramFound ? "Y" : "N", static_cast<unsigned long>(sys::heap().freeBytes / 1024));
  g_cv.line(ln++, okColor(ks.controllerPresent && seen == keys::kNumIds), "keys %d/57 max%d rect%lu ghost%lu", seen,
            g_maxHeld, static_cast<unsigned long>(g_rectCount), static_cast<unsigned long>(g_ghostSuspects));
  char lab[28];
  configLabel(lab, sizeof(lab), g_acfg);
  g_cv.line(ln++, okColor(g_audioOk && st.underruns == 0), "%s u%lu", lab, static_cast<unsigned long>(st.underruns));
  g_cv.line(ln++, kWhite, "avg %.1f max %.0fus gap %.0f cpu %.1f%%", st.avgUs, st.maxUs, st.maxPeriodUs, st.cpuPercent);
  if (g_blipCount)
    g_cv.line(ln++, kWhite, "key->render %.1fms +buf %.1fms", g_blipSumUs / g_blipCount / 1000.0f,
              1000.0f * audio::latencyFrames() / g_fs);
  if (g_autoRan)
    g_cv.line(ln++, kCyan, "saw%.0f svf%.0f sat%.0f lad%.0f fdn%.0f", g_auto[1].cycPerInstSample,
              g_auto[2].cycPerInstSample, g_auto[3].cycPerInstSample, g_auto[4].cycPerInstSample,
              g_auto[5].cycPerInstSample);
  if (g_primRan)
    g_cv.line(ln++, kCyan, "madd%.1f div%.0f sinf%.0f dbl/%.0f", g_prim.fmadd, g_prim.fdiv, g_prim.sinf_, g_prim.ddiv);
  if (g_stressFlash.ran)
    g_cv.line(ln++, okColor(g_stressFlash.underruns == 0), "flash-wr gap %.0fus undr %lu", g_stressFlash.maxGapUs,
              static_cast<unsigned long>(g_stressFlash.underruns));
  g_cv.line(ln++, okColor(g_fsInt.ok), "/int %s  /sd %s", g_fsInt.ran ? (g_fsInt.ok ? "OK" : "FAIL") : "-",
            si.sd == storage::SdState::Mounted ? (g_fsSd.ok ? "OK" : "test?") : storage::sdStateName(si.sd));
  if (g_i2c.ran)
    g_cv.line(ln++, okColor(g_i2c.codec && g_i2c.kbd), "i2c codec%s kbd%s imu%02X", g_i2c.codec ? "Y" : "N",
              g_i2c.kbd ? "Y" : "N", g_i2c.imuAddr > 0 ? g_i2c.imuAddr : 0);
  g_cv.line(ln++, kWhite, "disp %.1fms  batt %d%%", disp::stats().avgUs / 1000.0f, battery().percent);
  drawFooter("Enter: print FULL report to serial", "photo this screen if no serial");
}

void draw() {
  g_cv.clear(kBlack);
  drawHeader();
  switch (g_page) {
    case kPageKeys: drawKeys(); break;
    case kPageAudio: drawAudio(); break;
    case kPageCpu: drawCpu(); break;
    case kPageSystem: drawSystem(); break;
    default: drawReport(); break;
  }
  disp::present(g_fb);
}

// ============================================================================
// Input
// ============================================================================
void setPage(int p) {
  g_page = static_cast<uint8_t>((p + kNumPages) % kNumPages);
  sp("[ui] page %s\n", kPageName[g_page]);
}

void audioKey(char c) {
  audio::Config cfg = g_acfg;
  switch (c) {
    case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': case '8': {
      const uint8_t s = static_cast<uint8_t>(c - '0');
      g_signal.store(s);
      sp("[audio] signal: %s\n", signalName(s));
      break;
    }
    case '-': case '=': case '[': case ']': {
      int v = audio::volumeReg();
      v += (c == '=') ? 4 : (c == '-') ? -4 : (c == ']') ? 1 : -1;
      if (v < 0) v = 0;
      if (v > 0xFF) v = 0xFF;
      const bool ok = audio::setVolumeReg(static_cast<uint8_t>(v));
      g_acfg.volumeReg = static_cast<uint8_t>(v);
      sp("[audio] volume reg 0x32 = 0x%02X (%+.1f dB) %s\n", v, audio::volumeDb(), ok ? "" : "(write FAILED)");
      break;
    }
    case 's':
      cfg.sampleRate = (cfg.sampleRate == 44100) ? 32000 : 44100;
      startAudio(cfg);
      break;
    case 'b':
      cfg.backend = (cfg.backend == audio::Backend::DirectI2S) ? audio::Backend::M5Speaker : audio::Backend::DirectI2S;
      startAudio(cfg);
      break;
    case 'c':
      cfg.core = cfg.core ? 0 : 1;
      startAudio(cfg);
      break;
    case 'd': {
      static const uint8_t steps[] = {3, 4, 6, 8, 12};
      int i = 0;
      while (i < 5 && steps[i] != cfg.dmaBufCount) ++i;
      cfg.dmaBufCount = steps[(i + 1) % 5];
      startAudio(cfg);
      break;
    }
    case 'w': {
      // ES8311 reg 0x09 (SDP in = DAC serial input). From the datasheet as
      // remembered, UNVERIFIED: bit 7 picks the I2S slot the mono DAC plays
      // (0 = left, 1 = right), bits 4:2 word length (000 = 24 bit, 011 = 16).
      // M5Unified leaves it at the reset value; cardputer-studio writes 0x0C.
      static const uint8_t vals[] = {0x00, 0x0C, 0x8C, 0x80};
      uint8_t cur = 0;
      const bool rd = audio::codecRead(0x09, &cur);
      int i = 0;
      while (i < 4 && vals[i] != cur) ++i;
      const uint8_t nv = vals[(i + 1) % 4];
      const bool ok = audio::codecWrite(0x09, nv);
      sp("[audio] ES8311 reg 0x09: 0x%02X -> 0x%02X (%s slot, %s) %s\n", rd ? cur : 0xFF, nv,
         (nv & 0x80) ? "RIGHT" : "LEFT", (nv & 0x0C) == 0x0C ? "16-bit" : "24-bit", ok ? "" : "WRITE FAILED");
      flash("reg09=%02X %s %s", nv, (nv & 0x80) ? "R-slot" : "L-slot", (nv & 0x0C) == 0x0C ? "16b" : "24b");
      break;
    }
    case 'i': {
      const bool ok = audio::codecInit();
      sp("[audio] codec init sequence re-sent: %s\n", ok ? "ACK" : "FAILED");
      flash("codec init %s", ok ? "OK" : "FAILED");
      break;
    }
    case 'r':
      archiveRun();
      resetAudioStats();
      g_blipCount = 0; g_blipSumUs = 0; g_blipMaxUs = 0; g_blipMinUs = 0xFFFFFFFFu;
      flash("audio stats reset");
      break;
    default: break;
  }
}

void cpuKey(char c) {
  if (g_ar.active) { flash("auto-run in progress"); return; }
  if (c >= '0' && c <= '9') {
    const bench::Kind k = static_cast<bench::Kind>(c - '0');
    int n = g_benchN;
    if (k == bench::kOff) n = 0;
    else if (n == 0 || k != g_benchKind) n = bench::autoInstances(k);
    setBench(k, n);
    resetAudioStatsSoon();
    sp("[cpu] bench %s x%d\n", bench::name(k), g_benchN);
    return;
  }
  switch (c) {
    case '-': case '=': {
      if (g_benchKind == bench::kOff) { flash("pick a kernel first (1-9)"); break; }
      int i = 0;
      const int ns = static_cast<int>(sizeof(kNSteps) / sizeof(kNSteps[0]));
      while (i < ns - 1 && kNSteps[i] < g_benchN) ++i;
      if (c == '=') { while (i < ns - 1 && kNSteps[i] <= g_benchN) ++i; }
      else { while (i > 0 && kNSteps[i] >= g_benchN) --i; }
      setBench(g_benchKind, kNSteps[i]);
      resetAudioStatsSoon();
      sp("[cpu] bench %s x%d\n", bench::name(g_benchKind), g_benchN);
      break;
    }
    case 'a': startAutoRun(); break;
    case 'p': runPrimitives(); flash("primitives measured"); break;
    case 'c': {
      audio::Config cfg = g_acfg;
      cfg.core = cfg.core ? 0 : 1;
      startAudio(cfg);
      break;
    }
    case 'f':
      if (!g_fullRate) {
        archiveRun();
        resetAudioStats();
        g_fullRate = true;
      } else {
        captureStress(&g_stressDisplay, "display");
        printStress("stress display-full-rate", g_stressDisplay);
        archiveRun("fullrate");
        resetAudioStats();
        g_fullRate = false;
      }
      break;
    case 'w':
      if (!storage::fs().mounted("/int")) { flash("/int not mounted"); break; }
      archiveRun();
      resetAudioStats();
      if (startJob(kJobFlashStress, "flash-write stress")) flash("writing 3x64K to flash...");
      break;
    case 'e':
      if (!storage::fs().mounted("/sd")) { flash("no SD mounted"); break; }
      archiveRun();
      resetAudioStats();
      if (startJob(kJobSdStress, "sd-write stress")) flash("writing 3x64K to SD...");
      break;
    case 'r':
      archiveRun();
      resetAudioStats();
      flash("audio stats reset");
      break;
    default: break;
  }
}

bool g_sdTestPending = false;

void systemKey(char c) {
  switch (c) {
    case 'a': allocTest(); break;
    case 'l':
      if (startJob(kJobFsInt, "/int test")) flash("/int test running");
      break;
    case 's':
      storage::sdMountAsync();
      g_sdTestPending = true;
      flash("mounting SD...");
      break;
    case 'i':
      startJob(kJobI2c, "i2c scan");
      break;
    case 'b': {
      static const uint8_t lv[] = {32, 80, 128, 200, 255};
      int i = 0;
      while (i < 5 && lv[i] <= disp::brightness()) ++i;
      disp::setBrightness(lv[i % 5]);
      flash("brightness %u", disp::brightness());
      break;
    }
    default: break;
  }
}

void handleKey(const keys::Event& e) {
  const uint8_t k = e.ev.key;
  const bool fnHeld = isHeld(g_held, kKeyFn);   // state BEFORE this event is applied
  if (k < 64) {
    if (e.ev.down) g_held |= (1ull << k), g_seenDown |= (1ull << k);
    else g_held &= ~(1ull << k), g_seenUp |= (1ull << k);
  }
  ++g_keyEvents;
  g_lastEv = e;
  g_haveLastEv = true;
  const int held = popcount64(g_held);
  if (held > g_maxHeld) g_maxHeld = held;
  sp("[key] %-4s %-3s id=%-2u raw=0x%02X held=%d INT->read %lu us\n", e.ev.down ? "down" : "up", keys::legend(k), k,
     e.raw, held, static_cast<unsigned long>(e.tReadUs - e.tIntUs));
  if (e.ev.down) {
    checkRectangle(k, e.tIntUs);
    g_lastDownTInt = e.tIntUs;
    g_lastDownKey = k;
  }
  if (!e.ev.down) return;

  // global navigation
  if (k == keys::kKeyG0) { setPage(g_page + 1); return; }
  if (fnHeld && k >= 1 && k <= 5) { setPage(k - 1); return; }       // Fn + 1..5
  if (fnHeld && k == kKeyEnter) { printReport(); return; }
  if (g_page == kPageKeys) {
    if (fnHeld && k == kKeyDel) {                                     // Fn + del: clear key stats
      g_seenDown = g_seenUp = 0; g_maxHeld = held; g_rectCount = 0; g_ghostSuspects = 0; g_keyEvents = 0;
      flash("key stats cleared");
    }
    return;                                                           // every other key is under test
  }
  if (fnHeld) return;
  if (k == kKeyTab) { setPage(g_page + 1); return; }
  const char c = keyChar(k);
  switch (g_page) {
    case kPageAudio: audioKey(c); break;
    case kPageCpu: cpuKey(c); break;
    case kPageSystem: systemKey(c); break;
    case kPageReport: if (k == kKeyEnter) printReport(); break;
    default: break;
  }
}

// Serial input: a character "presses" the key with that legend (so the app can
// be driven without a working keyboard). '>' = next page, '!' = report.
void pollSerial() {
  for (int n = 0; n < 16; ++n) {
    const int ch = sys::serialRead();
    if (ch < 0) return;
    static int prev = 0;
    const int last = prev;
    prev = ch;
    if (ch == '\n' && last == '\r') continue;     // CR LF = one Enter
    if (ch == '>') { setPage(g_page + 1); continue; }
    if (ch == '!') { printReport(); continue; }
    int id = -1;
    if (ch == ' ') id = kKeySpace;
    else if (ch == '\r' || ch == '\n') id = kKeyEnter;
    else if (ch == '\t') id = kKeyTab;
    else if (ch == 8 || ch == 127) id = kKeyDel;
    else {
      const char lc = (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : static_cast<char>(ch);
      for (uint8_t k = 0; k < pg::kNumKeys; ++k) {
        const char* l = keys::legend(k);
        if (l[0] == lc && !l[1]) { id = k; break; }
      }
    }
    if (id < 0) continue;
    if (g_page == kPageKeys) { setPage(kPageAudio); }               // serial = remote control, not a key test
    keys::Event e = {};
    e.ev.key = static_cast<uint8_t>(id);
    e.ev.down = true;
    e.tIntUs = e.tReadUs = static_cast<uint32_t>(sys::micros());
    // Feed through the same path, but keep the held mask untouched.
    const uint64_t held = g_held, sd = g_seenDown, su = g_seenUp;
    const int mx = g_maxHeld;
    const uint32_t evs = g_keyEvents;
    handleKey(e);
    g_held = held; g_seenDown = sd; g_seenUp = su; g_maxHeld = mx; g_keyEvents = evs;
  }
}

// ============================================================================
// Boot
// ============================================================================
void bootScreen(const char* step, uint8_t color = kWhite) {
  static int ln = 2;
  if (!step) { ln = 2; return; }
  g_cv.line(ln++, color, "%s", step);
  disp::present(g_fb);
  sp("[boot] %s\n", step);
}

}  // namespace

void setup() {
  g_boot = sys::begin(115200);
  g_heapBoot = sys::heap();
  sys::chipInfo(&g_chip);
  disp::begin();
  g_cv.initPalette();
  g_cv.clear(kBlack);
  g_cv.title("POCKET GROOVE smoke test", kVersion);
  bootScreen(nullptr);

  sp("\n\n===== POCKET GROOVE SMOKE TEST %s (built %s %s) =====\n", kVersion, __DATE__, __TIME__);
  char line[48];
  snprintf(line, sizeof(line), "board: %s (%d)", g_boot.boardName, g_boot.boardId);
  bootScreen(line, g_boot.isCardputerAdv ? kGreen : kRed);
  snprintf(line, sizeof(line), "%s %luMHz PSRAM %s heap %luk", g_chip.model, static_cast<unsigned long>(g_chip.cpuMhz),
           g_chip.psramFound ? "yes" : "no", static_cast<unsigned long>(g_heapBoot.freeBytes / 1024));
  bootScreen(line);

  bench::setAllocator([](unsigned bytes) { return static_cast<float*>(sys::allocInternal(bytes)); });
  for (int i = 0; i <= kLutSize; ++i) g_sinLut[i] = sinf(6.2831853f * static_cast<float>(i) / kLutSize);

  const bool kOk = keys::begin();
  keys::setListener(onKeyListener, nullptr);
  bootScreen(kOk ? "keyboard TCA8418: OK" : "keyboard TCA8418: NOT FOUND", okColor(kOk));

  bootScreen("mounting /int (first boot formats: ~5-20 s)", kYellow);
  const bool intOk = storage::beginInternal();
  snprintf(line, sizeof(line), "/int LittleFS: %s%s (%lu ms)", intOk ? "OK" : "FAILED",
           storage::info().intFormatted ? " formatted" : "", static_cast<unsigned long>(storage::info().intMountMs));
  bootScreen(line, okColor(intOk));

  // Audio before any big allocation (I2S DMA buffers need internal RAM).
  g_acfg = audio::Config();            // 44100, direct I2S, core 0, 4 DMA buffers, vol 0x90 (-23.5 dB)
  startAudio(g_acfg);
  snprintf(line, sizeof(line), "audio: %s", g_audioOk ? "OK (silent until you pick a signal)" : g_audioErr);
  bootScreen(line, okColor(g_audioOk));

  storage::sdMountAsync();
  g_sdTestPending = true;
  startJob(kJobI2c, "i2c scan");
  bootScreen("press any key -> keys page", kCyan);
  sys::delayMs(600);
  sp("[boot] done. Pages: Fn+1..5, G0 next. Serial: type a key char, '>' next page, '!' report.\n");
}

void loop() {
  keys::Event e;
  for (int n = 0; n < 32 && keys::poll(&e); ++n) handleKey(e);
  pollSerial();
  stepAutoRun();
  if (g_resetDueMs && static_cast<int32_t>(sys::millis() - g_resetDueMs) >= 0) {
    g_resetDueMs = 0;
    resetAudioStats();
  }

  // deferred jobs
  if (!g_jobBusy.load()) {
    if (g_sdTestPending && storage::sdState() != storage::SdState::Mounting) {
      g_sdTestPending = false;
      const storage::Info si = storage::info();
      sp("[fs /sd] mount: %s in %lu ms\n", storage::sdStateName(si.sd), static_cast<unsigned long>(si.sdMountMs));
      if (si.sd == storage::SdState::Mounted) startJob(kJobFsSd, "/sd test");
    } else if (g_i2c.ran && !g_fsInt.ran && storage::fs().mounted("/int")) {
      startJob(kJobFsInt, "/int test");
    } else if (g_fsInt.ran && !g_alloc.ran) {
      allocTest();
    }
  }

  const uint32_t now = sys::millis();
  if (g_fullRate || now - g_lastFrameMs >= 33) {
    g_lastFrameMs = now;
    draw();
  }
  if (!g_fullRate) sys::delayMs(1);    // let lower-priority work on core 1 run
}
