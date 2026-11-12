// Microphone quality measurement. See mic_quality.h.
#include "mic_quality.h"

#include <Arduino.h>

#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <new>

#include "app.h"
#include "dsp.h"
#include "duplex_audio.h"
#include "spectrum.h"
#include "tone.h"

namespace hws {
namespace quality {

namespace {

constexpr int      kFftN = 2048;            // 21.5 Hz bins at 44.1 kHz
constexpr uint32_t kSilSettleMs = 300;      // after a PGA change
constexpr uint32_t kSilMs = 2000;
constexpr uint32_t kToneSettleMs = 500;
constexpr uint32_t kToneMs = 1000;
constexpr uint32_t kToneHz = 1000;
constexpr float    kToneAmp = 8192.0f;      // -12 dBFS, as the mic matrix
constexpr float    kStimDb = -12.041f;
constexpr uint8_t  kToneVol = 0xAF;         // DAC -8 dB, as the mic matrix
constexpr float    kSpurMinHz = 40.0f;

enum CapState : int { kCapIdle = 0, kCapSettle, kCapRun, kCapDone };
std::atomic<int> g_cap{kCapIdle};
std::atomic<bool> g_running{false}, g_doneReq{false};
std::atomic<int> g_frameState{0};   // 0 audio fills, 1 ready for the job
bool g_toneOn = false, g_wantSpec = false, g_wantHarm = false;   // set by the job before arming
int g_slot = 0;
int32_t g_settleLeft = 0, g_capLeft = 0;
int g_fpos = 0;

// audio task while capturing
LevelAcc g_lvl;
HarmonicDetector g_harm;
ToneGen g_gen;

// one run's buffers
int16_t* g_frame = nullptr;
float* g_re = nullptr;
float* g_im = nullptr;
float* g_pow = nullptr;
ToneTable* g_tab = nullptr;

GainSet g_gains[kNumGainRows];
int g_nGains = 0, g_userGain = 0;
MicQuality g_res;
char g_status[48] = "[n] mic quality (~12 s, speaker on)";

void* alloc(size_t bytes) { return heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT); }

void freeAll() {
  heap_caps_free(g_frame);
  heap_caps_free(g_re);
  heap_caps_free(g_im);
  heap_caps_free(g_pow);
  heap_caps_free(g_tab);
  g_frame = nullptr;
  g_re = g_im = g_pow = nullptr;
  g_tab = nullptr;
}

// Settle, then capture `ms` (rounded to whole tone periods with the tone);
// the job runs the FFTs meanwhile. False on timeout.
bool capture(uint32_t settleMs, uint32_t ms, SpectrumAcc* spec) {
  const uint32_t rate = app::g_sampleRate;
  g_lvl.clear();
  g_harm.begin(g_tab);
  g_fpos = 0;
  g_frameState.store(0);
  g_settleLeft = static_cast<int32_t>(static_cast<uint64_t>(rate) * settleMs / 1000u);
  int32_t n = static_cast<int32_t>(static_cast<uint64_t>(rate) * ms / 1000u);
  if (g_wantHarm && g_tab && g_tab->len > 0) n = (n / g_tab->len) * g_tab->len;
  g_capLeft = n;
  g_cap.store(kCapSettle, std::memory_order_release);
  const uint32_t t0 = millis();
  while (g_cap.load(std::memory_order_acquire) != kCapDone) {
    if (spec && g_frameState.load(std::memory_order_acquire) == 1) {
      spec->addFrame(g_frame, g_re, g_im);
      g_frameState.store(0, std::memory_order_release);
      continue;
    }
    if (millis() - t0 > settleMs + ms + 2000u) {
      g_cap.store(kCapIdle);
      return false;
    }
    delay(2);
  }
  if (spec && g_frameState.load(std::memory_order_acquire) == 1) spec->addFrame(g_frame, g_re, g_im);
  g_cap.store(kCapIdle);
  return true;
}

void job(void*) {
  const uint32_t t0 = millis();
  new (&g_res) MicQuality();
  g_res.slot = g_slot;
  g_res.gain = g_userGain;
  g_res.rate = app::g_sampleRate;
  g_res.silSecs = static_cast<float>(kSilMs) / 1000.0f;
  g_res.fftN = kFftN;
  const uint8_t userPga = duplex::micPga();
  const int userReg16 = duplex::micReg16();
  bool ok = true;

  // 1. room silence per preset
  SpectrumAcc spec;
  for (int g = 0; g < g_nGains && ok; ++g) {
    snprintf(g_status, sizeof(g_status), "quality: silence gain %d (quiet!)", g + 1);
    duplex::setMicGain(g_gains[g].pga, g_gains[g].reg16);
    NoiseRow& row = g_res.noise[g];
    row.pgaDb = g_gains[g].pgaDb;
    uint8_t v = 0;
    if (duplex::codecRead(0x14, &v)) row.reg14 = v;
    if (duplex::codecRead(0x16, &v)) row.reg16 = v;
    spec.begin(g_pow, kFftN);
    g_toneOn = false;
    g_wantSpec = true;
    g_wantHarm = false;
    ok = capture(kSilSettleMs, kSilMs, &spec);
    if (!ok) break;
    const LevelResult lr = levelResult(g_lvl);
    row.valid = true;
    row.rmsDb = lr.acRmsDb;
    row.peakDb = lr.peakDb;
    row.dcLsb = lr.dcLsb;
    row.stuck = lr.stuck;
    row.clips = lr.clips;
    row.frames = spec.frames();
    const SpurResult s = spec.worstSpur(static_cast<float>(app::g_sampleRate), kSpurMinHz, g_re);
    row.spurValid = s.valid;
    row.spurHz = s.hz;
    row.spurDb = s.db;
    row.floorDb = s.floorDb;
    row.spurOverFloorDb = s.overFloorDb;
  }
  duplex::setMicGain(userPga, userReg16);

  // 2. the speaker tone at the user's preset
  if (ok) {
    snprintf(g_status, sizeof(g_status), "quality: 1 kHz tone, gain %d", g_userGain + 1);
    const uint8_t vol = duplex::volumeReg();
    duplex::setVolumeReg(kToneVol);
    g_gen.start(g_tab, kToneAmp);
    g_wantSpec = false;
    g_wantHarm = true;
    g_toneOn = true;
    const bool tok = capture(kToneSettleMs, kToneMs, nullptr);
    g_toneOn = false;
    delay(20);   // the audio task has sent silence again
    duplex::setVolumeReg(vol);
    if (tok) {
      g_res.toneValid = true;
      g_res.tone = g_harm.result();
      g_res.toneN = g_res.tone.n;
    }
    g_res.vol = kToneVol;
    g_res.stimDb = kStimDb;
    g_res.toneHz = kToneHz;
  }
  g_res.valid = ok;
  g_res.secs = static_cast<float>(millis() - t0) / 1000.0f;
  if (ok)
    snprintf(g_status, sizeof(g_status), "n: g%d %.1f thd %.1f snr %.1f", g_userGain + 1,
             static_cast<double>(g_res.noise[g_userGain < g_nGains ? g_userGain : 0].rmsDb),
             static_cast<double>(g_res.tone.thdDb), static_cast<double>(g_res.tone.snrDb));
  else
    snprintf(g_status, sizeof(g_status), "quality: capture timed out");
  freeAll();
  g_doneReq.store(true);
  g_running.store(false);
  vTaskDelete(nullptr);
}

}  // namespace

bool start(const GainSet* gains, int n, int userGain, int slot) {
  if (g_running.load()) return false;
  if (!duplex::running()) {
    app::msg("audio not running");
    return false;
  }
  g_nGains = n < 0 ? 0 : (n > kNumGainRows ? kNumGainRows : n);
  for (int i = 0; i < g_nGains; ++i) g_gains[i] = gains[i];
  g_userGain = userGain;
  g_slot = slot ? 1 : 0;
  g_frame = static_cast<int16_t*>(alloc(sizeof(int16_t) * kFftN));
  g_re = static_cast<float*>(alloc(sizeof(float) * kFftN));
  g_im = static_cast<float*>(alloc(sizeof(float) * kFftN));
  g_pow = static_cast<float*>(alloc(sizeof(float) * (kFftN / 2 + 1)));
  void* t = alloc(sizeof(ToneTable));
  g_tab = t ? new (t) ToneTable() : nullptr;
  if (!g_frame || !g_re || !g_im || !g_pow || !g_tab || !g_tab->init(kToneHz, app::g_sampleRate)) {
    freeAll();
    app::msg("quality: no RAM (%u B)", static_cast<unsigned>(kFftN * 10 + sizeof(ToneTable)));
    return false;
  }
  g_running.store(true);
  snprintf(g_status, sizeof(g_status), "quality starting...");
  if (xTaskCreatePinnedToCore(job, "hws_qual", 6144, nullptr, 5, nullptr, 1) != pdPASS) {
    freeAll();
    g_running.store(false);
    snprintf(g_status, sizeof(g_status), "quality: task create failed");
    return false;
  }
  return true;
}

bool running() { return g_running.load(); }
const char* statusLine() { return g_status; }

void process(const int16_t* in, int16_t* out, int frames) {
  for (int i = 0; i < frames; ++i) {
    const int16_t s = g_toneOn ? g_gen.next() : 0;
    out[2 * i] = s;
    out[2 * i + 1] = s;
  }
  const int st = g_cap.load(std::memory_order_acquire);
  if (st == kCapSettle) {
    g_settleLeft -= frames;
    if (g_settleLeft <= 0) g_cap.store(kCapRun, std::memory_order_release);
    return;
  }
  if (st != kCapRun) return;
  int n = frames;
  if (n > g_capLeft) n = g_capLeft;
  const int ch = g_slot;
  for (int i = 0; i < n; ++i) {
    const int16_t x = in[2 * i + ch];
    g_lvl.add(x);
    if (g_wantHarm) g_harm.add(x);
    if (g_wantSpec && g_frameState.load(std::memory_order_relaxed) == 0) {
      g_frame[g_fpos++] = x;
      if (g_fpos >= kFftN) {
        g_fpos = 0;
        g_frameState.store(1, std::memory_order_release);
      }
    }
  }
  g_capLeft -= n;
  if (g_capLeft <= 0) g_cap.store(kCapDone, std::memory_order_release);
}

bool tick() { return g_doneReq.exchange(false); }
const MicQuality& result() { return g_res; }

}  // namespace quality
}  // namespace hws
