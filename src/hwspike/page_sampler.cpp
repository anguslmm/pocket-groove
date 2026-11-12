// SAMPLER page: mic level and gain, record to flash, playback from mapped
// flash at several pitches and voice counts, playback-cost benchmark.
//
// Ownership:
//   audio task  meter accumulators, recording ring head, voices, benchmark run
//   writer job  (core 1, prio 5) erase + streaming flash writes + header; bench prep
//   UI          keys, results, report
// The mapped flash is only read by the audio task while g_playEnable is set
// (or by the benchmark); every erase / write / unmap first clears it and waits
// for the audio task to acknowledge two blocks, so nothing reads a mapping
// that is being torn down.
#include <Arduino.h>

#include <esp_attr.h>
#include <esp_cpu.h>
#include <esp_heap_caps.h>
#include <esp_rom_sys.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <atomic>
#include <cmath>
#include <cstring>

#include "app.h"
#include "dsp.h"
#include "duplex_audio.h"
#include "mic_matrix.h"
#include "sample_store.h"

namespace hws {
namespace sampler {

using app::key;

namespace {

// ---- keys -------------------------------------------------------------------
constexpr uint8_t k1 = key(0, 1), k4 = key(0, 4);
constexpr uint8_t kR = key(1, 4), kW = key(1, 2), kP = key(1, 10);
constexpr uint8_t kL = key(2, 10), kA = key(2, 2), kK = key(2, 9);
constexpr uint8_t kX = key(3, 4), kV = key(3, 6), kB = key(3, 7), kN = key(3, 8), kM = key(3, 9);

constexpr int8_t kPitchSemis[8] = {-12, -7, -5, 0, 3, 5, 7, 12};   // a s d f g h j k
constexpr int8_t kStack[8] = {0, 12, 7, 19, -12, 5, 24, 3};        // extra voices of one trigger
constexpr uint8_t kVoiceModes[4] = {1, 2, 4, 8};
constexpr int kMaxVoices = 8;
constexpr uint32_t kChunks[3] = {256, 1024, 4096};

// ---- gain presets -------------------------------------------------------------
struct GainPreset {
  const char* name;
  uint8_t pga;    // reg 0x14 low nibble, 3 dB steps
  int     reg16;  // -1 = leave the power-on value
  int     pgaDb;
};
const GainPreset kGains[kNumGainRows] = {
    {"PGA 0dB (M5Unified)", 0, -1, 0},
    {"PGA 15dB", 5, -1, 15},
    {"PGA 30dB (espp/Rust max)", 10, -1, 30},
    {"PGA 30 + r16=24 (Espressif)", 10, 0x24, 30},
};
int g_gain = 2;
std::atomic<int> g_channel{0};

// ---- meter (audio task accumulates, publishes) ---------------------------------
LevelAcc g_mAcc[2], g_sAcc[2];          // audio task
uint32_t g_mBlocks = 0, g_sBlocks = 0;  // audio task
LevelAcc g_mPub[2], g_sPub[2];          // under g_mux
portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;

// ---- silence measurement --------------------------------------------------------
std::atomic<uint32_t> g_measLeft{0};
std::atomic<bool> g_measDone{false};
LevelAcc g_measAcc;
int g_measGain = 0;

// ---- recording -------------------------------------------------------------------
enum RecState : int { kIdle = 0, kErasing, kRecording, kFinishing };
std::atomic<int> g_rec{kIdle};
constexpr uint32_t kRingN = 16384;      // 371 ms at 44.1 kHz
int16_t g_ring[kRingN];
std::atomic<uint32_t> g_head{0}, g_tail{0};
volatile uint32_t g_ringDrops = 0, g_recPushed = 0, g_recBlocks = 0;
uint32_t g_recMax = store::kRecBytes / 2;
LevelAcc g_recAcc;                      // audio task while recording
std::atomic<bool> g_stopReq{false};
uint32_t g_chunkSel = 0;
uint8_t g_chunkBuf[4096];

struct RecResult {
  bool     valid = false;
  uint32_t samples = 0;
  uint32_t chunk = 0, writes = 0;
  float    writeAvgUs = 0, writeMaxUs = 0, eraseMs = 0;
  uint32_t rxOvf = 0, ringDrops = 0, blocks = 0;
  float    maxGapUs = 0;
  bool     writeError = false;
  LevelResult level{};
  int      gain = 0;
};
RecResult g_lastRec;                   // UI
RecResult g_jobRec;                    // writer job -> UI
std::atomic<bool> g_recResultReady{false};

// ---- playback -------------------------------------------------------------------
std::atomic<bool> g_playEnable{false};
std::atomic<uint32_t> g_audioBlocks{0};
const int16_t* volatile g_recPtr = nullptr;
volatile uint32_t g_recLen = 0;
Voice g_voices[kMaxVoices];             // audio task
int32_t g_mix[duplex::kBlockFrames];   // audio task
struct Trig {
  int8_t  semi;
  uint8_t count;
  uint8_t raw;      // 1:1 playback of the recording
  uint8_t stopAll;
};
constexpr uint32_t kTrigN = 16;
Trig g_trig[kTrigN];
std::atomic<uint32_t> g_trigHead{0}, g_trigTail{0};
int g_voiceMode = 0;
volatile uint32_t g_activeVoices = 0;
volatile float g_playAvgCyc = 0;
volatile uint32_t g_playMaxCyc = 0;

// ---- benchmark --------------------------------------------------------------------
enum BenchState : int { kBIdle = 0, kBPrep, kBRun, kBDone };
std::atomic<int> g_bench{kBIdle};
std::atomic<int> g_prepPct{0};
struct BenchCfg {
  uint8_t src;     // 0 ram, 1 flash same window, 2 flash 1 MB spread
  uint8_t voices;
  uint8_t pollKb;
};
constexpr int kBenchWarm = 8, kBenchMeas = 120;
BenchCfg g_cfgs[kMaxBenchRows];
BenchRow g_rows[kMaxBenchRows];
int g_nCfg = 0;
volatile int g_cfgIdx = 0;
int g_cfgBlock = 0;                    // audio task
uint64_t g_cfgCyc = 0;                 // audio task
uint32_t g_cfgMax = 0;                 // audio task
uint64_t g_pollCyc = 0, g_pollLines = 0;
uint32_t g_pollCursor = 0;
volatile uint32_t g_pollSink = 0;
int16_t* g_ram = nullptr;
uint32_t g_ramSamples = 0;
const char* g_srcName[3] = {"ram48k", "flash48k", "flash1m"};
int g_benchRows = 0;                   // UI copy of the finished count
float g_missCyc = 0;

// ---- helpers ----------------------------------------------------------------------
void applyGain() {
  const GainPreset& g = kGains[g_gain];
  duplex::setMicGain(g.pga, g.reg16);
}

uint8_t reg16Of(int g) { return kGains[g].reg16 < 0 ? duplex::bootReg16() : static_cast<uint8_t>(kGains[g].reg16); }

// Stops playback and waits until the audio task has run two more blocks
// (it then no longer touches the mapping). UI / job thread only.
bool quiesce() {
  g_playEnable.store(false);
  const uint32_t b0 = g_audioBlocks.load();
  for (int i = 0; i < 100; ++i) {
    if (g_audioBlocks.load() - b0 >= 2) return true;
    delay(1);
  }
  return !duplex::running();   // no audio task: nothing reads the mapping
}

void postTrig(const Trig& t) {
  const uint32_t h = g_trigHead.load(std::memory_order_relaxed);
  if (h - g_trigTail.load(std::memory_order_acquire) >= kTrigN) return;
  g_trig[h % kTrigN] = t;
  g_trigHead.store(h + 1, std::memory_order_release);
}

// ---- writer job ---------------------------------------------------------------------
void recordJob(void*) {
  RecResult r;
  r.gain = g_gain;
  r.chunk = kChunks[g_chunkSel];
  uint32_t ms = 0;
  bool ok = store::erase(store::kRecHdrOff, 0x1000, nullptr);
  ok = store::erase(store::kRecOff, store::kRecBytes, &ms) && ok;
  r.eraseMs = static_cast<float>(ms);
  store::unmapAll();
  g_head.store(0);
  g_tail.store(0);
  g_ringDrops = 0;
  g_recPushed = 0;
  g_recBlocks = 0;
  g_recAcc.clear();
  g_stopReq.store(false);
  duplex::resetRxGap();
  delay(10);   // the reset is applied at the audio task's next block
  g_rec.store(ok ? kRecording : kIdle);
  const uint32_t chunkSamples = r.chunk / 2;
  uint32_t off = 0;
  uint64_t sumUs = 0;
  while (ok) {
    if (g_stopReq.load() && g_rec.load() == kRecording) g_rec.store(kFinishing);
    const uint32_t head = g_head.load(std::memory_order_acquire);
    const uint32_t tail = g_tail.load(std::memory_order_relaxed);
    const uint32_t avail = head - tail;
    const bool finishing = g_rec.load() != kRecording;
    if (avail >= chunkSamples || (finishing && avail > 0)) {
      uint32_t n = avail < chunkSamples ? avail : chunkSamples;
      if (off + n > g_recMax) n = g_recMax - off;
      int16_t* dst = reinterpret_cast<int16_t*>(g_chunkBuf);
      for (uint32_t i = 0; i < n; ++i) dst[i] = g_ring[(tail + i) % kRingN];
      uint32_t us = 0;
      if (!store::write(store::kRecOff + off * 2, dst, n * 2, &us)) {
        r.writeError = true;
        break;
      }
      ++r.writes;
      sumUs += us;
      if (us > r.writeMaxUs) r.writeMaxUs = static_cast<float>(us);
      g_tail.store(tail + n, std::memory_order_release);
      off += n;
      if (off >= g_recMax) break;
    } else if (finishing) {
      break;
    } else {
      vTaskDelay(1);
    }
  }
  g_rec.store(kFinishing);   // the audio task stops pushing
  delay(10);
  const duplex::Stats as = duplex::stats();
  r.samples = off;
  r.writeAvgUs = r.writes ? static_cast<float>(sumUs) / static_cast<float>(r.writes) : 0.0f;
  r.rxOvf = as.rxOverflows;
  r.maxGapUs = as.maxRxGapUs;
  r.ringDrops = g_ringDrops;
  r.blocks = g_recBlocks;
  r.level = levelResult(g_recAcc);
  if (off >= 2 && !r.writeError) {
    store::RecHeader h = {};
    h.samples = off;
    h.sampleRate = app::g_sampleRate;
    h.gain = static_cast<uint8_t>(r.gain);
    h.channel = static_cast<uint8_t>(g_channel.load());
    h.reg14 = static_cast<uint8_t>(0x10 | kGains[r.gain].pga);
    h.reg16 = reg16Of(r.gain);
    h.peak = g_recAcc.peak;
    h.clips = g_recAcc.clips;
    h.rmsDb = r.level.rmsDb;
    h.dcLsb = r.level.dcLsb;
    store::writeRecHeader(h);
    r.valid = true;
  }
  store::mapAll();
  g_recPtr = r.valid ? store::recData() : nullptr;
  g_recLen = r.valid ? off : 0;
  g_jobRec = r;
  g_recResultReady.store(true);
  g_rec.store(kIdle);
  g_playEnable.store(store::mapped());
  vTaskDelete(nullptr);
}

void benchPrepJob(void*) {
  const bool ok = store::prepareBench([](int pct, void*) { g_prepPct.store(pct); }, nullptr);
  g_prepPct.store(ok ? 100 : -1);
  if (!ok) g_nCfg = 0;              // the UI's Done path frees the RAM copy and re-enables playback
  g_bench.store(ok ? kBRun : kBDone);
  vTaskDelete(nullptr);
}

void buildBench() {
  g_nCfg = 0;
  for (uint8_t src = 0; src < 3; ++src)
    for (uint8_t v : {1, 2, 4, 8})
      for (uint8_t kb : {0, 16, 64}) {
        if (g_nCfg >= kMaxBenchRows) break;
        g_cfgs[g_nCfg++] = {src, v, kb};
      }
  g_cfgIdx = 0;
  g_cfgBlock = 0;
  g_pollCyc = g_pollLines = 0;
  for (auto& r : g_rows) r = BenchRow();
}

bool startBench() {
  if (!store::info().ok) { app::msg("no sample store: %s", store::info().why); return false; }
  if (g_rec.load() != kIdle || g_bench.load() == kBRun || g_bench.load() == kBPrep) return false;
  quiesce();
  if (!g_ram) {
    g_ramSamples = 48 * 1024 / 2;
    g_ram = static_cast<int16_t*>(heap_caps_malloc(g_ramSamples * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (!g_ram) {
      g_ramSamples = 32 * 1024 / 2;
      g_ram = static_cast<int16_t*>(heap_caps_malloc(g_ramSamples * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
      g_srcName[0] = "ram32k";
      g_srcName[1] = "flash32k";
    }
  }
  if (!g_ram) { app::msg("bench: no RAM for the copy"); g_playEnable.store(store::mapped()); return false; }
  buildBench();
  g_prepPct.store(0);
  if (!store::benchReady()) {
    g_bench.store(kBPrep);
    g_prepPct.store(0);
    if (xTaskCreatePinnedToCore(benchPrepJob, "hws_prep", 4096, nullptr, 5, nullptr, 1) != pdPASS) {
      g_bench.store(kBIdle);
      return false;
    }
    app::msg("bench: writing the 1 MB test sample...");
    return true;
  }
  g_bench.store(kBRun);
  return true;
}

// Called by the UI when the benchmark state becomes Run after the prep (the
// mapping is new) and before the audio task starts it.
std::atomic<bool> g_ramFilled{false};

}  // namespace

// ---- audio task -----------------------------------------------------------------------
void IRAM_ATTR input(const int16_t* in, int frames) {
  g_audioBlocks.fetch_add(1, std::memory_order_release);   // quiesce() waits on this (runs on both pages)
  for (int i = 0; i < frames; ++i) {
    g_mAcc[0].add(in[2 * i]);
    g_mAcc[1].add(in[2 * i + 1]);
  }
  const int ch = g_channel.load(std::memory_order_relaxed);
  if (g_measLeft.load(std::memory_order_relaxed) > 0) {
    for (int i = 0; i < frames; ++i) g_measAcc.add(in[2 * i + ch]);
    const uint32_t left = g_measLeft.load(std::memory_order_relaxed);
    const uint32_t nl = left > static_cast<uint32_t>(frames) ? left - static_cast<uint32_t>(frames) : 0;
    g_measLeft.store(nl, std::memory_order_relaxed);
    if (nl == 0) g_measDone.store(true, std::memory_order_release);
  }
  if (g_rec.load(std::memory_order_relaxed) == kRecording) {
    uint32_t h = g_head.load(std::memory_order_relaxed);
    const uint32_t t = g_tail.load(std::memory_order_acquire);
    for (int i = 0; i < frames && g_recPushed < g_recMax; ++i) {
      const int16_t s = in[2 * i + ch];
      g_recAcc.add(s);
      g_recPushed = g_recPushed + 1;
      if (h - t < kRingN) {
        g_ring[h % kRingN] = s;
        ++h;
      } else {
        g_ringDrops = g_ringDrops + 1;
      }
    }
    g_head.store(h, std::memory_order_release);
    g_recBlocks = g_recBlocks + 1;
    if (g_recPushed >= g_recMax) g_rec.store(kFinishing, std::memory_order_relaxed);
  }
  if (++g_mBlocks >= 34) {   // ~100 ms
    portENTER_CRITICAL(&g_mux);
    g_mPub[0] = g_mAcc[0];
    g_mPub[1] = g_mAcc[1];
    portEXIT_CRITICAL(&g_mux);
    g_sAcc[0].merge(g_mAcc[0]);
    g_sAcc[1].merge(g_mAcc[1]);
    g_mAcc[0].clear();
    g_mAcc[1].clear();
    g_mBlocks = 0;
    if (++g_sBlocks >= 10) {  // ~1 s
      portENTER_CRITICAL(&g_mux);
      g_sPub[0] = g_sAcc[0];
      g_sPub[1] = g_sAcc[1];
      portEXIT_CRITICAL(&g_mux);
      g_sAcc[0].clear();
      g_sAcc[1].clear();
      g_sBlocks = 0;
    }
  }
}

namespace {

void IRAM_ATTR renderVoices(int32_t* mix, int frames) {
  for (int v = 0; v < kMaxVoices; ++v) voiceRender(g_voices[v], mix, frames);
}

void IRAM_ATTR benchBlock(int frames) {
  const int idx = g_cfgIdx;
  if (idx >= g_nCfg) {
    g_bench.store(kBDone, std::memory_order_release);
    return;
  }
  const BenchCfg& c = g_cfgs[idx];
  if (g_cfgBlock == 0) {
    const int16_t* data;
    uint32_t len;
    if (c.src == 0) { data = g_ram; len = g_ramSamples; }
    else if (c.src == 1) { data = store::benchData(); len = g_ramSamples; }
    else { data = store::benchData(); len = store::kBenchBytes / 2; }
    for (int v = 0; v < kMaxVoices; ++v) g_voices[v].active = false;
    for (int v = 0; v < c.voices; ++v)
      voiceStart(g_voices[v], data, len, static_cast<uint32_t>(static_cast<uint64_t>(len) * v / c.voices), 1.4983071f,
                 0.2f, true);
    g_cfgCyc = 0;
    g_cfgMax = 0;
  }
  if (c.pollKb) {
    // Read pollKb KB of other mapped flash, one byte per 32-byte cache line,
    // marching through 256 KB (8x the data cache): every read is a line fill.
    const volatile uint8_t* p = store::pollData();
    const uint32_t bytes = static_cast<uint32_t>(c.pollKb) * 1024u;
    uint32_t sink = 0, cur = g_pollCursor;
    const uint32_t t0 = esp_cpu_get_cycle_count();
    for (uint32_t o = 0; o < bytes; o += 32) {
      sink += p[cur];
      cur += 32;
      if (cur >= store::kPollBytes) cur = 0;
    }
    const uint32_t dt = esp_cpu_get_cycle_count() - t0;
    g_pollCursor = cur;
    g_pollSink = g_pollSink + sink;
    if (g_cfgBlock >= kBenchWarm) {
      g_pollCyc += dt;
      g_pollLines += bytes / 32;
    }
  }
  const uint32_t t0 = esp_cpu_get_cycle_count();
  memset(g_mix, 0, sizeof(int32_t) * frames);
  renderVoices(g_mix, frames);
  const uint32_t dt = esp_cpu_get_cycle_count() - t0;
  if (g_cfgBlock >= kBenchWarm) {
    g_cfgCyc += dt;
    if (dt > g_cfgMax) g_cfgMax = dt;
  }
  if (++g_cfgBlock >= kBenchWarm + kBenchMeas) {
    BenchRow& r = g_rows[idx];
    r.src = g_srcName[c.src];
    r.voices = c.voices;
    r.pollKb = c.pollKb;
    r.cycPerSample = static_cast<float>(g_cfgCyc) / static_cast<float>(kBenchMeas * frames);
    r.cycPerVoiceSample = r.cycPerSample / static_cast<float>(c.voices);
    r.maxBlockCyc = g_cfgMax;
    r.corePct = 100.0f * r.cycPerSample * static_cast<float>(app::g_sampleRate) /
                (static_cast<float>(esp_rom_get_cpu_ticks_per_us()) * 1.0e6f);
    g_cfgBlock = 0;
    g_cfgIdx = idx + 1;
    for (int v = 0; v < kMaxVoices; ++v) g_voices[v].active = false;
  }
}

void startVoice(const Trig& t, int j, float gain) {
  const int16_t* data = g_recPtr;
  const uint32_t len = g_recLen;
  if (!data || len < 2) return;
  int best = -1;
  uint32_t bestIdx = 0;
  for (int v = 0; v < kMaxVoices; ++v) {
    if (!g_voices[v].active) { best = v; break; }
    if (g_voices[v].idx >= bestIdx) { bestIdx = g_voices[v].idx; best = v; }   // steal the one furthest in
  }
  const float semi = t.raw ? 0.0f : static_cast<float>(t.semi + kStack[j]);
  voiceStart(g_voices[best], data, len, 0, semitoneRatio(semi), gain, false);
}

}  // namespace

void IRAM_ATTR render(int16_t* out, int frames) {
  const int bs = g_bench.load(std::memory_order_acquire);
  if (bs == kBRun && g_ramFilled.load(std::memory_order_acquire)) {
    benchBlock(frames);
    memset(out, 0, sizeof(int16_t) * 2 * frames);
    return;
  }
  if (!g_playEnable.load(std::memory_order_acquire)) {
    for (int v = 0; v < kMaxVoices; ++v) g_voices[v].active = false;
    g_activeVoices = 0;
    memset(out, 0, sizeof(int16_t) * 2 * frames);
    return;
  }
  // triggers
  uint32_t tt = g_trigTail.load(std::memory_order_relaxed);
  const uint32_t th = g_trigHead.load(std::memory_order_acquire);
  while (tt != th) {
    const Trig t = g_trig[tt % kTrigN];
    ++tt;
    if (t.stopAll || t.raw) for (int v = 0; v < kMaxVoices; ++v) g_voices[v].active = false;
    if (t.stopAll) continue;
    const int n = t.raw ? 1 : t.count;
    const float gain = t.raw ? 1.0f : 0.8f / sqrtf(static_cast<float>(n));
    for (int j = 0; j < n; ++j) startVoice(t, j, gain);
  }
  g_trigTail.store(tt, std::memory_order_release);

  const uint32_t t0 = esp_cpu_get_cycle_count();
  memset(g_mix, 0, sizeof(int32_t) * frames);
  renderVoices(g_mix, frames);
  const uint32_t dt = esp_cpu_get_cycle_count() - t0;
  uint32_t act = 0;
  for (int v = 0; v < kMaxVoices; ++v) act += g_voices[v].active ? 1u : 0u;
  g_activeVoices = act;
  if (act) {
    const float a = g_playAvgCyc;
    g_playAvgCyc = (a == 0.0f) ? static_cast<float>(dt) : a + (static_cast<float>(dt) - a) * (1.0f / 32.0f);
    if (dt > g_playMaxCyc) g_playMaxCyc = dt;
  }
  for (int i = 0; i < frames; ++i) {
    const int16_t s = sat16(g_mix[i]);
    out[2 * i] = s;
    out[2 * i + 1] = s;
  }
}

// ---- UI ------------------------------------------------------------------------------
void begin() {
  applyGain();
  const bool ok = store::begin();
  const store::Info& in = store::info();
  app::sp("[sampler] store: %s%s part 0x%06lX+0x%06lX, spiffs 0x%06lX+0x%06lX, running 0x%06lX, mapped %d\n",
          ok ? "OK" : "REFUSED: ", in.why, static_cast<unsigned long>(in.partAddr),
          static_cast<unsigned long>(in.partSize), static_cast<unsigned long>(in.fsAddr),
          static_cast<unsigned long>(in.fsSize), static_cast<unsigned long>(in.runningAddr), store::mapped() ? 1 : 0);
  store::RecHeader h;
  if (ok && store::readRecHeader(&h)) {
    g_recPtr = store::recData();
    g_recLen = h.samples;
    g_lastRec.valid = true;
    g_lastRec.samples = h.samples;
    g_lastRec.gain = h.gain < kNumGainRows ? h.gain : 0;
    g_lastRec.level.peakDb = dbfs(static_cast<float>(h.peak));
    g_lastRec.level.rmsDb = h.rmsDb;
    g_lastRec.level.dcLsb = h.dcLsb;
    g_lastRec.level.clips = h.clips;
    app::sp("[sampler] recording in flash: %lu samples (%.2f s), gain %u\n", static_cast<unsigned long>(h.samples),
            static_cast<double>(static_cast<float>(h.samples) / static_cast<float>(app::g_sampleRate)), h.gain + 1);
  }
  g_playEnable.store(store::mapped());
}

void enter() { g_playEnable.store(store::mapped() && g_rec.load() == kIdle && g_bench.load() != kBRun); }

void leave() { postTrig({0, 0, 0, 1}); }

namespace {
// UI-side results per gain
struct GainResults {
  bool silValid = false;
  LevelResult sil{};
  bool recValid = false;
  LevelResult rec{};
  float recSecs = 0;
};
GainResults g_gr[kNumGainRows];
float g_peakHold[2] = {-120, -120};
uint32_t g_peakHoldT = 0;
}  // namespace

bool startMatrix() {
  const int bs = g_bench.load();
  if (micmatrix::running()) { app::msg("matrix already running"); return false; }
  if (g_rec.load() != kIdle || bs == kBRun || bs == kBPrep) { app::msg("busy"); return false; }
  quiesce();   // no playback from the mapping while the driver restarts
  if (!micmatrix::start()) {
    g_playEnable.store(store::mapped());
    app::msg("matrix did not start");
    return false;
  }
  app::msg("mic matrix: ~30 s, unplug headphones");
  return true;
}

void key(uint8_t k, bool down) {
  if (!down) return;
  if (micmatrix::running()) { app::msg("mic matrix running..."); return; }
  if (k == kM) { startMatrix(); return; }
  const int rec = g_rec.load();
  const int bs = g_bench.load();
  const bool busy = rec != kIdle || bs == kBRun || bs == kBPrep;
  if (k == kR) {
    if (rec == kRecording) { g_stopReq.store(true); return; }
    if (busy) { app::msg("busy"); return; }
    if (!store::info().ok) { app::msg("no sample store: %s", store::info().why); return; }
    quiesce();
    g_rec.store(kErasing);
    if (xTaskCreatePinnedToCore(recordJob, "hws_rec", 4096, nullptr, 5, nullptr, 1) != pdPASS) {
      g_rec.store(kIdle);
      g_playEnable.store(store::mapped());
      app::msg("could not start the recorder");
    }
    return;
  }
  if (k >= k1 && k <= k4) {
    if (rec == kRecording) { app::msg("not while recording"); return; }
    g_gain = k - k1;
    applyGain();
    app::msg("gain %d: %s", g_gain + 1, kGains[g_gain].name);
    return;
  }
  if (k == kL) {
    g_channel.store(g_channel.load() ^ 1);
    app::msg("input slot %s", g_channel.load() ? "RIGHT" : "LEFT");
    return;
  }
  if (k == kN) {
    if (g_measLeft.load() > 0) return;
    g_measAcc.clear();
    g_measGain = g_gain;
    g_measDone.store(false);
    g_measLeft.store(3 * app::g_sampleRate);
    app::msg("silence: quiet for 3 s...");
    return;
  }
  if (k == kW) {
    if (busy) return;
    g_chunkSel = (g_chunkSel + 1) % 3;
    app::msg("flash write chunk %lu B", static_cast<unsigned long>(kChunks[g_chunkSel]));
    return;
  }
  if (k == kV) {
    g_voiceMode = (g_voiceMode + 1) % 4;
    app::msg("voices per trigger: %u", kVoiceModes[g_voiceMode]);
    return;
  }
  if (k == kX) { postTrig({0, 0, 0, 1}); return; }
  if (k == kP) {
    if (!g_recPtr) { app::msg("nothing recorded yet ([r])"); return; }
    postTrig({0, 1, 1, 0});
    return;
  }
  if (k >= kA && k <= kK) {
    if (!g_recPtr) { app::msg("nothing recorded yet ([r])"); return; }
    postTrig({kPitchSemis[k - kA], kVoiceModes[g_voiceMode], 0, 0});
    return;
  }
  if (k == kB) {
    if (busy) { app::msg("busy"); return; }
    if (startBench()) app::msg("bench running (~15 s), keep quiet");
    return;
  }
}

void tick() {
  static bool s_matrixWasRunning = false;
  int slot = 0;
  if (micmatrix::tick(&slot)) {
    g_channel.store(slot);
    applyGain();   // the user's gain preset on top of the setup the matrix switched to
    app::msg("%s: slot %s", micmatrix::statusLine(), slot ? "R" : "L");
  }
  const bool mr = micmatrix::running();
  if (s_matrixWasRunning && !mr) g_playEnable.store(store::mapped() && g_rec.load() == kIdle);
  s_matrixWasRunning = mr;
  if (g_measDone.exchange(false)) {
    const LevelResult r = levelResult(g_measAcc);
    g_gr[g_measGain].silValid = true;
    g_gr[g_measGain].sil = r;
    app::msg("silence: %.1f dBFS rms, pk %.1f", static_cast<double>(r.acRmsDb), static_cast<double>(r.peakDb));
  }
  if (g_recResultReady.exchange(false)) {
    g_lastRec = g_jobRec;
    if (g_lastRec.valid) {
      GainResults& gr = g_gr[g_lastRec.gain];
      gr.recValid = true;
      gr.rec = g_lastRec.level;
      gr.recSecs = static_cast<float>(g_lastRec.samples) / static_cast<float>(app::g_sampleRate);
      app::msg("rec %.2f s, pk %.1f dBFS, drops %lu", static_cast<double>(gr.recSecs),
               static_cast<double>(gr.rec.peakDb),
               static_cast<unsigned long>(g_lastRec.rxOvf + g_lastRec.ringDrops));
    } else {
      app::msg("recording failed%s", g_lastRec.writeError ? " (flash write)" : "");
    }
  }
  const int bs = g_bench.load();
  if (bs == kBRun && !g_ramFilled.load()) {
    // after a fresh prep the mapping is new; copy the RAM window now
    const int16_t* src = store::benchData();
    if (src && g_ram) {
      memcpy(g_ram, src, g_ramSamples * 2);
      g_ramFilled.store(true, std::memory_order_release);
    } else {
      g_nCfg = 0;
      g_prepPct.store(-1);
      g_bench.store(kBDone);
    }
  }
  if (bs == kBDone) {
    g_benchRows = g_nCfg;
    g_missCyc = g_pollLines ? static_cast<float>(g_pollCyc) / static_cast<float>(g_pollLines) : 0.0f;
    heap_caps_free(g_ram);
    g_ram = nullptr;
    g_ramFilled.store(false);
    g_bench.store(kBIdle);
    g_playEnable.store(store::mapped() && g_rec.load() == kIdle);
    if (g_prepPct.load() < 0) app::msg("bench: test sample not written/mapped");
    else app::msg("bench done: Enter prints the table");
  }
}

void draw() {
  using namespace smoke;
  Canvas& cv = app::g_cv;
  cv.clear(kBlack);
  cv.title("SAMPLER  [tab] tilt", kReportVersion);
  const store::Info& si = store::info();
  LevelAcc m[2];
  portENTER_CRITICAL(&g_mux);
  m[0] = g_mPub[0];
  m[1] = g_mPub[1];
  portEXIT_CRITICAL(&g_mux);
  const LevelResult l0 = levelResult(m[0]), l1 = levelResult(m[1]);
  const uint32_t now = millis();
  if (now - g_peakHoldT > 1500) {
    g_peakHold[0] = g_peakHold[1] = -120;
    g_peakHoldT = now;
  }
  if (l0.peakDb > g_peakHold[0]) g_peakHold[0] = l0.peakDb;
  if (l1.peakDb > g_peakHold[1]) g_peakHold[1] = l1.peakDb;
  const int ch = g_channel.load();
  cv.line(1, si.ok ? kGreen : kRed, "store %s %s", si.ok ? "app1 @0x340000" : "REFUSED", si.ok ? (store::benchReady() ? "bench-rdy" : "") : si.why);
  cv.line(2, kWhite, "L%c rms %6.1f pk %6.1f%s", ch == 0 ? '*' : ' ', static_cast<double>(l0.acRmsDb),
          static_cast<double>(g_peakHold[0]), l0.stuck ? " STUCK" : "");
  cv.line(3, kWhite, "R%c rms %6.1f pk %6.1f%s", ch == 1 ? '*' : ' ', static_cast<double>(l1.acRmsDb),
          static_cast<double>(g_peakHold[1]), l1.stuck ? " STUCK" : "");
  // meters: -72..0 dBFS over 66 px at the right
  for (int c = 0; c < 2; ++c) {
    const float pk = c ? g_peakHold[1] : g_peakHold[0];
    const float rms = c ? l1.acRmsDb : l0.acRmsDb;
    const int y = (2 + c) * kCellH + 1;
    const int x0 = 170, w = 66;
    cv.rect(x0, y, w, 6, kDark);
    auto px = [&](float db) { int v = static_cast<int>((db + 72.0f) / 72.0f * static_cast<float>(w)); return v < 0 ? 0 : (v > w ? w : v); };
    cv.fillRect(x0, y + 1, px(rms), 4, kGreen);
    cv.fillRect(x0 + px(pk) - 1, y, 2, 6, pk > -1.0f ? kRed : kYellow);
  }
  cv.line(4, kCyan, "[1-4] %d %s", g_gain + 1, kGains[g_gain].name);
  cv.line(5, kWhite, "[l] slot %s dc %.0f/%.0f clip %lu", ch ? "R" : "L", static_cast<double>(l0.dcLsb),
          static_cast<double>(l1.dcLsb), static_cast<unsigned long>(ch ? l1.clips : l0.clips));
  const int rec = g_rec.load();
  if (rec == kErasing) {
    cv.line(6, kYellow, "[r] erasing flash...");
  } else if (rec == kRecording || rec == kFinishing) {
    cv.line(6, kRed, "[r] REC %.2f / %.2f s drops %lu", static_cast<double>(static_cast<float>(g_recPushed) / static_cast<float>(app::g_sampleRate)),
            static_cast<double>(static_cast<float>(g_recMax) / static_cast<float>(app::g_sampleRate)),
            static_cast<unsigned long>(g_ringDrops));
  } else {
    cv.line(6, kWhite, "[r] record  [w] chunk %luB", static_cast<unsigned long>(kChunks[g_chunkSel]));
  }
  if (g_lastRec.valid || g_lastRec.writes) {
    cv.line(7, (g_lastRec.rxOvf || g_lastRec.ringDrops) ? kRed : kGreen, "last %.2fs pk %.1f ovf %lu drop %lu",
            static_cast<double>(static_cast<float>(g_lastRec.samples) / static_cast<float>(app::g_sampleRate)),
            static_cast<double>(g_lastRec.level.peakDb), static_cast<unsigned long>(g_lastRec.rxOvf),
            static_cast<unsigned long>(g_lastRec.ringDrops));
    cv.line(8, kWhite, "wr %lux%lu avg %.0f max %.0fus gap %.0f", static_cast<unsigned long>(g_lastRec.writes),
            static_cast<unsigned long>(g_lastRec.chunk), static_cast<double>(g_lastRec.writeAvgUs),
            static_cast<double>(g_lastRec.writeMaxUs), static_cast<double>(g_lastRec.maxGapUs));
  } else {
    cv.line(7, kGrey, "no recording yet");
  }
  const GainResults& gr = g_gr[g_gain];
  if (g_measLeft.load() > 0) cv.line(9, kYellow, "[n] measuring silence...");
  else if (gr.silValid) cv.line(9, kWhite, "[n] silence %.1f dBFS pk %.1f dc %.0f", static_cast<double>(gr.sil.acRmsDb),
                                static_cast<double>(gr.sil.peakDb), static_cast<double>(gr.sil.dcLsb));
  else cv.line(9, kGrey, "[n] measure silence (3 s)");
  const float per = static_cast<float>(esp_rom_get_cpu_ticks_per_us()) * 1.0e6f / static_cast<float>(app::g_sampleRate);
  cv.line(10, kWhite, "[a-k] play x%u [v] act %lu [p]raw [x]stop", kVoiceModes[g_voiceMode],
          static_cast<unsigned long>(g_activeVoices));
  cv.line(11, kWhite, "play %.0f cyc/smp (%.1f%% core) max %.0f",
          static_cast<double>(g_playAvgCyc / duplex::kBlockFrames),
          static_cast<double>(100.0f * g_playAvgCyc / duplex::kBlockFrames / per),
          static_cast<double>(static_cast<float>(g_playMaxCyc) / duplex::kBlockFrames));
  const int bs = g_bench.load();
  if (bs == kBPrep) cv.line(12, kYellow, "[b] writing test sample %d%%", g_prepPct.load());
  else if (bs == kBRun) cv.line(12, kYellow, "[b] bench %d / %d", g_cfgIdx, g_nCfg);
  else if (g_benchRows) {
    // the 8-voice rows without polluter: ram / flash48k / flash1m
    float c[3] = {0, 0, 0};
    for (int i = 0; i < g_benchRows; ++i)
      if (g_rows[i].voices == 8 && g_rows[i].pollKb == 0) {
        const int s = (g_rows[i].src == g_srcName[0]) ? 0 : (g_rows[i].src == g_srcName[1] ? 1 : 2);
        c[s] = g_rows[i].cycPerVoiceSample;
      }
    cv.line(12, kGreen, "8v cyc/v-smp ram %.0f fl %.0f 1M %.0f", static_cast<double>(c[0]), static_cast<double>(c[1]),
            static_cast<double>(c[2]));
    if (!micmatrix::running() && !micmatrix::finished())
      cv.line(13, kGreen, "flash line fill %.0f cyc", static_cast<double>(g_missCyc));
  } else {
    cv.line(12, kGrey, "[b] playback cost bench (~15 s)");
  }
  if (micmatrix::running()) cv.line(13, kYellow, "%s", micmatrix::statusLine());
  else if (micmatrix::finished()) cv.line(13, micmatrix::works() ? kGreen : kRed, "%s", micmatrix::statusLine());
  else if (!g_benchRows) cv.line(13, kGrey, "[m] mic matrix (~30 s, speaker on)");
  cv.line(14, kYellow, "%s", app::currentMsg());
  cv.line(15, kGrey, "r 1-4 l n w a-k v p x b m  -/= vol");
}

void report() {
  SamplerReport r;
  const store::Info& si = store::info();
  r.storeOk = si.ok;
  r.storeWhy = si.why;
  r.partAddr = si.partAddr;
  r.partSize = si.partSize;
  r.fsAddr = si.fsAddr;
  r.fsSize = si.fsSize;
  r.runningAddr = si.runningAddr;
  r.recRegionOff = store::kRecOff;
  r.recRegionSize = store::kRecBytes;
  r.benchRegionOff = store::kBenchOff;
  r.benchRegionSize = store::kBenchBytes;
  r.benchReady = store::benchReady();
  static const uint8_t regs[10] = {0x01, 0x09, 0x0A, 0x0D, 0x0E, 0x14, 0x15, 0x16, 0x17, 0x1C};
  r.regsValid = true;
  for (int i = 0; i < 10; ++i)
    if (!duplex::codecRead(regs[i], &r.regs[i])) r.regsValid = false;
  r.rate = app::g_sampleRate;
  r.channel = g_channel.load();
  r.gain = g_gain;
  LevelAcc s[2];
  portENTER_CRITICAL(&g_mux);
  s[0] = g_sPub[0];
  s[1] = g_sPub[1];
  portEXIT_CRITICAL(&g_mux);
  for (int c = 0; c < 2; ++c) {
    const LevelResult lr = levelResult(s[c]);
    r.liveRmsDb[c] = lr.acRmsDb;
    r.livePeakDb[c] = lr.peakDb;
    r.liveDc[c] = lr.dcLsb;
    r.liveStuck[c] = lr.stuck;
  }
  for (int g = 0; g < kNumGainRows; ++g) {
    GainRow& row = r.gains[g];
    row.used = true;
    row.pgaDb = kGains[g].pgaDb;
    row.reg14 = static_cast<uint8_t>(0x10 | kGains[g].pga);
    row.reg16 = reg16Of(g);
    row.silValid = g_gr[g].silValid;
    row.silAcRmsDb = g_gr[g].sil.acRmsDb;
    row.silPeakDb = g_gr[g].sil.peakDb;
    row.silDcLsb = g_gr[g].sil.dcLsb;
    row.silClips = g_gr[g].sil.clips;
    row.silStuck = g_gr[g].sil.stuck;
    row.recValid = g_gr[g].recValid;
    row.recPeakDb = g_gr[g].rec.peakDb;
    row.recRmsDb = g_gr[g].rec.rmsDb;
    row.recDcLsb = g_gr[g].rec.dcLsb;
    row.recClips = g_gr[g].rec.clips;
    row.recSecs = g_gr[g].recSecs;
  }
  r.recValid = g_lastRec.valid;
  r.recSamples = g_lastRec.samples;
  r.recSecs = static_cast<float>(g_lastRec.samples) / static_cast<float>(app::g_sampleRate);
  r.recChunk = g_lastRec.chunk;
  r.recWrites = g_lastRec.writes;
  r.recWriteAvgUs = g_lastRec.writeAvgUs;
  r.recWriteMaxUs = g_lastRec.writeMaxUs;
  r.recRxOverflows = g_lastRec.rxOvf;
  r.recRingDrops = g_lastRec.ringDrops;
  r.recBlocks = g_lastRec.blocks;
  r.recMaxRxGapUs = g_lastRec.maxGapUs;
  r.recEraseMs = g_lastRec.eraseMs;
  r.recBlockPeriodUs = duplex::stats().blockPeriodUs;
  r.benchRows = g_benchRows;
  for (int i = 0; i < g_benchRows && i < kMaxBenchRows; ++i) r.bench[i] = g_rows[i];
  r.missCycPerLine = g_missCyc;
  const float cpu = static_cast<float>(esp_rom_get_cpu_ticks_per_us());
  r.playAvgUs = g_playAvgCyc / cpu;
  r.playMaxUs = static_cast<float>(g_playMaxCyc) / cpu;
  r.playCpuPct = 100.0f * r.playAvgUs / duplex::stats().blockPeriodUs;
  app::fillMemory(&r.heapFree, &r.heapMin, &r.heapLargest, &r.staticDram);
  r.uptimeS = millis() / 1000;
  formatSamplerReport(r, app::reportSink, nullptr);
  if (micmatrix::finished()) micmatrix::report();
}

}  // namespace sampler
}  // namespace hws
