// The microphone matrix. See mic_matrix.h and docs/hw-spike.md 4b.
//
// Where each candidate comes from (all read in this session):
//  * spike: duplex::defaultCodecInit() = the instrument's DAC sequence + the
//    Rust BSP's ADC set (tszarzynski/cardputer-adv @2de5578 src/codec.rs).
//  * m5unified: M5Unified 0.2.29 _speaker_enabled_cb_cardputer_adv followed by
//    _microphone_enabled_cb_cardputer_adv (M5Unified.inl L1045, L1537), i.e.
//    what M5.Speaker then M5.Mic write (0x01 ends at 0xBA: DAC clocks off).
//    The DAC volume write (0x32 = 0xBF) is left to duplex's ramp.
//  * esp_codec_dev: espressif/esp-adf master components/esp_codec_dev/device/
//    es8311/es8311.c, es8311_open() + es8311_set_fs() + es8311_start() for
//    slave mode, MCLK from SCLK (use_mclk = false), 16-bit, 44.1 kHz,
//    ESP_CODEC_DEV_WORK_MODE_BOTH, analog mic; no_dac_ref true (0x44 = 0x08)
//    or false (0x44 = 0x58: "internal reference signal (ADCL + DACR)").
//    Its clock table entry for MCLK = 256 fs at 44.1 kHz is pre_div 1,
//    adc_div 1, dac_div 1, fs_mode 0, lrck 0x00FF, bclk_div 4, OSR 0x10 / 0x10,
//    and with use_mclk = false it forces pre_multi = 3 (x8).
//  * esp-bsp: espressif/esp-bsp master components/es8311/es8311.c,
//    es8311_init() with mclk_from_mclk_pin = false (internal MCLK = fs x 16 x 2
//    = 1.4112 MHz -> table row pre_div 1, pre_multi 3, rest as above) and
//    es8311_microphone_config(false) (0x17 = 0xC8, 0x14 = 0x1A).
//  * M5.Mic control: M5Unified's own Mic_Class with the board config M5.begin
//    would have set (M5Unified.inl _begin_audio: i2s_port 0, over_sampling 1,
//    data 46, ws 43, bck 41, 16 kHz, right slot) and the exact
//    _microphone_enabled_cb_cardputer_adv writes, used as its examples do
//    (begin, record, wait, end); our duplex driver is shut down meanwhile.
//
// Second run (after the 2026-11-12 result, hardware-notes 11.7):
//  * Baseline: the registers at the start of the run (= the boot default)
//    are written back before every config (kRegSet, diff only), so each one
//    starts from the same state; cfgNN_diff reports what it left different.
//  * Ranking only on microphone slots (tone.h micSlots(): never a slot routed
//    to the DAC reference by reg 0x44, never one that is the stimulus itself).
//  * Configs 24-34: the cold-reset candidates (why the reset killed the ADC).
//    Reset bits of reg 0x00 do NOT restore the register file (the device's
//    regs_after kept esp_codec_dev's 0x10 / 0x11 / 0x15 / 0x1B through
//    esp-bsp's 0x1F reset), so "cold" here = reset asserted + the power-on
//    values written explicitly (kPor).
#include "mic_matrix.h"

#include <Arduino.h>
#include <M5Unified.h>

#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>

#include "app.h"
#include "duplex_audio.h"
#include "report.h"
#include "tone.h"

namespace hws {
namespace micmatrix {

namespace {

constexpr uint8_t kPga = 6;                 // 18 dB analog PGA for the spike-based setups
constexpr uint8_t kReg14 = 0x10 | kPga;
constexpr uint8_t kMatrixVol = 0xAF;        // DAC -8 dB during the run (tone -12 dBFS -> -20 dB)
constexpr float   kToneAmp = 8192.0f;       // -12 dBFS
constexpr float   kStimDb = -12.041f;       // 20 log10(8192 / 32768)
constexpr uint32_t kToneHz = 1000;
constexpr uint32_t kCaptureMs = 500;
constexpr uint8_t kDelay = 0xF0;            // pseudo register in a table: delay val * 4 ms

enum Seq : uint8_t { kSeqSpike, kSeqM5, kSeqAdf, kSeqAdfRef, kSeqBsp, kSeqResetSpike, kSeqSlowSpike };

// What happens to reg 0x00 (and the register file) before the sequence.
enum Pre : uint8_t {
  kPreNone = 0,
  kPreReset,       // 0x00 = 0x1F (CSM off, every block in reset), 20 ms; registers as they are (0x15 = 0x40 from the baseline)
  kPreReset15,     // 0x15 = 0x00 (ADC soft ramp off) first, then as kPreReset
  kPreCold,        // 0x00 = 0x1F, 20 ms, then kPor written: as close to power-on as I2C gets
  kPreRstCsmOn,    // 0x00 = 0x9F (blocks reset, CSM stays on), 20 ms, 0x00 = 0x80
  kPreCsmCycle,    // 0x00 = 0x00 (CSM off, no block reset), 20 ms
};

struct RegW {
  uint8_t reg, val;
};

struct MCfg {
  const char* name;
  uint8_t slotBits;
  uint8_t din;           // duplex::DinMode; kDinAsFound = GPIO46 restored to the state M5.begin left
  Seq seq;
  const RegW* patch;     // written after the sequence
  uint8_t npatch;
  bool before;           // codec before the I2S clocks
  uint16_t settleMs;
  bool loopR;            // R slot = DAC loopback (not the mic)
  bool m5mic;
  Pre pre;
};

// ES8311 power-on values of every register a config writes (not 0x00, 0x32).
// Measured on this device (regs_before of 2026-11-12, registers the spike had
// not written): 0x03-0x08, 0x0B, 0x0C, 0x0F, 0x10, 0x11, 0x15, 0x16,
// 0x18-0x1B. The rest are the datasheet defaults as remembered, NOT read from
// the datasheet in this session (UNVERIFIED): 0x01 00, 0x02 00, 0x09 00,
// 0x0A 00, 0x0D FC, 0x0E 6A, 0x12 02, 0x13 40, 0x14 10, 0x17 00, 0x1C 4C,
// 0x31 00, 0x37 48, 0x44 00, 0x45 00. Analog power-down (0x0D) last.
const RegW kPor[] = {{0x01, 0x00}, {0x02, 0x00}, {0x03, 0x10}, {0x04, 0x10}, {0x05, 0x00}, {0x06, 0x03}, {0x07, 0x00},
                     {0x08, 0xFF}, {0x09, 0x00}, {0x0A, 0x00}, {0x0B, 0x00}, {0x0C, 0x20}, {0x0E, 0x6A}, {0x0F, 0x00},
                     {0x10, 0x13}, {0x11, 0x7C}, {0x12, 0x02}, {0x13, 0x40}, {0x14, 0x10}, {0x15, 0x00}, {0x16, 0x04},
                     {0x17, 0x00}, {0x18, 0x00}, {0x19, 0x00}, {0x1A, 0x00}, {0x1B, 0x0C}, {0x1C, 0x4C}, {0x31, 0x00},
                     {0x37, 0x48}, {0x44, 0x00}, {0x45, 0x00}, {0x0D, 0xFC}};

// The registers the baseline restore and the per-config diff cover.
bool inRegSet(int r) { return r <= 0x1C || (r >= 0x31 && r <= 0x37) || r == 0x44 || r == 0x45; }

const RegW kP02x4[] = {{0x02, 0x10}};
const RegW kPLoop[] = {{0x44, 0x58}};
const RegW kPDmic[] = {{0x14, static_cast<uint8_t>(0x40 | kReg14)}};
const RegW kPMic2[] = {{0x14, static_cast<uint8_t>(0x20 | kPga)}};
const RegW kPMute[] = {{0x0A, 0x4C}};
const RegW kP24[] = {{0x0A, 0x00}};
const RegW kPBias[] = {{0x0B, 0x00}, {0x0C, 0x00}, {0x10, 0x1F}, {0x11, 0x7F}, {0x13, 0x10},
                       {0x1B, 0x0A}, {0x1C, 0x6A}, {0x15, 0x40}, {0x44, 0x08}, {0x45, 0x00}};
const RegW kPDig[] = {{0x16, 0x24}, {0x17, 0xC8}};
const RegW kPPga30[] = {{0x14, 0x1A}};

#define P(x) x, static_cast<uint8_t>(sizeof(x) / sizeof(x[0]))
using duplex::kDinAsFound;
using duplex::kDinForcedHigh;
using duplex::kDinOffPulldown;
using duplex::kDinOutputOff;

// 0-23: the first run's configs, same numbers (now each from the baseline).
// 24-34: why a reset kills the ADC, and a cold init (hardware-notes 11.7).
const MCfg kCfgs[] = {
    // name                         bits din              seq             patch       before settle loopR m5mic pre
    {"spike-as-is",                 16, kDinAsFound,     kSeqSpike,      nullptr, 0, false, 250, false, false, kPreNone},
    {"spike+din-fix",               16, kDinOutputOff,   kSeqSpike,      nullptr, 0, false, 250, false, false, kPreNone},
    {"spike+din-forced-high",       16, kDinForcedHigh,  kSeqSpike,      nullptr, 0, false, 250, false, false, kPreNone},
    {"spike+din-off+pulldown",      16, kDinOffPulldown, kSeqSpike,      nullptr, 0, false, 250, false, false, kPreNone},
    {"m5unified-seq",               16, kDinOutputOff,   kSeqM5,         nullptr, 0, false, 250, false, false, kPreNone},
    {"m5unified-seq-as-is",         16, kDinAsFound,     kSeqM5,         nullptr, 0, false, 250, false, false, kPreNone},
    {"esp_codec_dev",               16, kDinOutputOff,   kSeqAdf,        nullptr, 0, false, 250, false, false, kPreNone},
    {"esp_codec_dev+dacref",        16, kDinOutputOff,   kSeqAdfRef,     nullptr, 0, false, 250, true,  false, kPreNone},
    {"esp-bsp-es8311",              16, kDinOutputOff,   kSeqBsp,        nullptr, 0, false, 250, false, false, kPreNone},
    {"spike+loopback-44=58",        16, kDinOutputOff,   kSeqSpike,      P(kPLoop),  false, 250, true,  false, kPreNone},
    {"slot32-x8-512fs",             32, kDinOutputOff,   kSeqSpike,      nullptr, 0, false, 250, false, false, kPreNone},
    {"slot32-x4-256fs",             32, kDinOutputOff,   kSeqSpike,      P(kP02x4),  false, 250, false, false, kPreNone},
    {"slot16-x4-128fs",             16, kDinOutputOff,   kSeqSpike,      P(kP02x4),  false, 250, false, false, kPreNone},
    {"dmic-on",                     16, kDinOutputOff,   kSeqSpike,      P(kPDmic),  false, 250, false, false, kPreNone},
    {"input-mic2",                  16, kDinOutputOff,   kSeqSpike,      P(kPMic2),  false, 250, false, false, kPreNone},
    {"sdp-out-muted",               16, kDinOutputOff,   kSeqSpike,      P(kPMute),  false, 250, false, false, kPreNone},
    {"sdp-out-24bit",               16, kDinOutputOff,   kSeqSpike,      P(kP24),    false, 250, false, false, kPreNone},
    {"esp-bias-hpf-regs",           16, kDinOutputOff,   kSeqSpike,      P(kPBias),  false, 250, false, false, kPreNone},
    {"digital-gain-16=24-17=C8",    16, kDinOutputOff,   kSeqSpike,      P(kPDig),   false, 250, false, false, kPreNone},
    {"full-reset-first",            16, kDinOutputOff,   kSeqResetSpike, nullptr, 0, false, 300, false, false, kPreNone},
    {"codec-before-clock",          16, kDinOutputOff,   kSeqSpike,      nullptr, 0, true,  300, false, false, kPreNone},
    {"slow-power-up",               16, kDinOutputOff,   kSeqSlowSpike,  nullptr, 0, false, 600, false, false, kPreNone},
    {"M5.Mic-as-examples",          16, kDinAsFound,     kSeqSpike,      nullptr, 0, false, 200, false, true,  kPreNone},
    {"M5.Mic+din-fix",              16, kDinOutputOff,   kSeqSpike,      nullptr, 0, false, 200, false, true,  kPreNone},
    // --- cold-reset candidates (24 = the boot default as a control: M5.Mic's end left 0x00 = 0x00) ---
    {"esp (control)",               16, kDinOutputOff,   kSeqAdf,        nullptr, 0, false, 300, false, false, kPreNone},
    {"reset(15=40)+esp 0.3s",      16, kDinOutputOff,   kSeqAdf,        nullptr, 0, false, 300, false, false, kPreReset},
    {"reset(15=40)+esp 1.2s",       16, kDinOutputOff,   kSeqAdf,        nullptr, 0, false, 1200, false, false, kPreReset},
    {"reset(15=40)+esp 3s",         16, kDinOutputOff,   kSeqAdf,        nullptr, 0, false, 3000, false, false, kPreReset},
    {"15=00,reset+esp 0.3s",        16, kDinOutputOff,   kSeqAdf,        nullptr, 0, false, 300, false, false, kPreReset15},
    {"cold+esp 0.3s",               16, kDinOutputOff,   kSeqAdf,        nullptr, 0, false, 300, false, false, kPreCold},
    {"cold+esp codec-first",        16, kDinOutputOff,   kSeqAdf,        nullptr, 0, true,  300, false, false, kPreCold},
    {"rst-csm-on(9F)+esp",          16, kDinOutputOff,   kSeqAdf,        nullptr, 0, false, 300, false, false, kPreRstCsmOn},
    {"csm-off/on(00)+esp",          16, kDinOutputOff,   kSeqAdf,        nullptr, 0, false, 300, false, false, kPreCsmCycle},
    {"cold+esp-bsp",                16, kDinOutputOff,   kSeqBsp,        nullptr, 0, false, 300, false, false, kPreCold},
    {"cold+spike",                  16, kDinOutputOff,   kSeqSpike,      nullptr, 0, false, 300, false, false, kPreCold},
    {"m5unified-seq+pga30",         16, kDinOutputOff,   kSeqM5,         P(kPPga30), false, 250, false, false, kPreNone},
};
#undef P
constexpr int kNumCfgs = static_cast<int>(sizeof(kCfgs) / sizeof(kCfgs[0]));
static_assert(kNumCfgs <= kMaxMicCfgs, "too many matrix configs");

// ---- state ------------------------------------------------------------------
enum CapState : int { kCapIdle = 0, kCapSettle, kCapRun, kCapDone };
std::atomic<int> g_cap{kCapIdle};
std::atomic<bool> g_running{false}, g_finished{false}, g_works{false};
std::atomic<bool> g_printReq{false}, g_appliedReq{false};
std::atomic<int> g_progress{0};
int g_appliedSlot = 0;
int32_t g_settleLeft = 0, g_capLeft = 0;   // audio task (frames)
bool g_toneOn = false;
ToneTable g_tab, g_tab16;
ToneGen g_gen;
ToneDetector g_det[2];
MicMatrixReport g_rep;
char g_status[48] = "[m] mic matrix (~50 s, speaker on)";
uint8_t g_userVol = 0x80;

bool W(uint8_t reg, uint8_t val) { return duplex::codecWrite(reg, val); }
uint8_t R(uint8_t reg) {
  uint8_t v = 0;
  duplex::codecRead(reg, &v);
  return v;
}
bool runTable(const RegW* t, int n) {
  bool ok = true;
  for (int i = 0; i < n; ++i) {
    if (t[i].reg == kDelay) delay(4u * t[i].val);
    else ok = W(t[i].reg, t[i].val) && ok;
  }
  return ok;
}

// ---- codec sequences -----------------------------------------------------------
bool seqSpike() {
  duplex::setReg14Hi(0x10);
  return duplex::spikeCodecInit();
}

// ---- baseline --------------------------------------------------------------------
uint8_t g_base[kNumCodecRegs];
bool g_baseValid = false;

// Writes back every register of kRegSet that differs from the baseline
// (0x00 first, 0x32 = volume left to duplex's ramp).
void restoreBaseline() {
  if (!g_baseValid) return;
  uint8_t v = 0;
  if (duplex::codecRead(0x00, &v) && v != g_base[0x00]) W(0x00, g_base[0x00]);
  for (int r = 1; r < kNumCodecRegs; ++r) {
    if (!inRegSet(r) || r == 0x32) continue;
    if (duplex::codecRead(static_cast<uint8_t>(r), &v) && v != g_base[r]) W(static_cast<uint8_t>(r), g_base[r]);
  }
}

void diffRegs(MicCfgRow& row) {
  row.nDiff = 0;
  row.diffValid = g_baseValid;
  if (!g_baseValid) return;
  for (int r = 0; r < kNumCodecRegs; ++r) {
    if (!inRegSet(r) || r == 0x32) continue;
    uint8_t v = 0;
    if (!duplex::codecRead(static_cast<uint8_t>(r), &v)) {
      row.diffValid = false;
      continue;
    }
    if (v == g_base[r]) continue;
    if (row.nDiff < kMaxRegDiffs) {
      row.diffReg[row.nDiff] = static_cast<uint8_t>(r);
      row.diffVal[row.nDiff] = v;
    }
    ++row.nDiff;
  }
}

bool runPre(Pre p) {
  switch (p) {
    case kPreNone: return true;
    case kPreReset: {
      const bool ok = W(0x00, 0x1F);
      delay(20);
      return ok;
    }
    case kPreReset15: {
      bool ok = W(0x15, 0x00);
      ok = W(0x00, 0x1F) && ok;
      delay(20);
      return ok;
    }
    case kPreCold: {
      const bool ok = W(0x00, 0x1F);
      delay(20);
      return runTable(kPor, static_cast<int>(sizeof(kPor) / sizeof(kPor[0]))) && ok;
    }
    case kPreRstCsmOn: {
      bool ok = W(0x00, 0x9F);
      delay(20);
      return W(0x00, 0x80) && ok;
    }
    case kPreCsmCycle: {
      const bool ok = W(0x00, 0x00);
      delay(20);
      return ok;
    }
  }
  return true;
}

bool seqM5() {
  static const RegW spk[] = {{0x00, 0x80}, {0x01, 0xB5}, {0x02, 0x18}, {0x0D, 0x01},
                             {0x12, 0x00}, {0x13, 0x10}, {0x37, 0x08}};
  static const RegW mic[] = {{0x00, 0x80}, {0x01, 0xBA}, {0x02, 0x18}, {0x0D, 0x01}, {0x0E, 0x02},
                             {0x14, 0x10}, {0x17, 0xBF}, {0x1C, 0x6A}};
  bool ok = runTable(spk, sizeof(spk) / sizeof(spk[0]));
  return runTable(mic, sizeof(mic) / sizeof(mic[0])) && ok;
}

bool seqBsp() {
  bool ok = W(0x00, 0x1F);
  delay(20);
  ok = W(0x00, 0x00) && ok;
  ok = W(0x00, 0x80) && ok;
  ok = W(0x01, 0xBF) && ok;                                               // 0x3F | BIT(7)
  ok = W(0x06, static_cast<uint8_t>(R(0x06) & ~0x20)) && ok;
  ok = W(0x02, static_cast<uint8_t>((R(0x02) & 0x07) | (0 << 5) | (3 << 3))) && ok;
  ok = W(0x03, 0x10) && ok;
  ok = W(0x04, 0x10) && ok;
  ok = W(0x05, 0x00) && ok;
  ok = W(0x06, static_cast<uint8_t>((R(0x06) & 0xE0) | (4 - 1))) && ok;
  ok = W(0x07, static_cast<uint8_t>(R(0x07) & 0xC0)) && ok;
  ok = W(0x08, 0xFF) && ok;
  ok = W(0x00, static_cast<uint8_t>(R(0x00) & 0xBF)) && ok;
  static const RegW rest[] = {{0x09, 0x0C}, {0x0A, 0x0C}, {0x0D, 0x01}, {0x0E, 0x02}, {0x12, 0x00},
                              {0x13, 0x10}, {0x1C, 0x6A}, {0x37, 0x08}, {0x17, 0xC8}, {0x14, 0x1A}};
  return runTable(rest, sizeof(rest) / sizeof(rest[0])) && ok;
}

bool codecFor(void* user) {
  const MCfg& c = *static_cast<const MCfg*>(user);
  restoreBaseline();
  bool ok = runPre(c.pre);
  switch (c.seq) {
    case kSeqSpike: ok = seqSpike() && ok; break;
    case kSeqM5: ok = seqM5() && ok; break;
    case kSeqAdf: ok = duplex::espCodecDevInit(false) && ok; break;
    case kSeqAdfRef: ok = duplex::espCodecDevInit(true) && ok; break;
    case kSeqBsp: ok = seqBsp() && ok; break;
    case kSeqResetSpike: {
      static const RegW rst[] = {{0x00, 0x1F}, {kDelay, 5}, {0x00, 0x00}};
      ok = runTable(rst, 3);
      ok = seqSpike() && ok;
      break;
    }
    case kSeqSlowSpike: {
      static const RegW slow[] = {{0x00, 0x1F}, {kDelay, 5},  {0x00, 0x00}, {0x00, 0x80}, {0x01, 0xBF},
                                  {0x02, 0x18}, {0x0D, 0x01}, {kDelay, 25}, {0x0E, 0x02}, {kDelay, 25}};
      ok = runTable(slow, sizeof(slow) / sizeof(slow[0]));
      ok = seqSpike() && ok;
      break;
    }
  }
  if (c.patch && c.npatch) ok = runTable(c.patch, c.npatch) && ok;
  return ok;
}

// GPIO46 back to the state M5.begin() left it in (kDinAsFound means "as at boot").
void restoreBootPin() {
  const PinSnap b = duplex::pinBoot();
  if (b.valid && b.oe) duplex::applyDinMode(kDinForcedHigh);
  else duplex::applyDinMode(kDinOutputOff);
  duplex::setDinPulls(b.valid && b.wpu, b.valid && b.wpd);
}

duplex::Config cfgFor(const MCfg& c, uint8_t vol) {
  duplex::Config dc;
  dc.sampleRate = app::g_sampleRate;
  dc.volumeReg = vol;
  dc.slotBits = c.slotBits;
  dc.dinMode = (c.din == kDinAsFound) ? static_cast<uint8_t>(kDinAsFound) : c.din;
  dc.codec = codecFor;
  dc.codecUser = const_cast<MCfg*>(&c);
  dc.codecBeforeClock = c.before;
  return dc;
}

// Runs settle + capture in the audio task; false on timeout.
bool capture(uint32_t settleMs, bool tone) {
  const uint32_t rate = app::g_sampleRate;
  g_toneOn = tone;
  g_gen.start(&g_tab, kToneAmp);
  g_det[0].begin(&g_tab);
  g_det[1].begin(&g_tab);
  g_settleLeft = static_cast<int32_t>(static_cast<uint64_t>(rate) * settleMs / 1000u);
  // a whole number of tone-table periods: no leakage in the tone bin
  int32_t n = static_cast<int32_t>(static_cast<uint64_t>(rate) * kCaptureMs / 1000u);
  if (g_tab.len > 0) n = (n / g_tab.len) * g_tab.len;
  g_capLeft = n;
  g_cap.store(kCapSettle, std::memory_order_release);
  const uint32_t t0 = millis();
  while (g_cap.load(std::memory_order_acquire) != kCapDone) {
    if (millis() - t0 > settleMs + kCaptureMs + 2000u) {
      g_cap.store(kCapIdle);
      return false;
    }
    delay(5);
  }
  g_cap.store(kCapIdle);
  return true;
}

// Verdict per slot, then the score of the better MICROPHONE slot only: a slot
// that reg 0x44 routes to the DAC, or that is the stimulus itself, never counts.
void judge(MicCfgRow& row) {
  for (int s = 0; s < 2; ++s) row.v[s] = judgeMic(row.res[s]);
  micSlots(row.regsValid, row.r44, row.loopbackR, row.res, row.tone, kStimDb, row.micOk);
  if (row.m5mic) row.micOk[1] = false;   // one mono stream, printed once
  scoreMicRow(row);
}

void readRegs(MicCfgRow& row) {
  uint8_t v[5];
  static const uint8_t regs[5] = {0x01, 0x02, 0x0A, 0x14, 0x44};
  row.regsValid = true;
  for (int i = 0; i < 5; ++i)
    if (!duplex::codecRead(regs[i], &v[i])) row.regsValid = false;
  row.r01 = v[0];
  row.r02 = v[1];
  row.r0A = v[2];
  row.r14 = v[3];
  row.r44 = v[4];
}

void dumpRegs(uint8_t* out, bool* valid) {
  *valid = true;
  for (int r = 0; r < kNumCodecRegs; ++r)
    if (!duplex::codecRead(static_cast<uint8_t>(r), &out[r])) *valid = false;
}

void runDuplexCfg(int i) {
  const MCfg& c = kCfgs[i];
  MicCfgRow& row = g_rep.cfg[i];
  row.name = c.name;
  row.slotBits = c.slotBits;
  row.din = c.din;
  row.tone = true;
  row.loopbackR = c.loopR;
  row.m5mic = false;
  row.rate = app::g_sampleRate;
  if (c.din == kDinAsFound) restoreBootPin();
  duplex::setMicGain(kPga, -1);   // the spike sequence picks this up (0x14 = 0x16, 0x16 = power-on)
  if (!duplex::restart(cfgFor(c, kMatrixVol))) {
    app::sp("[matrix] cfg %d %s: restart failed: %s\n", i, c.name, duplex::lastError());
    row.ran = false;
    return;
  }
  if (!capture(c.settleMs, true)) {
    app::sp("[matrix] cfg %d %s: capture timed out\n", i, c.name);
    row.ran = false;
    return;
  }
  row.res[0] = g_det[0].result();
  row.res[1] = g_det[1].result();
  readRegs(row);
  diffRegs(row);
  row.ran = true;
  judge(row);
}

// ---- pin test (codec running the spike setup, GPIO46 as at boot) -----------------
void pinTest() {
  constexpr uint32_t kReads = 20000;
  float* o = g_rep.pinOnes;
  o[0] = duplex::sampleDinOnes(kReads);
  duplex::applyDinMode(kDinOutputOff);
  duplex::setDinPulls(true, false);
  delay(2);
  o[1] = duplex::sampleDinOnes(kReads);
  duplex::setDinPulls(false, true);
  delay(2);
  o[2] = duplex::sampleDinOnes(kReads);
  const uint8_t r0a = R(0x0A);
  W(0x0A, static_cast<uint8_t>(r0a | 0x40));   // SDP out mute: a driving codec sends 0
  delay(5);
  duplex::setDinPulls(true, false);
  delay(2);
  o[3] = duplex::sampleDinOnes(kReads);
  duplex::setDinPulls(false, true);
  delay(2);
  o[4] = duplex::sampleDinOnes(kReads);
  W(0x0A, r0a);
  duplex::setDinPulls(false, false);
  g_rep.pinTestValid = true;
  const bool espOe = (g_rep.pinBoot.valid && g_rep.pinBoot.oe) || (g_rep.pinAfterI2s.valid && g_rep.pinAfterI2s.oe);
  const bool followsPull = o[1] > 0.9f && o[2] < 0.1f;
  const bool mutedLow = o[3] < 0.05f;           // pulled up, still reads 0: something drives it low
  const bool codecDrives = mutedLow || !followsPull;
  const bool floats = followsPull && o[3] > 0.9f && o[4] < 0.1f;
  if (codecDrives) g_rep.pinVerdict = espOe ? "esp_drove_din_itself+codec_drives_line" : "codec_drives_line";
  else if (floats) g_rep.pinVerdict = espOe ? "esp_drove_din_itself+line_floats" : "line_floats_codec_silent";
  else g_rep.pinVerdict = espOe ? "esp_drove_din_itself+unclear" : "unclear";
  restoreBootPin();
}

// ---- M5.Mic control -------------------------------------------------------------
bool m5MicCb(void*, bool enabled) {
  // M5Unified 0.2.29 _microphone_enabled_cb_cardputer_adv, byte for byte
  static const RegW on[] = {{0x00, 0x80}, {0x01, 0xBA}, {0x02, 0x18}, {0x0D, 0x01}, {0x0E, 0x02},
                            {0x14, 0x10}, {0x17, 0xBF}, {0x1C, 0x6A}};
  static const RegW off[] = {{0x0D, 0xFC}, {0x0E, 0x6A}, {0x00, 0x00}};
  if (enabled) runTable(on, sizeof(on) / sizeof(on[0]));
  else runTable(off, sizeof(off) / sizeof(off[0]));
  return true;
}

// Mic_Class::setCallback is protected (M5.begin sets it per board; with
// cfg.internal_mic = false it never did). A derived class may name it.
struct MicAccess : m5::Mic_Class {
  static void setCb(m5::Mic_Class& mic, bool (*cb)(void*, bool)) { (mic.*(&MicAccess::setCallback))(nullptr, cb); }
};

constexpr uint32_t kM5Rate = 16000;   // Mic_Class default
constexpr size_t kM5Chunk = 1600;     // 0.1 s
int16_t g_m5buf[kM5Chunk];

bool m5Record(ToneDetector* det) {
  if (!M5.Mic.record(g_m5buf, kM5Chunk, kM5Rate)) return false;
  const uint32_t t0 = millis();
  while (M5.Mic.isRecording()) {
    if (millis() - t0 > 1500) return false;
    delay(2);
  }
  if (det)
    for (size_t i = 0; i < kM5Chunk; ++i) det->add(g_m5buf[i]);
  return true;
}

void runM5MicCfg(int i) {
  const MCfg& c = kCfgs[i];
  MicCfgRow& row = g_rep.cfg[i];
  row.name = c.name;
  row.slotBits = 16;
  row.din = c.din;
  row.tone = false;   // M5.Mic's callback turns the DAC clocks off: no stimulus
  row.m5mic = true;
  row.rate = kM5Rate;
  duplex::end();
  restoreBaseline();   // no clock: the codec takes I2C without one
  if (c.din == kDinAsFound) restoreBootPin();
  auto mc = M5.Mic.config();
  mc.pin_data_in = 46;
  mc.pin_ws = 43;
  mc.pin_bck = 41;
  mc.pin_mck = -1;
  mc.over_sampling = 1;
  mc.i2s_port = I2S_NUM_0;
  mc.sample_rate = kM5Rate;
  mc.stereo = 0;
  mc.left_channel = 0;   // input_only_right, the default
  M5.Mic.config(mc);
  MicAccess::setCb(M5.Mic, m5MicCb);
  bool ok = M5.Mic.begin();
  if (ok && c.din == kDinOutputOff) duplex::applyDinMode(kDinOutputOff);
  ToneDetector det;
  det.begin(&g_tab16);
  if (ok) {
    for (uint32_t t = 0; t < c.settleMs && ok; t += 100) ok = m5Record(nullptr);
    for (uint32_t t = 0; t < kCaptureMs && ok; t += 100) ok = m5Record(&det);
  }
  readRegs(row);
  diffRegs(row);
  M5.Mic.end();
  row.ran = ok;
  if (ok) {
    row.res[0] = det.result();
    row.res[1] = row.res[0];
    judge(row);
  } else {
    app::sp("[matrix] cfg %d %s: M5.Mic begin/record failed\n", i, c.name);
  }
}

// ---- the run -------------------------------------------------------------------
void job(void*) {
  const uint32_t t0 = millis();
  new (&g_rep) MicMatrixReport();   // in place: a temporary would be ~5 KB of this task's stack
  g_rep.n = kNumCfgs;
  for (int i = 0; i < kNumCfgs; ++i) g_rep.cfg[i].name = kCfgs[i].name;
  g_rep.pinBoot = duplex::pinBoot();
  g_rep.pinAfterI2s = duplex::pinAfterI2s();
  g_rep.chipValid = duplex::codecRead(0xFD, &g_rep.chip[0]) && duplex::codecRead(0xFE, &g_rep.chip[1]) &&
                    duplex::codecRead(0xFF, &g_rep.chip[2]);
  dumpRegs(g_rep.regsBefore, &g_rep.regsBeforeValid);
  memcpy(g_base, g_rep.regsBefore, sizeof(g_base));
  g_baseValid = g_rep.regsBeforeValid;
  const uint8_t userPga = duplex::micPga();
  const int userReg16 = duplex::micReg16();

  for (int i = 0; i < kNumCfgs; ++i) {
    g_progress.store(i);
    snprintf(g_status, sizeof(g_status), "matrix %d/%d %s", i + 1, kNumCfgs, kCfgs[i].name);
    if (kCfgs[i].m5mic) runM5MicCfg(i);
    else runDuplexCfg(i);
    if (i == 0) {
      snprintf(g_status, sizeof(g_status), "matrix: DIN pin test");
      pinTest();
    }
    const MicCfgRow& r = g_rep.cfg[i];
    app::sp("[matrix] %2d %-26s ran=%d L:%s rms %.1f tone %.1f snr %.1f | R:%s rms %.1f tone %.1f\n", i, r.name,
            r.ran ? 1 : 0, r.res[0].stuck ? "STUCK" : "live", static_cast<double>(r.res[0].acRmsDb),
            static_cast<double>(r.res[0].toneDb), static_cast<double>(r.res[0].snrDb),
            r.res[1].stuck ? "STUCK" : "live", static_cast<double>(r.res[1].acRmsDb),
            static_cast<double>(r.res[1].toneDb));
  }

  restoreBaseline();
  rankMicMatrix(g_rep);
  // The sampler can only run a duplex setup: take the best one whose best
  // slot is a live microphone slot (never the DAC reference).
  int apply = -1;
  for (int k = 0; k < g_rep.n; ++k) {
    const MicCfgRow& r = g_rep.cfg[g_rep.rank[k]];
    const int s = r.bestSlot ? 1 : 0;
    if (r.ran && !r.m5mic && r.micOk[s] && r.v[s].alive) {
      apply = g_rep.rank[k];
      break;
    }
  }
  duplex::setMicGain(userPga, userReg16);
  if (apply >= 0) {
    const MCfg& c = kCfgs[apply];
    if (c.din == kDinAsFound) restoreBootPin();
    g_rep.applied = duplex::restart(cfgFor(c, g_userVol));
    uint8_t r14 = 0x10;
    duplex::codecRead(0x14, &r14);
    duplex::setReg14Hi(r14);
    duplex::setMicGain(userPga, userReg16);
    g_appliedSlot = g_rep.cfg[apply].bestSlot;
  } else {
    duplex::Config dc;   // the boot default (esp_codec_dev + DIN fix)
    dc.sampleRate = app::g_sampleRate;
    dc.volumeReg = g_userVol;
    duplex::setReg14Hi(0x10);
    duplex::restart(dc);
  }
  dumpRegs(g_rep.regsAfter, &g_rep.regsAfterValid);
  g_rep.secs = static_cast<float>(millis() - t0) / 1000.0f;

  char v[48];
  micMatrixVerdict(g_rep, v, sizeof(v));
  snprintf(g_status, sizeof(g_status), "%s", v);
  g_works.store(g_rep.best >= 0);
  g_finished.store(true);
  g_appliedReq.store(g_rep.applied);
  g_printReq.store(true);
  g_running.store(false);
  vTaskDelete(nullptr);
}

}  // namespace

bool start() {
  if (g_running.load()) return false;
  if (!duplex::running()) {
    app::msg("audio not running");
    return false;
  }
  g_tab.init(kToneHz, app::g_sampleRate);
  g_tab16.init(kToneHz, kM5Rate);
  g_userVol = duplex::volumeReg();
  g_running.store(true);
  snprintf(g_status, sizeof(g_status), "matrix starting...");
  if (xTaskCreatePinnedToCore(job, "hws_matrix", 6144, nullptr, 5, nullptr, 1) != pdPASS) {
    g_running.store(false);
    snprintf(g_status, sizeof(g_status), "matrix: task create failed");
    return false;
  }
  return true;
}

bool running() { return g_running.load(); }
bool finished() { return g_finished.load(); }
bool works() { return g_works.load(); }
const char* statusLine() { return g_status; }

void process(const int16_t* in, int16_t* out, int frames) {
  const int st = g_cap.load(std::memory_order_acquire);
  if (st == kCapIdle || st == kCapDone) {
    memset(out, 0, sizeof(int16_t) * 2 * static_cast<size_t>(frames));
    return;
  }
  for (int i = 0; i < frames; ++i) {
    const int16_t s = g_toneOn ? g_gen.next() : 0;
    out[2 * i] = s;
    out[2 * i + 1] = s;
  }
  if (st == kCapSettle) {
    g_settleLeft -= frames;
    if (g_settleLeft <= 0) {
      g_det[0].clear();
      g_det[1].clear();
      g_cap.store(kCapRun, std::memory_order_release);
    }
    return;
  }
  int n = frames;
  if (n > g_capLeft) n = g_capLeft;
  for (int i = 0; i < n; ++i) {
    g_det[0].add(in[2 * i]);
    g_det[1].add(in[2 * i + 1]);
  }
  g_capLeft -= n;
  if (g_capLeft <= 0) g_cap.store(kCapDone, std::memory_order_release);
}

bool tick(int* slot) {
  if (g_printReq.exchange(false)) report();
  if (g_appliedReq.exchange(false)) {
    if (slot) *slot = g_appliedSlot;
    return true;
  }
  return false;
}

void report() {
  if (!g_finished.load()) {
    app::sp("[matrix] no run yet: SAMPLER page key m (or serial M)\n");
    return;
  }
  formatMicMatrixReport(g_rep, app::reportSink, nullptr);
}

}  // namespace micmatrix
}  // namespace hws
