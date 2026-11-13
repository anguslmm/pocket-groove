// Pocket Groove -- on-device audio profiler (env:profile, env:profile-ram).
//
// Flash it, power on, wait ~10 minutes; nothing needs a key. The LCD says what
// is running; the results go to USB serial (115200) as one block of
// fixed-format lines between "===== PG-PROFILE BEGIN" and "===== PG-PROFILE
// END =====", printed as each section finishes and then repeated every 20 s,
// so the monitor can be opened at any time.
//
// What it measures (all with the CPU cycle counter of core 0, 240 MHz):
//  SYS/SYM   cache geometry, flash mode, where hot symbols live (flash/IRAM/DRAM)
//  MISS/FPU  flash-cache line refill cost; FPU cost with normal vs subnormal operands
//  LIBM      warm and cold (caches invalidated) cost of the libm calls the engine makes
//  per rate (44.1 kHz, then 32 kHz; the voices re-init with that rate's tables):
//   RUN/TOP  per-block distribution of the full instrument (offline: render called in a
//            loop on core 0, no I2S), 10 worst blocks with step position / notes / voices
//   VOX      each voice's render cost INSIDE the engine (a timing proxy wraps each
//            voice), noteOn and setParams cost
//   WARM     the same render calls re-run on a byte copy of the voice right after the
//            real one (warm cache) and, sampled, after invalidating both caches (cold)
//   OWN      engine's own work = total - voices (mixer, ducker, FX, master, sequencer)
//   FXE      delay / reverb cost in the engine (own with sends vs without)
//   FXI      delay / reverb / master alone (warm, cold)
//   CURVE    total vs number of voices allowed to sound (1, 2, 4, 8): the cache test
//   MOTION   macro motion (engine.h "MACRO MOTION"): ACIDJAM with the bass on ACID and
//            the lead on HANDS, every macro standing (RUN mot-still) against two
//            macros of each sweeping all the time (RUN mot-sweep): the difference as
//            a share of one core, and the worst blocks of the sweep
//   SAMPLER  ACIDJAM with PERC on CHOP, then STUTTER (stage F1): the sampler voice's own
//            render cost while it sounds (cycles per frame and % of a core), its noteOn
//   TAIL     cost of each voice over its decay after one note, and of the engine after
//            Stop (FX tails): a rise at the end = denormals
//   LIVE     the real audio task + I2S for 20 s per configuration (DMA 4/8, UI on/off,
//            serial stats on/off): render vs time blocked in the I2S write, gaps, and a
//            log of slow blocks with whether a UI frame was being drawn at the time.
//
// Uses src/platform only; the UI load on core 1 is the real pg::ui::Ui drawing the
// real screen (full redraw at 30 fps when "UI on").
#include <Arduino.h>

#include <esp_cpu.h>
#include <esp_flash.h>
#include <esp_heap_caps.h>
#include <esp_memory_utils.h>
#include <esp_timer.h>
#include <esp32s3/rom/cache.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <perfmon.h>
#include <sdkconfig.h>

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <new>

#include "../platform/audio_out.h"
#include "../platform/display.h"
#include "../platform/sample_flash.h"
#include "../platform/system.h"
#include "pg/bass/bass_math.h"
#include "pg/demo/demo.h"
#include "pg/engine/delay.h"
#include "pg/engine/master.h"
#include "pg/engine/reverb.h"
#include "pg/instrument/instrument.h"
#include "pg/lead/lead_tables.h"
#include "pg/macro/macro.h"
#include "pg/ui/gfx.h"
#include "pg/ui/ui.h"

// Mangled names of a few hot functions, for their addresses only (weak: never called).
extern "C" void _ZN2pg6Engine7processEPsi() __attribute__((weak));
extern "C" void _ZN2pg4lead9LeadVoice6renderEPfS2_i() __attribute__((weak));
extern "C" void _ZN2pg4bass9BassVoice6renderEPfS2_i() __attribute__((weak));
extern "C" void _ZN2pg5drums9KickVoice6renderEPfS2_i() __attribute__((weak));
extern "C" void _ZN2pg13engine_detail6Reverb3runILb1EEEvPKfPfS5_i() __attribute__((weak));
extern "C" int _iram_text_start, _iram_text_end, _data_start, _bss_end;

#if !defined(PG_AUDIO_RAM) || PG_AUDIO_RAM == 0
#define PG_PROFILE_UI 1
#else
#define PG_PROFILE_UI 0
#endif

size_t getArduinoLoopTaskStackSize() { return 12 * 1024; }

namespace {

namespace audio = plat::audio;
namespace sys = plat::sys;
namespace disp = plat::display;

#ifdef PG_AUDIO_RAM
constexpr int kTier = PG_AUDIO_RAM;
#else
constexpr int kTier = 0;
#endif

inline uint32_t cyc() { return esp_cpu_get_cycle_count(); }

pg::Instrument g_inst;
// The real UI as core-1 load. Not in profile-ram: the 45 KB of IRAM leave no
// room for the UI's 49 KB (framebuffer + Ui); there "UI on" runs are skipped.
constexpr bool kHaveUi = kTier == 0;
#if PG_PROFILE_UI
pg::ui::Ui g_ui;
pg::Framebuffer g_fb;
#endif

// ---------------------------------------------------------------- report ----
constexpr size_t kRepCap = 20 * 1024;
char* g_rep = nullptr;   // heap (kRepCap), allocated at boot
size_t g_repLen = 0;
volatile bool g_done = false;

void rp(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void rp(const char* fmt, ...) {
  char line[256];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  if (n < 0) return;
  if (n >= static_cast<int>(sizeof(line))) n = sizeof(line) - 1;
  if (g_rep && g_repLen + n + 1 < kRepCap) {
    memcpy(g_rep + g_repLen, line, n);
    g_repLen += n;
    g_rep[g_repLen] = 0;
  }
  sys::serialWrite(line, n);   // live progress too
}

// x10 fixed point "cycles per frame" without float printf
struct Dec { int i, f; };
Dec dec(uint64_t num, uint64_t den) {
  if (den == 0) return {0, 0};
  const uint64_t v = (num * 10 + den / 2) / den;
  return {static_cast<int>(v / 10), static_cast<int>(v % 10)};
}
#define DF(d) (d).i, (d).f

// ---------------------------------------------------------------- LCD / UI (core 1) ----
volatile int g_uiMode = 0;          // 0 frozen (nothing drawn), 1 full redraw at 30 fps
volatile bool g_uiBusy = false;     // a UI frame is being computed / pushed right now
volatile uint32_t g_uiFrames = 0, g_uiFrameCycMax = 0;
volatile uint64_t g_uiFrameCycSum = 0;
volatile bool g_serialStats = false;
char g_status[64] = "STARTING";
volatile bool g_statusDirty = true;

void setStatus(const char* s) {
  strlcpy(g_status, s, sizeof(g_status));
  g_statusDirty = true;
}

void drawStatusScreen() {
  disp::directText(4, 4, "POCKET GROOVE PROFILER          ", 0xFFE0, 1);
  disp::directText(4, 18, "RESULTS: USB SERIAL 115200       ", 0xFFFF, 1);
  disp::directText(4, 30, "NO KEYS NEEDED. ~10 MIN.         ", 0xFFFF, 1);
  char b[48];
  snprintf(b, sizeof(b), "%-34.34s", g_status);
  disp::directText(4, 50, b, 0x07E0, 1);
  snprintf(b, sizeof(b), "%-34s", kTier ? "BUILD: HOT PATH IN IRAM" : "BUILD: HOT PATH IN FLASH");
  disp::directText(4, 64, b, 0x07FF, 1);
}

// ---------------------------------------------------------------- probes ----
// A Voice that forwards to the real voice and times it. In warm mode it also
// re-renders a byte copy of the voice's pre-render state right after the real
// render (warm), and every 32nd call after invalidating both caches (cold).
constexpr size_t kMaxVoice = sizeof(pg::lead::LeadVoice) > sizeof(pg::bass::BassVoice)
                                 ? sizeof(pg::lead::LeadVoice) : sizeof(pg::bass::BassVoice);
alignas(16) uint8_t s_snap[kMaxVoice + 64], s_post[kMaxVoice + 64], s_scr[kMaxVoice + 64];

struct VStat {
  uint64_t rSum, pSum, nSum, wSum, cSum, wcSum;
  uint32_t rN, pN, nN, wN, cN, rMax, pMax, nMax;
  uint32_t copyFaults;
};

class Probe final : public pg::Voice {
 public:
  pg::Voice* in = nullptr;
  size_t size = 0;
  bool gate = true;          // false: noteOn swallowed (voice never sounds)
  bool warm = false;
  VStat st{};
  uint32_t qCyc = 0, qExcl = 0, qNotes = 0;

  void init(float sr) override { in->init(sr); }
  const pg::ParamInfo* paramInfo(int i) const override { return in->paramInfo(i); }
  void setParams(const uint8_t* p) override {
    const uint32_t t0 = cyc();
    in->setParams(p);
    const uint32_t dt = cyc() - t0;
    qCyc += dt; st.pSum += dt; ++st.pN; if (dt > st.pMax) st.pMax = dt;
  }
  void setParamsFine(const float* p) override {   // macro motion: counted with setParams
    const uint32_t t0 = cyc();
    in->setParamsFine(p);
    const uint32_t dt = cyc() - t0;
    qCyc += dt; st.pSum += dt; ++st.pN; if (dt > st.pMax) st.pMax = dt;
  }
  void setTempo(float b) override { in->setTempo(b); }
  void noteOn(const pg::NoteEvent& e) override {
    if (!gate) return;
    const uint32_t t0 = cyc();
    in->noteOn(e);
    const uint32_t dt = cyc() - t0;
    qCyc += dt; st.nSum += dt; ++st.nN; if (dt > st.nMax) st.nMax = dt;
    ++qNotes;
  }
  void noteOff(uint8_t n) override { in->noteOff(n); }
  void choke() override { in->choke(); }
  void render(float* L, float* R, int n) override {
    const uint32_t tIn = cyc();
    if (warm) memcpy(s_snap, static_cast<void*>(in), size);
    const uint32_t t0 = cyc();
    in->render(L, R, n);
    const uint32_t dt = cyc() - t0;
    qCyc += dt; st.rSum += dt; ++st.rN; if (dt > st.rMax) st.rMax = dt;
    if (warm) {
      float tl[pg::kQuantum], tr[pg::kQuantum];
      memcpy(s_post, static_cast<void*>(in), size);
      pg::Voice* cp = reinterpret_cast<pg::Voice*>(s_scr);
      if ((st.rN & 31) == 1) {
        memcpy(s_scr, s_snap, size);
        Cache_Invalidate_ICache_All();
        Cache_Invalidate_DCache_All();
        const uint32_t c0 = cyc();
        cp->render(tl, tr, n);
        const uint32_t cd = cyc() - c0;
        memcpy(s_scr, s_snap, size);
        const uint32_t w0 = cyc();
        cp->render(tl, tr, n);
        const uint32_t wd = cyc() - w0;
        st.cSum += cd; st.wcSum += wd; ++st.cN;
      }
      memcpy(s_scr, s_snap, size);
      const uint32_t w0 = cyc();
      cp->render(tl, tr, n);
      st.wSum += cyc() - w0; ++st.wN;
      if (memcmp(s_post, static_cast<void*>(in), size) != 0) {   // the copy wrote through a pointer
        memcpy(static_cast<void*>(in), s_post, size);
        ++st.copyFaults;
      }
    }
    qExcl += (cyc() - tIn) - dt;
  }
  bool active() const override { return in->active(); }
  bool mono() const override { return in->mono(); }
  void setMonoOutput(bool m) override { in->setMonoOutput(m); }
  void setSampleSlot(uint8_t slot) override { in->setSampleSlot(slot); }
};

Probe g_probe[pg::kNumTracks];
Probe g_smpProbe;   // the PERC track's sampler (VOICE KINDS), when a sampler instrument plays
const char* const kTrackName[pg::kNumTracks] = {"kick", "snare", "clap", "hatC", "hatO", "perc", "bass", "lead"};

size_t voiceSize(int t) {
  switch (t) {
    case pg::kTrackKick: return sizeof(pg::drums::KickVoice);
    case pg::kTrackSnare: return sizeof(pg::drums::SnareVoice);
    case pg::kTrackClap: return sizeof(pg::drums::ClapVoice);
    case pg::kTrackHatClosed:
    case pg::kTrackHatOpen: return sizeof(pg::drums::HatVoice);
    case pg::kTrackPerc: return sizeof(pg::drums::PercVoice);
    case pg::kTrackBass: return sizeof(pg::bass::BassVoice);
    default: return sizeof(pg::lead::LeadVoice);
  }
}

// ---------------------------------------------------------------- perfmon ----
bool g_pmOk = false;
void pmStart() {
  if (!g_pmOk) return;
  xtensa_perfmon_stop();
  xtensa_perfmon_reset(0);
  xtensa_perfmon_reset(1);
  xtensa_perfmon_start();
}
void pmRead(uint64_t* i, uint64_t* d) {
  if (!g_pmOk) return;
  xtensa_perfmon_stop();
  *i += xtensa_perfmon_value(0);
  *d += xtensa_perfmon_value(1);
}

void yieldNow() { vTaskDelay(1); }

// ---------------------------------------------------------------- engine runs ----
struct RunCfg {
  const char* tag;
  int demo, pattern, bars;
  uint8_t gateMask;         // bit t: track t may sound
  bool warm, noDelay, noRev;
  int uiMode;
  int tailSec;              // after the bars: Stop and render this long (TAIL buckets of 1 s)
  int leadChord = -1;       // >= 0: every lead step plays this ChordType (0 = single note)
  int motion = 0;           // 1: bass ACID + lead HANDS bound, still; 2: the same with two macros of each sweeping
  const char* percInst = nullptr;   // PERC plays this instrument (a sampler one: CHOP, TAPE, STUTTER)
};

constexpr int kBins = 1024, kBinCyc = 2048;   // block histogram: 2048-cycle bins up to 2.1 M
struct Top { uint32_t cyc, blk; int8_t bq; uint8_t notes, act, since; };
struct RunRes {
  uint32_t hist[kBins];
  uint32_t n, max, over, firstBlk;
  uint64_t sum;
  Top top[10];
  uint64_t ownSum, ownBndSum; uint32_t ownN, ownBndN, ownMax, ownBndMax;
  uint64_t istall, dstall, pmCyc;
  uint64_t tail[32]; uint32_t tailQ[32];
  uint32_t blockCyc;        // nominal period
  int bpm;
};
RunRes g_res;
int g_rate = 44100;

const pg::macro::MacroSet* byName(int track, const char* name) {
  for (int i = 0; i < pg::macro::instrumentCount(track); ++i)
    if (strcmp(pg::macro::instrumentAt(track, i)->name, name) == 0) return pg::macro::instrumentAt(track, i);
  return nullptr;
}

// MOTION runs: one UI frame's macro moves (every 33 ms), as a held key or tilt
// would send them: OPEN and BITE of the bass, OPEN and PUSH of the lead, four
// triangles over the whole range, out of phase, with the default glide.
void motionFrame(int frame) {
  for (int k = 0; k < 4; ++k) {
    const int ph = (frame * 4 + k * 70) % 510;
    const int v = ph < 255 ? ph : 509 - ph;
    g_inst.engine().post(pg::setMacroCommand(k < 2 ? pg::kTrackBass : pg::kTrackLead, k & 1, static_cast<uint8_t>(v)));
  }
}

void setupProject(int demo, int pattern, const RunCfg* c) {
  pg::Engine& eng = g_inst.engine();
  pg::Project& p = g_inst.project();
  g_inst.init(static_cast<float>(g_rate), true);
  for (int t = 0; t < pg::kNumTracks; ++t) {
    g_probe[t].in = g_inst.voice(t);
    g_probe[t].size = voiceSize(t);
    g_probe[t].gate = true;
    g_probe[t].warm = false;
    g_probe[t].st = VStat{};
    eng.setVoice(t, &g_probe[t]);
  }
  g_smpProbe.in = &g_inst.sampler();
  g_smpProbe.size = sizeof(pg::sampler::SamplerVoice);
  g_smpProbe.gate = true;
  g_smpProbe.warm = false;
  g_smpProbe.st = VStat{};
  eng.setAltVoice(pg::kTrackPerc, pg::kVoiceSampler, &g_smpProbe);
  pg::demo::build(eng, p, demo);
  if (c) {
    for (int t = 0; t < pg::kNumTracks; ++t) {
      if (c->noDelay) p.tracks[t].params[pg::kMixDelaySend] = 0;
      if (c->noRev) p.tracks[t].params[pg::kMixReverbSend] = 0;
      g_probe[t].gate = (c->gateMask >> t) & 1;
      g_probe[t].warm = c->warm;
    }
    if (c->leadChord >= 0)
      for (auto& pat : p.patterns)
        for (auto& st : pat.tracks[pg::kTrackLead].steps) st.setChord(static_cast<uint8_t>(c->leadChord));
  }
  if (c && c->percInst) {
    // A chop on every off-beat 8th at least, so the sampler sounds through the run.
    pg::TrackPattern& tp = p.patterns[pattern].tracks[pg::kTrackPerc];
    const int len = tp.length ? tp.length : 16;
    for (int i = 2; i < len && i < pg::kMaxSteps; i += 4)
      if (!tp.steps[i].on()) {
        tp.steps[i] = pg::kDefaultStep;
        tp.steps[i].addFlags(pg::kStepOn);
      }
  }
  p.songMode = 0;
  eng.sanitizeProject(p);
  if (c && c->motion) {
    // Macro motion (engine.h "MACRO MOTION"): the bass and the lead play instruments.
    const uint8_t bassPos[4] = {102, 60, 0, 128}, leadPos[4] = {77, 60, 0, 128};
    const pg::macro::MacroSet* acid = byName(pg::kTrackBass, "ACID");
    const pg::macro::MacroSet* hands = byName(pg::kTrackLead, "HANDS");
    if (acid) eng.post(pg::setInstrumentCommand(pg::kTrackBass, acid->id, bassPos));
    if (hands) eng.post(pg::setInstrumentCommand(pg::kTrackLead, hands->id, leadPos));
  }
  if (c && c->percInst) {
    const uint8_t mid[4] = {128, 128, 128, 128};
    if (const pg::macro::MacroSet* m = byName(pg::kTrackPerc, c->percInst))
      eng.post(pg::setInstrumentCommand(pg::kTrackPerc, m->id, mid));
  }
  pg::Command q{};
  q.type = pg::CmdType::QueuePattern;
  q.a = static_cast<uint8_t>(pattern);
  eng.post(q);
  pg::Command play{};
  play.type = pg::CmdType::Play;
  eng.post(play);
}

void addTop(RunRes& r, const Top& t) {
  int k = 9;
  if (t.cyc <= r.top[k].cyc) return;
  while (k > 0 && r.top[k - 1].cyc < t.cyc) { r.top[k] = r.top[k - 1]; --k; }
  r.top[k] = t;
}

uint32_t pctl(const RunRes& r, int pm) {   // per-mille percentile, bin upper edge
  const uint64_t want = (static_cast<uint64_t>(r.n) * pm + 999) / 1000;
  uint64_t acc = 0;
  for (int i = 0; i < kBins; ++i) {
    acc += r.hist[i];
    if (acc >= want) return static_cast<uint32_t>(i + 1) * kBinCyc;
  }
  return r.max;
}

int16_t g_buf[audio::kBlockFrames * 2];

void runEngine(const RunCfg& c) {
  RunRes& r = g_res;
  memset(&r, 0, sizeof(r));
  g_uiMode = 0;
  vTaskDelay(pdMS_TO_TICKS(60));            // let a UI frame in flight finish
  setupProject(c.demo, c.pattern, &c);
  r.bpm = g_inst.project().globals[pg::kGTempo];
  if (r.bpm < 40) r.bpm = 120;
  r.blockCyc = static_cast<uint32_t>(static_cast<uint64_t>(audio::kBlockFrames) * 240000000ull / g_rate);
  const uint64_t frames = static_cast<uint64_t>(c.bars) * 16ull * static_cast<uint64_t>(g_rate) * 15ull / r.bpm;
  const uint32_t blocks = static_cast<uint32_t>(frames / audio::kBlockFrames);
  const uint32_t tailBlocks = static_cast<uint32_t>(c.tailSec) * g_rate / audio::kBlockFrames;
  const uint32_t blocksPerSec = g_rate / audio::kBlockFrames;
  g_uiMode = c.uiMode;
  uint32_t since = 255;
  const uint32_t uiFrame = static_cast<uint32_t>(g_rate) * 33 / 1000;
  uint32_t nextUi = 0;
  int uiCount = 0;
  for (uint32_t b = 0; b < blocks + tailBlocks; ++b) {
    if (c.motion == 2 && b * audio::kBlockFrames >= nextUi) {
      motionFrame(uiCount++);
      nextUi += uiFrame;
    }
    if (b == blocks && tailBlocks) {
      pg::Command s{};
      s.type = pg::CmdType::Stop;
      g_inst.engine().post(s);
    }
    uint32_t blk = 0, notes = 0, act = 0;
    int bq = -1;
    uint32_t sinceAtStart = since;
    pmStart();
    const uint32_t pm0 = cyc();
    for (int q = 0; q < audio::kBlockFrames / pg::kQuantum; ++q) {
      for (auto& p : g_probe) { p.qCyc = 0; p.qExcl = 0; p.qNotes = 0; }
      g_smpProbe.qCyc = 0; g_smpProbe.qExcl = 0; g_smpProbe.qNotes = 0;
      const uint32_t t0 = cyc();
      g_inst.process(g_buf + 2 * pg::kQuantum * q, pg::kQuantum);
      uint32_t dt = cyc() - t0;
      uint32_t vc = 0, ex = 0, qn = 0, a = 0;
      for (auto& p : g_probe) { vc += p.qCyc; ex += p.qExcl; qn += p.qNotes; a += p.active() ? 1 : 0; }
      vc += g_smpProbe.qCyc; ex += g_smpProbe.qExcl; qn += g_smpProbe.qNotes;
      dt = dt > ex ? dt - ex : 0;
      const uint32_t own = dt > vc ? dt - vc : 0;
      blk += dt;
      notes += qn;
      if (a > act) act = a;
      if (qn) { if (bq < 0) bq = q; since = 0; } else if (since < 255) ++since;
      if (b < blocks) {
        r.ownSum += own; ++r.ownN; if (own > r.ownMax) r.ownMax = own;
        if (qn) { r.ownBndSum += own; ++r.ownBndN; if (own > r.ownBndMax) r.ownBndMax = own; }
      }
    }
    const uint32_t pmc = cyc() - pm0;
    if (b < blocks) {
      pmRead(&r.istall, &r.dstall);
      r.pmCyc += pmc;
      if (b == 0) r.firstBlk = blk;
      if (b >= 8) {   // the first blocks after Play are reported separately (cold start)
        r.hist[blk / kBinCyc < kBins ? blk / kBinCyc : kBins - 1]++;
        ++r.n; r.sum += blk; if (blk > r.max) r.max = blk;
        if (blk > r.blockCyc) ++r.over;
        addTop(r, Top{blk, b, static_cast<int8_t>(bq), static_cast<uint8_t>(notes > 255 ? 255 : notes),
                      static_cast<uint8_t>(act), static_cast<uint8_t>(sinceAtStart)});
      }
    } else {
      if (g_pmOk) xtensa_perfmon_stop();
      const uint32_t sec = (b - blocks) / blocksPerSec;
      if (sec < 32) { r.tail[sec] += blk; r.tailQ[sec] += audio::kBlockFrames; }
    }
    if ((b & 7) == 7) yieldNow();
  }
  g_uiMode = 0;
}

// cycles/frame summary of the last run
void printRun(const RunCfg& c) {
  const RunRes& r = g_res;
  const uint64_t fr = static_cast<uint64_t>(r.n) * audio::kBlockFrames;
  rp("RUN %s %d/P%d %dbpm bars %d blocks %lu | c/f mean %d.%d p50 %d.%d p90 %d.%d p99 %d.%d max %d.%d | "
     "load %lu%% maxblk %luus over-period %lu first %d.%d\n",
     c.tag, c.demo, c.pattern + 1, r.bpm, c.bars, static_cast<unsigned long>(r.n), DF(dec(r.sum, fr)),
     DF(dec(pctl(r, 500), audio::kBlockFrames)), DF(dec(pctl(r, 900), audio::kBlockFrames)),
     DF(dec(pctl(r, 990), audio::kBlockFrames)), DF(dec(r.max, audio::kBlockFrames)),
     static_cast<unsigned long>(r.n ? (r.sum * 100 / r.n) / r.blockCyc : 0),
     static_cast<unsigned long>(r.max / 240), static_cast<unsigned long>(r.over),
     DF(dec(r.firstBlk, audio::kBlockFrames)));
}

void printRunDetail(const RunCfg& c) {
  const RunRes& r = g_res;
  rp("HIST %s c/f:", c.tag);   // coarse: 10 bands
  {
    char b[200];
    int n = 0;
    const uint32_t edges[] = {500, 1000, 1500, 2000, 3000, 4000, 6000, 8000, 12000, 0xFFFFFFFF};
    uint32_t cnt[10] = {};
    for (int i = 0; i < kBins; ++i) {
      const uint32_t cpf = static_cast<uint32_t>(i) * kBinCyc / audio::kBlockFrames;
      int k = 0;
      while (cpf >= edges[k]) ++k;
      cnt[k] += r.hist[i];
    }
    const char* nm[] = {"<500", "<1k", "<1.5k", "<2k", "<3k", "<4k", "<6k", "<8k", "<12k", ">=12k"};
    for (int k = 0; k < 10; ++k) n += snprintf(b + n, sizeof(b) - n, " %s:%lu", nm[k], static_cast<unsigned long>(cnt[k]));
    rp("%s\n", b);
  }
  for (int k = 0; k < 6 && r.top[k].cyc; ++k) {
    const Top& t = r.top[k];
    rp("TOP %s #%d blk %lu c/f %d.%d bnd %s%d notes %d voices %d since %d\n", c.tag, k + 1,
       static_cast<unsigned long>(t.blk), DF(dec(t.cyc, audio::kBlockFrames)), t.bq >= 0 ? "@q" : "none",
       t.bq >= 0 ? t.bq : 0, t.notes, t.act, t.since);
  }
  const uint64_t fr = static_cast<uint64_t>(r.ownN) * pg::kQuantum;
  rp("OWN %s engine-own c/f mean %d.%d max-q %d.%d | step quanta: n %lu mean %d.%d max %d.%d\n", c.tag,
     DF(dec(r.ownSum, fr)), DF(dec(r.ownMax, pg::kQuantum)), static_cast<unsigned long>(r.ownBndN),
     DF(dec(r.ownBndSum, static_cast<uint64_t>(r.ownBndN) * pg::kQuantum)), DF(dec(r.ownBndMax, pg::kQuantum)));
  if (g_pmOk && r.pmCyc)
    rp("STALL %s I-stall %lu%% D-stall %lu%% of render cycles (xtensa perfmon, all masks)\n", c.tag,
       static_cast<unsigned long>(r.istall * 100 / r.pmCyc), static_cast<unsigned long>(r.dstall * 100 / r.pmCyc));
  const uint64_t allFr = static_cast<uint64_t>(r.ownN) * pg::kQuantum;
  for (int t = 0; t < pg::kNumTracks; ++t) {
    const VStat& v = g_probe[t].st;
    rp("VOX %s %-5s c/f %d.%d act %lu%% avg %d.%d maxq %d.%d | on n%lu %lu/%lu | sP %lu/%lu\n",
       c.tag, kTrackName[t], DF(dec(v.rSum, static_cast<uint64_t>(v.rN) * pg::kQuantum)),
       static_cast<unsigned long>(r.ownN ? static_cast<uint64_t>(v.rN) * 100 / r.ownN : 0), DF(dec(v.rSum, allFr)),
       DF(dec(v.rMax, pg::kQuantum)), static_cast<unsigned long>(v.nN),
       static_cast<unsigned long>(v.nN ? v.nSum / v.nN : 0), static_cast<unsigned long>(v.nMax),
       static_cast<unsigned long>(v.pN ? v.pSum / v.pN : 0), static_cast<unsigned long>(v.pMax));
  }
}

// ---------------------------------------------------------------- microbenchmarks ----
void flushCaches() {
  Cache_Invalidate_ICache_All();
  Cache_Invalidate_DCache_All();
}

volatile float g_vf[8] = {0.3f, 1.7f, 0.9f, 2.5f, 1e-40f, 0.5f, 3.0f, 0.25f};
volatile float g_sink;

void benchMiss() {
  const pg::lead::Tables& tb = pg::lead::tablesFor(44100.0f);
  const volatile uint8_t* p = reinterpret_cast<const volatile uint8_t*>(tb.saw[0].d);
  uint32_t best[2] = {0xFFFFFFFF, 0xFFFFFFFF};
  for (int rep = 0; rep < 8; ++rep) {
    for (int pass = 0; pass < 2; ++pass) {
      if (pass == 0) Cache_Invalidate_DCache_All();
      uint32_t s = 0;
      const uint32_t t0 = cyc();
      for (int i = 0; i < 128; ++i) s += p[i * 32];
      const uint32_t dt = cyc() - t0;
      g_sink = static_cast<float>(s);
      if (dt < best[pass]) best[pass] = dt;
    }
  }
  rp("MISS D-cache line refill from flash: cold %lu warm %lu cyc per 128 lines -> %lu cyc per 32-byte line\n",
     static_cast<unsigned long>(best[0]), static_cast<unsigned long>(best[1]),
     static_cast<unsigned long>((best[0] - best[1]) / 128));
}

__attribute__((noinline)) uint32_t fpuLoop(float y, float a, float b, int n) {
  const uint32_t t0 = cyc();
  for (int i = 0; i < n; ++i) y = y * a + b;
  const uint32_t dt = cyc() - t0;
  g_sink = y;
  return dt;
}
__attribute__((noinline)) uint32_t mulLoop(float y, float a, int n) {
  const uint32_t t0 = cyc();
  float acc = 0.0f;
  for (int i = 0; i < n; ++i) { acc += y * a; y = y * 1.0000001f; }
  const uint32_t dt = cyc() - t0;
  g_sink = acc;
  return dt;
}
__attribute__((noinline)) uint32_t divLoop(float y, float a, int n) {
  const uint32_t t0 = cyc();
  float acc = 0.0f;
  for (int i = 0; i < n; ++i) { acc += y / a; a += 1.0e-7f; }
  const uint32_t dt = cyc() - t0;
  g_sink = acc;
  return dt;
}

void benchFpu() {
  const int n = 4096;
  const float a = g_vf[5], tiny = g_vf[4];   // 0.5, 1e-40 (subnormal)
  // y*0.5 + b: normal operands vs a subnormal fixed point (b = 1e-40 keeps y subnormal)
  const uint32_t mn = fpuLoop(1.0f, a, 0.25f, n);
  const uint32_t ms = fpuLoop(tiny, a, tiny, n);
  const uint32_t un = mulLoop(1.0f, 0.75f, n), us = mulLoop(tiny, 0.75f, n);
  const uint32_t dn = divLoop(1.0f, 3.0f, n), ds = divLoop(tiny, 3.0f, n);
  rp("FPU cyc/iter: madd normal %d.%d subnormal %d.%d | mul+add normal %d.%d subnormal %d.%d | div normal %d.%d "
     "subnormal %d.%d  (subnormal >> normal = denormal penalty)\n",
     DF(dec(mn, n)), DF(dec(ms, n)), DF(dec(un, n)), DF(dec(us, n)), DF(dec(dn, n)), DF(dec(ds, n)));
}

template <typename F>
void benchFn(const char* name, F f, char* line, int& len, int cap) {
  // warm: 256 calls with varying arguments; cold: 8 x (flush both caches, 1 call)
  uint32_t t0 = cyc();
  float acc = 0.0f;
  for (int i = 0; i < 256; ++i) acc += f(g_vf[i & 7] + static_cast<float>(i) * 0.001f);
  const uint32_t warm = (cyc() - t0) / 256;
  uint32_t cold = 0;
  for (int i = 0; i < 8; ++i) {
    flushCaches();
    t0 = cyc();
    acc += f(g_vf[i & 7] + 0.37f);
    cold += cyc() - t0;
  }
  g_sink = acc;
  len += snprintf(line + len, cap - len, " %s %lu/%lu", name, static_cast<unsigned long>(warm),
                  static_cast<unsigned long>(cold / 8));
}

void benchLibm() {
  char line[240];
  int len = 0;
  benchFn("sinf", [](float x) { return sinf(x); }, line, len, sizeof(line));
  benchFn("cosf", [](float x) { return cosf(x); }, line, len, sizeof(line));
  benchFn("expf", [](float x) { return expf(x); }, line, len, sizeof(line));
  benchFn("powf", [](float x) { return powf(2.0f, x); }, line, len, sizeof(line));
  benchFn("sqrtf", [](float x) { return sqrtf(x); }, line, len, sizeof(line));
  benchFn("tanhf", [](float x) { return tanhf(x); }, line, len, sizeof(line));
  benchFn("log2f", [](float x) { return log2f(x); }, line, len, sizeof(line));
  benchFn("div", [](float x) { return 1.0f / (x + g_vf[6]); }, line, len, sizeof(line));
  rp("LIBM cyc per call warm/cold:%s\n", line);
}

// p-lock burst: setParams with every sound param changed, and one noteOn, per voice (warm/cold)
void benchParamBurst() {
  setupProject(0, 0, nullptr);
  pg::Project& p = g_inst.project();
  char line[240];
  int len = 0;
  for (int t = 0; t < pg::kNumTracks; ++t) {
    pg::Voice* v = g_inst.voice(t);
    uint8_t a[pg::kSoundParams], b[pg::kSoundParams];
    for (int i = 0; i < pg::kSoundParams; ++i) {
      a[i] = p.tracks[t].params[i];
      const pg::ParamInfo* pi = v->paramInfo(i);
      b[i] = (pi && a[i] < pi->max) ? static_cast<uint8_t>(a[i] + 1) : (pi && a[i] > pi->min ? a[i] - 1 : a[i]);
    }
    v->setParams(a);
    uint32_t warm = 0, cold = 0;
    for (int k = 0; k < 16; ++k) {
      const uint32_t t0 = cyc();
      v->setParams((k & 1) ? a : b);
      warm += cyc() - t0;
    }
    for (int k = 0; k < 4; ++k) {
      flushCaches();
      const uint32_t t0 = cyc();
      v->setParams((k & 1) ? a : b);
      cold += cyc() - t0;
    }
    pg::NoteEvent e{static_cast<uint8_t>(t == pg::kTrackBass ? 45 : (t == pg::kTrackLead ? 57 : 60)), 100, false,
                    false, 0};
    uint32_t nw = 0, nc = 0;
    for (int k = 0; k < 4; ++k) {
      flushCaches();
      uint32_t t0 = cyc();
      v->noteOn(e);
      nc += cyc() - t0;
      t0 = cyc();
      v->noteOn(e);
      nw += cyc() - t0;
      v->choke();
    }
    len += snprintf(line + len, sizeof(line) - len, " %s %lu/%lu,%lu/%lu", kTrackName[t],
                    static_cast<unsigned long>(warm / 16), static_cast<unsigned long>(cold / 4),
                    static_cast<unsigned long>(nw / 4), static_cast<unsigned long>(nc / 4));
    if (t == 3) {
      rp("BURST cyc warm/cold: setParams(all changed),noteOn:%s\n", line);
      len = 0;
    }
  }
  rp("BURST cyc warm/cold: setParams(all changed),noteOn:%s\n", line);
}

// ---------------------------------------------------------------- FX alone ----
void benchFxAlone() {
  float in[8], out[8];
  uint32_t seed = 1;
  auto noise = [&]() { seed = seed * 1664525u + 1013904223u; return (static_cast<int32_t>(seed) >> 8) * (0.1f / 8388608.0f); };
  const float half = static_cast<float>(g_rate) * 0.5f;
  char line[220];
  int len = 0;
  auto* d = static_cast<pg::engine_detail::Delay*>(heap_caps_malloc(sizeof(pg::engine_detail::Delay), MALLOC_CAP_INTERNAL));
  if (d) {
    memset(static_cast<void*>(d), 0, sizeof(*d));
    d->init(half);
    d->setMono(true);
    d->setTime(half * 0.357f);
    d->setParams(0.45f, 0.3f, 0.02f, 0.5f);
    uint64_t w = 0;
    uint32_t c = 0;
    for (int k = 0; k < 4000; ++k) {
      for (float& x : in) x = noise();
      const bool cold = (k % 500) == 499;
      if (cold) flushCaches();
      const uint32_t t0 = cyc();
      d->processMono(in, out, 8);
      const uint32_t dt = cyc() - t0;
      if (cold) c += dt; else w += dt;
      if ((k & 255) == 255) yieldNow();
    }
    len += snprintf(line + len, sizeof(line) - len, " delay %d.%d/%d.%d", DF(dec(w, 3992ull * 16)), DF(dec(c, 8ull * 16)));
    heap_caps_free(d);
  } else {
    len += snprintf(line + len, sizeof(line) - len, " delay n/a(no RAM)");
  }
  auto* rv = static_cast<pg::engine_detail::Reverb*>(heap_caps_malloc(sizeof(pg::engine_detail::Reverb), MALLOC_CAP_INTERNAL));
  if (rv) {
    memset(static_cast<void*>(rv), 0, sizeof(*rv));
    rv->init(half);
    rv->setParams(2.0f, 0.4f);
    uint64_t w = 0;
    uint32_t c = 0;
    for (int k = 0; k < 4000; ++k) {
      for (float& x : in) x = noise();
      const bool cold = (k % 500) == 499;
      if (cold) flushCaches();
      const uint32_t t0 = cyc();
      rv->processMono(in, out, 8);
      const uint32_t dt = cyc() - t0;
      if (cold) c += dt; else w += dt;
      if ((k & 255) == 255) yieldNow();
    }
    len += snprintf(line + len, sizeof(line) - len, " reverb %d.%d/%d.%d", DF(dec(w, 3992ull * 16)), DF(dec(c, 8ull * 16)));
    heap_caps_free(rv);
  } else {
    len += snprintf(line + len, sizeof(line) - len, " reverb n/a(no RAM)");
  }
  {
    static pg::engine_detail::MasterBus m;
    m.init(static_cast<float>(g_rate), pg::kQuantum);
    pg::demo::build(g_inst.engine(), g_inst.project(), 0);
    const uint8_t* gl = g_inst.project().globals;
    float bus[pg::kQuantum];
    int16_t o[2 * pg::kQuantum];
    float pk = 0, pk2 = 0;
    uint64_t w = 0;
    uint32_t c = 0;
    for (int k = 0; k < 4000; ++k) {
      for (float& x : bus) x = 3.0f * noise();
      const bool cold = (k % 500) == 499;
      if (cold) flushCaches();
      const uint32_t t0 = cyc();
      m.setParams(gl[pg::kGMasterFilter], gl[pg::kGMasterReso], gl[pg::kGMasterDrive], gl[pg::kGMasterVolume]);
      m.processMono(bus, o, pg::kQuantum, &pk, &pk2);
      const uint32_t dt = cyc() - t0;
      if (cold) c += dt; else w += dt;
      if ((k & 255) == 255) yieldNow();
    }
    len += snprintf(line + len, sizeof(line) - len, " master %d.%d/%d.%d", DF(dec(w, 3992ull * 16)), DF(dec(c, 8ull * 16)));
  }
  rp("FXI alone c/f warm/cold (delay+reverb per output frame at half rate):%s\n", line);
}

// ---------------------------------------------------------------- bass x lead, no engine ----
// The voices alone, outside the engine: lead alone, bass alone, then the two
// interleaved (bass, lead, bass, lead ...), then interleaved with kick + hat.
// If interleaving makes each voice dearer than alone, they evict each other
// from the caches. Notes retrigger every 16th (126 BPM), noteOn not timed.
void benchPair() {
  for (int chordN = 1; chordN <= 4; ++chordN) {
    static const uint8_t kChordFor[5] = {0, 0, 1, 2, 4};   // None, Oct, Fifth, Min: 1..4 notes
    setupProject(0, 0, nullptr);
    pg::Project& p = g_inst.project();
    const int tr[4] = {pg::kTrackBass, pg::kTrackLead, pg::kTrackKick, pg::kTrackHatClosed};
    pg::Voice* v[4];
    for (int i = 0; i < 4; ++i) { v[i] = g_inst.voice(tr[i]); v[i]->setParams(p.tracks[tr[i]].params); }
    const uint32_t step = static_cast<uint32_t>(g_rate) * 15 / 126 / pg::kQuantum;
    auto trig = [&](int i) {
      pg::NoteEvent e{static_cast<uint8_t>(i == 0 ? 45 : (i == 1 ? 57 : 60)), 100, false, false,
                      static_cast<uint8_t>(i == 1 ? kChordFor[chordN] : 0)};
      v[i]->noteOn(e);
    };
    float L[pg::kQuantum], R[pg::kQuantum];
    // mode 0: lead alone, 1: bass alone, 2: bass+lead interleaved, 3: bass+lead+kick+hat
    uint64_t cy[4][4] = {};
    uint32_t nq[4][4] = {};
    for (int mode = 0; mode < 4; ++mode) {
      for (int i = 0; i < 4; ++i) v[i]->choke();
      const bool use[4] = {mode != 0, mode != 1, mode == 3, mode == 3};
      for (uint32_t q = 0; q < 2000; ++q) {
        if (q % step == 0)
          for (int i = 0; i < 4; ++i) if (use[i]) trig(i);
        for (int i = 0; i < 4; ++i) {
          if (!use[i] || !v[i]->active()) continue;
          v[i]->setParams(p.tracks[tr[i]].params);
          const uint32_t t0 = cyc();
          v[i]->render(L, R, pg::kQuantum);
          cy[mode][i] += cyc() - t0;
          ++nq[mode][i];
        }
        if ((q & 255) == 255) yieldNow();
      }
    }
    auto cf = [&](int m, int i) { return dec(cy[m][i], static_cast<uint64_t>(nq[m][i]) * pg::kQuantum); };
    rp("PAIR lead %d-note | alone: lead %d.%d bass %d.%d | interleaved: lead %d.%d bass %d.%d | "
       "+kick+hat: lead %d.%d bass %d.%d kick %d.%d hat %d.%d (c/f while active)\n",
       chordN, DF(cf(0, 1)), DF(cf(1, 0)), DF(cf(2, 1)), DF(cf(2, 0)), DF(cf(3, 1)), DF(cf(3, 0)), DF(cf(3, 2)),
       DF(cf(3, 3)));
  }
}

// ---------------------------------------------------------------- voice tails ----
void voiceTails() {
  for (int t = 0; t < pg::kNumTracks; ++t) {
    setupProject(0, 0, nullptr);
    pg::Voice* v = g_inst.voice(t);
    v->setParams(g_inst.project().tracks[t].params);
    pg::NoteEvent e{static_cast<uint8_t>(t == pg::kTrackBass ? 45 : (t == pg::kTrackLead ? 57 : 60)), 110, false,
                    false, 0};
    v->noteOn(e);
    const uint32_t qPerBucket = static_cast<uint32_t>(g_rate) / 2 / pg::kQuantum;   // 0.5 s
    const uint32_t offAt = static_cast<uint32_t>(g_rate) / 4 / pg::kQuantum;        // noteOff at 0.25 s
    uint64_t bucket[24] = {};
    uint32_t bq[24] = {};
    float L[pg::kQuantum], R[pg::kQuantum];
    uint32_t q = 0, lastActive = 0;
    const uint32_t maxQ = qPerBucket * 24;
    for (; q < maxQ; ++q) {
      if (q == offAt) v->noteOff(e.note);
      if (!v->active()) break;
      v->setParams(g_inst.project().tracks[t].params);
      const uint32_t t0 = cyc();
      v->render(L, R, pg::kQuantum);
      const uint32_t dt = cyc() - t0;
      bucket[q / qPerBucket] += dt;
      bq[q / qPerBucket]++;
      lastActive = q;
      if ((q & 1023) == 1023) yieldNow();
    }
    char line[240];
    int len = snprintf(line, sizeof(line), "TAIL %-5s active %lu ms, c/f per 0.5 s:", kTrackName[t],
                       static_cast<unsigned long>((static_cast<uint64_t>(lastActive + 1) * pg::kQuantum * 1000) / g_rate));
    for (int k = 0; k < 24 && bq[k]; ++k)
      len += snprintf(line + len, sizeof(line) - len, " %d", static_cast<int>(bucket[k] / (static_cast<uint64_t>(bq[k]) * pg::kQuantum)));
    rp("%s%s\n", line, q >= maxQ ? " (still active at 12 s)" : "");
    v->choke();
  }
}

// ---------------------------------------------------------------- live (I2S) ----
struct Slow { uint32_t tMs, rend, wr, gap; uint8_t ui; };
struct LiveRes {
  uint32_t hist[kBins];
  uint32_t n, rMax, gapMax, wMin, slowN;
  uint64_t rSum, wSum;
  Slow slow[16];
};
LiveRes g_live;
volatile bool g_liveOn = false;

void onBlock(const audio::BlockInfo& bi, void*) {
  if (!g_liveOn) return;
  LiveRes& L = g_live;
  ++L.n;
  L.rSum += bi.renderCyc;
  L.wSum += bi.writeCyc;
  if (bi.renderCyc > L.rMax) L.rMax = bi.renderCyc;
  if (bi.writeCyc < L.wMin) L.wMin = bi.writeCyc;
  if (bi.gapCyc > L.gapMax) L.gapMax = bi.gapCyc;
  const uint32_t bin = bi.renderCyc / kBinCyc;
  L.hist[bin < kBins ? bin : kBins - 1]++;
  if (bi.gapCyc > bi.periodCyc + bi.periodCyc / 2 || bi.renderCyc > bi.periodCyc) {
    if (L.slowN < 16) {
      L.slow[L.slowN] = Slow{static_cast<uint32_t>(esp_timer_get_time() / 1000), bi.renderCyc / 240, bi.writeCyc / 240,
                             bi.gapCyc / 240, static_cast<uint8_t>(g_uiBusy ? 1 : 0)};
    }
    ++L.slowN;
  }
}

void renderLive(int16_t* out, int frames, void*) { g_inst.process(out, frames); }

void runLive(const char* tag, int dma, int uiMode, bool serialStats, int seconds) {
  if (uiMode && !kHaveUi) {
    rp("LIVE %s %dHz skipped: no UI in this build (see the profile image)\n", tag, g_rate);
    return;
  }
  g_uiMode = 0;
  vTaskDelay(pdMS_TO_TICKS(60));
  setupProject(0, 0, nullptr);
  memset(&g_live, 0, sizeof(g_live));
  g_live.wMin = 0xFFFFFFFF;
  audio::Config cfg;
  cfg.sampleRate = static_cast<uint32_t>(g_rate);
  cfg.backend = audio::Backend::DirectI2S;
  cfg.core = 0;
  cfg.dmaBufCount = static_cast<uint8_t>(dma);
  cfg.volumeReg = 0x80;   // -31.5 dB: quiet, but audible so the user can tell it is alive
  audio::setBlockObserver(onBlock, nullptr);
  if (!audio::begin(cfg, renderLive, nullptr)) {
    rp("LIVE %s %d FAILED: %s\n", tag, g_rate, audio::lastError());
    return;
  }
  vTaskDelay(pdMS_TO_TICKS(300));   // DMA ring filled, underrun counter armed
  audio::resetStats();
  vTaskDelay(pdMS_TO_TICKS(20));
  const uint32_t uf0 = g_uiFrames;
  g_uiFrameCycMax = 0;
  g_uiFrameCycSum = 0;
  g_serialStats = serialStats;
  g_uiMode = uiMode;
  g_liveOn = true;
  vTaskDelay(pdMS_TO_TICKS(seconds * 1000));
  g_liveOn = false;
  g_uiMode = 0;
  g_serialStats = false;
  const audio::Stats st = audio::stats();
  const uint32_t uf = g_uiFrames - uf0;
  audio::end();
  audio::setBlockObserver(nullptr, nullptr);
  LiveRes& L = g_live;
  RunRes tmp;   // reuse percentile helper
  memcpy(tmp.hist, L.hist, sizeof(L.hist));
  tmp.n = L.n;
  tmp.max = L.rMax;
  rp("LIVE %s %dHz dma%d ui%s ser%s | blocks %lu render us mean %lu p99 %lu max %lu | write(blocked) us mean %lu min %lu | "
     "max gap %luus | underruns %lu late %lu slow %lu | ui frames %lu avg %luus max %luus\n",
     tag, g_rate, dma, uiMode ? "ON" : "off", serialStats ? "ON" : "off", static_cast<unsigned long>(L.n),
     static_cast<unsigned long>(L.n ? L.rSum / L.n / 240 : 0), static_cast<unsigned long>(pctl(tmp, 990) / 240),
     static_cast<unsigned long>(L.rMax / 240), static_cast<unsigned long>(L.n ? L.wSum / L.n / 240 : 0),
     static_cast<unsigned long>(L.wMin == 0xFFFFFFFF ? 0 : L.wMin / 240), static_cast<unsigned long>(L.gapMax / 240),
     static_cast<unsigned long>(st.underruns), static_cast<unsigned long>(st.lateBlocks),
     static_cast<unsigned long>(L.slowN), static_cast<unsigned long>(uf),
     static_cast<unsigned long>(uf ? g_uiFrameCycSum / uf / 240 : 0), static_cast<unsigned long>(g_uiFrameCycMax / 240));
  for (uint32_t k = 0; k < L.slowN && k < 5; ++k)
    rp("SLOW %s t=%lums render %luus write %luus gap %luus ui-drawing %d\n", tag,
       static_cast<unsigned long>(L.slow[k].tMs), static_cast<unsigned long>(L.slow[k].rend),
       static_cast<unsigned long>(L.slow[k].wr), static_cast<unsigned long>(L.slow[k].gap), L.slow[k].ui);
}

// ---------------------------------------------------------------- header ----
const char* region(const void* p) {
  if (!p) return "absent";
  if (esp_ptr_in_iram(p)) return "IRAM";
  if (esp_ptr_in_dram(p)) return "DRAM";
  if (esp_ptr_in_drom(p)) return "flash-rodata";
  const uintptr_t a = reinterpret_cast<uintptr_t>(p);
  if (a >= 0x42000000u && a < 0x44000000u) return "flash-text";
  return "?";
}

void header() {
  const char* mode = "?";
  if (esp_flash_default_chip) {
    switch (esp_flash_default_chip->read_mode) {
      case SPI_FLASH_SLOWRD: mode = "SLOWRD"; break;
      case SPI_FLASH_FASTRD: mode = "FASTRD"; break;
      case SPI_FLASH_DOUT: mode = "DOUT"; break;
      case SPI_FLASH_DIO: mode = "DIO"; break;
      case SPI_FLASH_QOUT: mode = "QOUT"; break;
      case SPI_FLASH_QIO: mode = "QIO"; break;
      default: mode = "other"; break;
    }
  }
  uint8_t hdr[4] = {0, 0, 0, 0};
  esp_flash_read(nullptr, hdr, 0x0, sizeof(hdr));   // bootloader image header: magic, segs, spi_mode, speed/size
  rp("SYS ESP32-S3 cpu %luMHz | icache %dKB %d-way %dB | dcache %dKB %d-way %dB | flash read_mode %s (driver), "
     "sdkconfig %s/%s, bootloader hdr mode %u freq-code %u | IDF %s\n",
     static_cast<unsigned long>(getCpuFrequencyMhz()), CONFIG_ESP32S3_INSTRUCTION_CACHE_SIZE / 1024,
     CONFIG_ESP32S3_ICACHE_ASSOCIATED_WAYS, CONFIG_ESP32S3_INSTRUCTION_CACHE_LINE_SIZE,
     CONFIG_ESP32S3_DATA_CACHE_SIZE / 1024, CONFIG_ESP32S3_DCACHE_ASSOCIATED_WAYS, CONFIG_ESP32S3_DATA_CACHE_LINE_SIZE,
     mode, CONFIG_ESPTOOLPY_FLASHMODE, CONFIG_ESPTOOLPY_FLASHFREQ, hdr[2], hdr[3] & 0x0F, esp_get_idf_version());
  rp("SYS build PG_AUDIO_RAM=%d | IRAM text %lu B | static DRAM %lu B | heap free %lu min-ever %lu largest %lu\n", kTier,
     static_cast<unsigned long>(reinterpret_cast<uintptr_t>(&_iram_text_end) - reinterpret_cast<uintptr_t>(&_iram_text_start)),
     static_cast<unsigned long>(reinterpret_cast<uintptr_t>(&_bss_end) - reinterpret_cast<uintptr_t>(&_data_start)),
     static_cast<unsigned long>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
     static_cast<unsigned long>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
     static_cast<unsigned long>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
  const void* proc = reinterpret_cast<const void*>(&_ZN2pg6Engine7processEPsi);
  const void* lead = reinterpret_cast<const void*>(&_ZN2pg4lead9LeadVoice6renderEPfS2_i);
  const void* bass = reinterpret_cast<const void*>(&_ZN2pg4bass9BassVoice6renderEPfS2_i);
  const void* kick = reinterpret_cast<const void*>(&_ZN2pg5drums9KickVoice6renderEPfS2_i);
  const void* rev = reinterpret_cast<const void*>(&_ZN2pg13engine_detail6Reverb3runILb1EEEvPKfPfS5_i);
  const void* t44 = pg::lead::kTables44k.saw[0].d;
  const void* t32 = pg::lead::kTables32k.saw[0].d;
  const void* sine = pg::bass::gSine;
  if (kTier > 0 && !(esp_ptr_in_iram(proc) && esp_ptr_in_iram(lead) && esp_ptr_in_iram(bass))) {
    rp("SYS *** PLACEMENT FAILED: built with PG_AUDIO_RAM=%d but the hot path is NOT in IRAM ***\n", kTier);
    setStatus("PLACEMENT FAILED (SEE SERIAL)");
  }
  rp("SYM Engine::process %p %s | LeadVoice::render %p %s | BassVoice::render %p %s | KickVoice::render %p %s | "
     "Reverb::run<mono> %p %s\n", proc, region(proc), lead, region(lead), bass, region(bass), kick, region(kick), rev,
     region(rev));
  rp("SYM lead table 44k %p %s | lead table 32k %p %s | bass gSine %p %s | sizeof lead %u bass %u kick %u\n", t44,
     region(t44), t32, region(t32), sine, region(sine), static_cast<unsigned>(sizeof(pg::lead::LeadVoice)),
     static_cast<unsigned>(sizeof(pg::bass::BassVoice)), static_cast<unsigned>(sizeof(pg::drums::KickVoice)));
}

// busiest pattern: most note hits per step (ratchets count), over every demo
void findBusiest(int* bd, int* bp, int* score10) {
  *bd = 0; *bp = 0; *score10 = 0;
  int best = -1;
  for (int d = 0; d < pg::demo::count(); ++d) {
    pg::demo::build(g_inst.engine(), g_inst.project(), d);
    const pg::Project& p = g_inst.project();
    for (int pt = 0; pt < pg::kNumPatterns; ++pt) {
      const pg::Pattern& P = p.patterns[pt];
      int len = P.length ? P.length : 16;
      int hits = 0;
      bool any = false;
      for (int t = 0; t < pg::kNumTracks; ++t) {
        const int tl = P.tracks[t].length ? P.tracks[t].length : 16;
        for (int s = 0; s < len; ++s) {
          const pg::Step& st = P.tracks[t].steps[s % tl];
          if (st.flags() & pg::kStepOn) { hits += pg::stepRatchet(st); any = true; }
        }
      }
      if (!any) continue;
      const int sc = hits * 10 / len;
      if (sc > best && !(d == 0 && pt == 0)) { best = sc; *bd = d; *bp = pt; *score10 = sc; }
    }
  }
}

// ---------------------------------------------------------------- orchestration ----
void profileTask(void*) {
  vTaskDelay(pdMS_TO_TICKS(1500));   // give a late serial monitor a chance; nothing depends on it
  g_pmOk = xtensa_perfmon_init(0, XTPERF_CNT_I_STALL, XTPERF_MASK_I_STALL_ALL, 0, -1) == ESP_OK &&
           xtensa_perfmon_init(1, XTPERF_CNT_D_STALL, XTPERF_MASK_D_STALL_ALL, 0, -1) == ESP_OK;
  rp("\n===== PG-PROFILE BEGIN v1 %s =====\n", kTier ? "IRAM-hot-path" : "flash-hot-path");
  header();
  rp("KEY c/f = CPU cycles per audio frame (5442 = 100%% of one core at 44.1k, 7500 at 32k); RUN p50/p90/p99/max "
     "are per 128-frame block; VOX c/f = render while active, act = %% of quanta active, avg = c/f added to the "
     "whole run, on n mean/max = noteOn calls and cycles, sP = setParams mean/max cycles; TOP bnd @qN = first "
     "note in quantum N of the block, since = quanta since the previous note\n");
  setStatus("MICROBENCHMARKS");
  benchMiss();
  benchFpu();
  benchLibm();
  int bd, bp, bs;
  findBusiest(&bd, &bp, &bs);
  rp("BUSY busiest pattern (excluding ACIDJAM P1): demo %d P%d, %d.%d hits/step\n", bd, bp + 1, bs / 10, bs % 10);

  const int rates[2] = {44100, 32000};
  for (int ri = 0; ri < 2; ++ri) {
    g_rate = rates[ri];
    rp("== RATE %d ==\n", g_rate);
    char st[48];
    const uint8_t all = 0xFF;
    RunCfg A{"A", 0, 0, 16, all, false, false, false, 0, 0};
    snprintf(st, sizeof(st), "%dHZ: ACIDJAM 16 BARS", g_rate);
    setStatus(st);
    runEngine(A); printRun(A); printRunDetail(A);
    const uint64_t ownA = g_res.ownSum, nA = g_res.ownN;

    RunCfg AU{"A+ui", 0, 0, 8, all, false, false, false, 1, 0};
    snprintf(st, sizeof(st), "%dHZ: ACIDJAM + UI DRAWING", g_rate);
    setStatus(st);
    const uint32_t uf0 = g_uiFrames;
    if (kHaveUi) { runEngine(AU); printRun(AU); }
    rp("UI A+ui: %lu full UI frames drawn on core 1 during the run (compare RUN A vs A+ui)\n",
       static_cast<unsigned long>(g_uiFrames - uf0));

    RunCfg W{"W", 0, 0, 4, all, true, false, false, 0, 0};
    snprintf(st, sizeof(st), "%dHZ: WARM VS IN-ENGINE", g_rate);
    setStatus(st);
    runEngine(W);
    for (int t = 0; t < pg::kNumTracks; ++t) {
      const VStat& v = g_probe[t].st;
      rp("WARM %-5s c/f in-engine %d.%d | warm re-run %d.%d | cold(caches flushed) %d.%d vs its warm %d.%d | "
         "calls %lu copyfaults %lu\n",
         kTrackName[t], DF(dec(v.rSum, static_cast<uint64_t>(v.rN) * pg::kQuantum)),
         DF(dec(v.wSum, static_cast<uint64_t>(v.wN) * pg::kQuantum)),
         DF(dec(v.cSum, static_cast<uint64_t>(v.cN) * pg::kQuantum)),
         DF(dec(v.wcSum, static_cast<uint64_t>(v.cN) * pg::kQuantum)), static_cast<unsigned long>(v.rN),
         static_cast<unsigned long>(v.copyFaults));
    }

    RunCfg B{"noDly", 0, 0, 4, all, false, true, false, 0, 0};
    RunCfg C{"noFX", 0, 0, 4, all, false, true, true, 0, 0};
    snprintf(st, sizeof(st), "%dHZ: FX IN ENGINE", g_rate);
    setStatus(st);
    runEngine(B);
    const uint64_t ownB = g_res.ownSum, nB = g_res.ownN;
    runEngine(C);
    const uint64_t ownC = g_res.ownSum, nC = g_res.ownN;
    {
      const Dec a = dec(ownA, nA * pg::kQuantum), b = dec(ownB, nB * pg::kQuantum), c = dec(ownC, nC * pg::kQuantum);
      rp("FXE engine-own c/f: all %d.%d | no delay sends %d.%d | no delay+reverb sends %d.%d "
         "(delay = all - noDly, reverb = noDly - noFX, rest = mixer+ducker+master+sequencer)\n", DF(a), DF(b), DF(c));
    }
    benchFxAlone();
    benchParamBurst();

    // The cache curve: same material, only some tracks allowed to sound (the
    // others' notes are swallowed), FX and master always on.
    benchPair();
    struct Cv { const char* tag; uint8_t mask; int chord; };
    const Cv cv[] = {{"none", 0x00, -1}, {"bass", 0x40, -1}, {"lead", 0x80, -1}, {"lead1", 0x80, 0},
                     {"bass+lead", 0xC0, -1}, {"bass+lead1", 0xC0, 0}, {"drums", 0x3F, -1},
                     {"drums+bass", 0x7F, -1}, {"drums+lead", 0xBF, -1}, {"all", 0xFF, -1}, {"all-lead1", 0xFF, 0}};
    for (const Cv& k : cv) {
      RunCfg K{k.tag, 0, 0, 2, k.mask, false, false, false, 0, 0, k.chord};
      snprintf(st, sizeof(st), "%dHZ: CURVE %s", g_rate, k.tag);
      setStatus(st);
      runEngine(K);
      const uint64_t fr = static_cast<uint64_t>(g_res.ownN) * pg::kQuantum;
      char vs[96];
      int vl = 0;
      for (int t = 0; t < pg::kNumTracks; ++t)
        if (g_probe[t].st.rN && ((k.mask >> t) & 1))
          vl += snprintf(vs + vl, sizeof(vs) - vl, " %s %d.%d", kTrackName[t], DF(dec(g_probe[t].st.rSum, fr)));
      rp("CURVE %-10s mask 0x%02X chord %d | total c/f %d.%d p99 %d.%d max %d.%d | engine-own %d.%d | voices avg c/f:%s\n",
         k.tag, k.mask, k.chord, DF(dec(g_res.sum, static_cast<uint64_t>(g_res.n) * audio::kBlockFrames)),
         DF(dec(pctl(g_res, 990), audio::kBlockFrames)), DF(dec(g_res.max, audio::kBlockFrames)),
         DF(dec(g_res.ownSum, fr)), vs);
      if (k.mask == 0) rp("IDLE sequencer+mixer+FX+master with nothing sounding: step quanta %lu, max %d.%d c/f\n",
                          static_cast<unsigned long>(g_res.ownBndN), DF(dec(g_res.ownBndMax, pg::kQuantum)));
    }

    RunCfg BZ{"busy", bd, bp, 8, all, false, false, false, 0, 0};
    snprintf(st, sizeof(st), "%dHZ: BUSIEST PATTERN", g_rate);
    setStatus(st);
    runEngine(BZ); printRun(BZ); printRunDetail(BZ);

    // Macro motion (engine.h "MACRO MOTION"): ACIDJAM with the bass on ACID and the
    // lead on HANDS, once with every macro standing and once with two macros of
    // each sweeping all the time (a new target every 33 ms, the default glide).
    {
      RunCfg MS{"mot-still", 0, 0, 8, all, false, false, false, 0, 0, -1, 1};
      RunCfg MW{"mot-sweep", 0, 0, 8, all, false, false, false, 0, 0, -1, 2};
      snprintf(st, sizeof(st), "%dHZ: MACRO MOTION", g_rate);
      setStatus(st);
      runEngine(MS); printRun(MS);
      const uint64_t s0 = g_res.sum, n0 = g_res.n;
      const uint32_t max0 = g_res.max;
      runEngine(MW); printRun(MW); printRunDetail(MW);
      const uint64_t s1 = g_res.sum, n1 = g_res.n;
      const int64_t extra = (n1 ? static_cast<int64_t>(s1 / n1) : 0) - (n0 ? static_cast<int64_t>(s0 / n0) : 0);
      const int64_t cpb = static_cast<int64_t>(audio::kBlockFrames) * 240000000ll / g_rate;   // cycles per block = 100 %
      const int64_t pct10 = cpb ? extra * 1000 / cpb : 0;
      const int64_t a10 = pct10 < 0 ? -pct10 : pct10;
      rp("MOTION %dHz sweep - still: %lld cycles per %d-frame block = %s%lld.%lld %% of a core (budget ~3 %%) | "
         "max block c/f still %lu sweep %lu\n",
         g_rate, static_cast<long long>(extra), audio::kBlockFrames, pct10 < 0 ? "-" : "",
         static_cast<long long>(a10 / 10), static_cast<long long>(a10 % 10),
         static_cast<unsigned long>(max0 / audio::kBlockFrames), static_cast<unsigned long>(g_res.max / audio::kBlockFrames));
    }

    // The sampler (stage F1): ACIDJAM with PERC on each sampler instrument at its
    // macros' middle; the sampler's own render cost while it sounds (the probe
    // wraps it like the other voices), and the whole run's worst blocks.
    {
      const char* const smp[2] = {"CHOP", "STUTTER"};
      for (const char* name : smp) {
        RunCfg S{name, 0, 0, 8, all, false, false, false, 0, 0, -1, 0, name};
        snprintf(st, sizeof(st), "%dHZ: SAMPLER %s", g_rate, name);
        setStatus(st);
        runEngine(S); printRun(S);
        const VStat& v = g_smpProbe.st;
        const Dec cf = dec(v.rSum, static_cast<uint64_t>(v.rN) * pg::kQuantum);
        const uint64_t perFrame = 240000000ull / static_cast<uint64_t>(g_rate);   // cycles per frame = 100 % of a core
        const Dec pc = dec(v.rSum * 100ull, static_cast<uint64_t>(v.rN) * pg::kQuantum * perFrame);
        rp("SAMPLER %dHz %s: render c/f while sounding %d.%d = %d.%d %% of a core | quanta sounding %lu of %lu | "
           "noteOn n %lu mean %lu max %lu cyc | setParams mean %lu max %lu cyc | pool %s\n",
           g_rate, name, DF(cf), DF(pc), static_cast<unsigned long>(v.rN),
           static_cast<unsigned long>(g_res.ownN), static_cast<unsigned long>(v.nN),
           static_cast<unsigned long>(v.nN ? v.nSum / v.nN : 0), static_cast<unsigned long>(v.nMax),
           static_cast<unsigned long>(v.pN ? v.pSum / v.pN : 0), static_cast<unsigned long>(v.pMax),
           plat::samples::info().ok ? "app1 mapped" : "built-in sounds only");
      }
    }

    RunCfg T{"tail", 0, 0, 2, all, false, false, false, 0, 12};
    snprintf(st, sizeof(st), "%dHZ: TAILS / DENORMALS", g_rate);
    setStatus(st);
    runEngine(T);
    {
      char line[240];
      int len = snprintf(line, sizeof(line), "TAIL engine after Stop, c/f per 1 s:");
      for (int s = 0; s < 12 && g_res.tailQ[s]; ++s)
        len += snprintf(line + len, sizeof(line) - len, " %d", static_cast<int>(g_res.tail[s] / g_res.tailQ[s]));
      rp("%s\n", line);
    }
    voiceTails();

    snprintf(st, sizeof(st), "%dHZ: LIVE I2S (AUDIBLE)", g_rate);
    setStatus(st);
    runLive("d4-ui", 4, 1, false, 20);
    runLive("d4-frozen", 4, 0, false, 20);
    runLive("d8-ui", 8, 1, false, 20);
    runLive("d4-ui-ser", 4, 1, true, 20);
  }
  rp("===== PG-PROFILE END =====\n");
  g_done = true;
  setStatus("DONE. REPORT REPEATS ON SERIAL");
  vTaskDelete(nullptr);
}

}  // namespace

void setup() {
  sys::begin(115200);
  disp::begin();
  disp::setBrightness(120);
  g_rep = static_cast<char*>(heap_caps_malloc(kRepCap, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  g_inst.init(44100.0f, true);
  if (plat::samples::begin()) g_inst.setSamplePool(&plat::samples::pool());   // the SAMPLER runs read app1 as the instrument does
#if PG_PROFILE_UI
  pg::ui::initPalette(g_fb);
  g_ui.init(g_inst.engine(), g_inst.project(), nullptr);
  g_ui.boot(sys::millis());
#endif
  drawStatusScreen();
  xTaskCreatePinnedToCore(profileTask, "pg_prof", 16384, nullptr, 18, nullptr, 0);
}

void loop() {
  static uint32_t lastRep = 0, lastStat = 0;
  const uint32_t now = sys::millis();
  if (g_uiMode == 1) {
    // UI on: the real UI, full screen pushed every frame (an upper bound on core-1 load)
    const uint32_t t0 = sys::cycles();
    g_uiBusy = true;
    pg::ui::UiEnv env;
    env.nowMs = now;
    env.batteryPct = 100;
    env.sdPresent = false;
    env.cpuPct = 0;
#if PG_PROFILE_UI
    g_ui.update(env, g_fb);
    disp::present(g_fb);
#else
    (void)env;
#endif
    g_uiBusy = false;
    const uint32_t dt = sys::cycles() - t0;
    g_uiFrames = g_uiFrames + 1;
    g_uiFrameCycSum += dt;
    if (dt > g_uiFrameCycMax) g_uiFrameCycMax = dt;
    g_statusDirty = true;   // the profiler banner is redrawn once the UI stops
    const int32_t rem = 33 - static_cast<int32_t>(sys::millis() - now);
    sys::delayMs(rem > 1 ? static_cast<uint32_t>(rem) : 1);
  } else {
    if (g_statusDirty && g_uiMode == 0) {
      g_statusDirty = false;
      drawStatusScreen();
    }
    sys::delayMs(20);
  }
  if (g_serialStats && now - lastStat >= 1000) {
    lastStat = now;
    const audio::Stats as = audio::stats();
    char b[160];
    snprintf(b, sizeof(b), "[stats] t=%lus avg %dus max %dus und %lu late %lu\n",
             static_cast<unsigned long>(now / 1000), static_cast<int>(as.avgUs), static_cast<int>(as.recentMaxUs),
             static_cast<unsigned long>(as.underruns), static_cast<unsigned long>(as.lateBlocks));
    sys::serialWrite(b);
  }
  if (g_done && now - lastRep >= 20000) {
    lastRep = now;
    sys::serialWrite(g_rep, g_repLen);
  }
}
