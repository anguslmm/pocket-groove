// Platform layer: audio output. See audio_out.h for the contract.
//
// Checked against ESP-IDF v5.5.5 headers (esp_driver_i2s/include/driver/
// i2s_common.h, i2s_std.h) and M5Unified 0.2.29 (utility/Speaker_Class.*):
//  * i2s_channel_write() takes its timeout in MILLISECONDS, not ticks.
//  * CONFIG_I2S_ISR_IRAM_SAFE=1 in this Arduino core build, so event callbacks
//    must live in IRAM and touch only DRAM (i2s_channel_register_event_callback
//    rejects anything else).
//  * The TX "finished buffer" queue is dma_desc_num-1 deep; on_send_q_ovf fires
//    when a DMA buffer completes while that queue is still full, i.e. exactly
//    when the writer has fallen a whole ring behind and the DMA starts sending
//    auto-cleared (silent) buffers. That is our underrun counter.
//  * The legacy driver (driver/i2s.h) must never be linked: IDF 5 aborts at
//    boot if both drivers are present, and M5Unified uses the new one.
//  * M5.Speaker.playRaw() lazily calls Speaker.begin(), which writes the codec
//    over I2C. The audio task therefore only calls it while isRunning().
#include "audio_out.h"

#include <Arduino.h>
#include <M5Unified.h>

#include <driver/i2s_std.h>
#include <esp_cpu.h>
#include <esp_rom_sys.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <cstdio>
#include <cstring>

namespace plat {
namespace audio {

namespace {

constexpr uint8_t  kCodecAddr   = 0x18;       // ES8311
constexpr uint32_t kCodecI2cHz  = 100000;
constexpr int      kPinBclk     = 41;
constexpr int      kPinWs       = 43;
constexpr int      kPinDout     = 42;
constexpr int      kTaskPrio    = 20;         // above every app task, below esp_timer (22) and IPC (24)
constexpr uint32_t kTaskStack   = 6144;       // bytes; high-water mark is reported in Stats
constexpr int      kRotBuffers  = 4;          // M5Speaker backend: playRaw keeps pointers (current + 1 queued)
constexpr uint8_t  kM5MasterVol = 180;        // with magnification 16 this is ~unity gain (-0.17 dB), see begin

struct State {
  Config        cfg;
  RenderFn      render   = nullptr;
  void*         user     = nullptr;
  bool          running  = false;
  bool          codecOk  = false;
  char          err[64]  = "";

  i2s_chan_handle_t tx   = nullptr;
  TaskHandle_t  task     = nullptr;
  SemaphoreHandle_t done = nullptr;
  volatile bool stopReq  = false;
  volatile bool resetReq = false;

  uint32_t      periodCycles = 0;
  uint32_t      cyclesPerUs  = 240;

  // written by the audio task, read by anyone (32-bit loads/stores are atomic)
  volatile uint32_t blocks = 0, lateBlocks = 0, writeErrors = 0, overloadYields = 0;
  volatile uint32_t lastCyc = 0, maxCyc = 0, maxPeriodCyc = 0, stackFreeMin = 0;
  volatile float    avgCyc = 0.0f;
  volatile int      taskCore = -1;
};

State s;

// ISR side of the underrun counter. DRAM only (IRAM-safe ISR).
volatile uint32_t s_underruns = 0;
volatile bool     s_underrunArmed = false;

int16_t s_block[kBlockFrames * 2];                 // DirectI2S: one block, copied by i2s_channel_write
int16_t* s_rot = nullptr;                          // M5Speaker: kRotBuffers blocks, heap, allocated on demand

bool IRAM_ATTR onSendQueueOverflow(i2s_chan_handle_t, i2s_event_data_t*, void*) {
  if (s_underrunArmed) s_underruns = s_underruns + 1;
  return false;
}

void setError(const char* what, int code = 0) {
  if (code) snprintf(s.err, sizeof(s.err), "%s (0x%x)", what, code);
  else      snprintf(s.err, sizeof(s.err), "%s", what);
}

// ---- ES8311 ------------------------------------------------------------------
bool regWrite(uint8_t reg, uint8_t val) {
  for (int attempt = 0; attempt < 3; ++attempt) {
    if (M5.In_I2C.writeRegister(kCodecAddr, reg, &val, 1, kCodecI2cHz)) return true;
    delay(1);
  }
  return false;
}
bool regRead(uint8_t reg, uint8_t* val) {
  uint8_t v = 0;
  if (!M5.In_I2C.readRegister(kCodecAddr, reg, &v, 1, kCodecI2cHz)) return false;
  if (val) *val = v;
  return true;
}

// The DAC bring-up M5Unified 0.2.29 uses for this board
// (_speaker_enabled_cb_cardputer_adv), with our volume instead of 0 dB.
// The board has no MCLK line: reg 0x01 selects BCLK as the codec master clock
// and reg 0x02 multiplies it by 8, so 32 BCLK per frame (16-bit stereo slots)
// gives the codec its 256 fs clock at any sample rate.
bool es8311DacInit(uint8_t volume) {
  static const uint8_t seq[][2] = {
    {0x00, 0x80},   // reset released, chip state machine on
    {0x01, 0xB5},   // clock manager: MCLK source = BCLK, DAC clocks on
    {0x02, 0x18},   // clock manager: multiply x8
    {0x0D, 0x01},   // power up analog circuitry
    {0x12, 0x00},   // power up DAC
    {0x13, 0x10},   // enable output to the headphone driver
  };
  bool ok = true;
  for (auto& r : seq) ok = regWrite(r[0], r[1]) && ok;
  ok = regWrite(0x32, volume) && ok;   // DAC volume
  ok = regWrite(0x37, 0x08) && ok;     // bypass DAC equaliser
  return ok;
}

// ---- audio task ----------------------------------------------------------------
void audioTask(void*) {
  const bool direct = (s.cfg.backend == Backend::DirectI2S);
  const uint32_t period = s.periodCycles;
  const size_t bytes = kBlockFrames * 2 * sizeof(int16_t);
  uint32_t prevStart = 0;
  bool havePrev = false;
  uint32_t busyBlocks = 0;      // consecutive blocks that used ~the whole period
  uint32_t warmup = 0;
  int rot = 0;
  s.taskCore = xPortGetCoreID();

  while (!s.stopReq) {
    if (s.resetReq) {
      s.resetReq = false;
      s.blocks = 0; s.lateBlocks = 0; s.writeErrors = 0; s.overloadYields = 0;
      s.lastCyc = 0; s.maxCyc = 0; s.maxPeriodCyc = 0; s.avgCyc = 0.0f;
      s_underruns = 0;
      havePrev = false;
    }

    int16_t* out = direct ? s_block : (s_rot + rot * kBlockFrames * 2);

    const uint32_t t0 = esp_cpu_get_cycle_count();
    s.render(out, kBlockFrames, s.user);
    const uint32_t dt = esp_cpu_get_cycle_count() - t0;

    s.lastCyc = dt;
    if (dt > s.maxCyc) s.maxCyc = dt;
    const float avg = s.avgCyc;
    s.avgCyc = (avg == 0.0f) ? static_cast<float>(dt) : avg + (static_cast<float>(dt) - avg) * (1.0f / 64.0f);
    if (dt > period) s.lateBlocks = s.lateBlocks + 1;
    if (havePrev) {
      const uint32_t gap = t0 - prevStart;
      if (gap > s.maxPeriodCyc) s.maxPeriodCyc = gap;
    }
    prevStart = t0;
    havePrev = true;

    bool ok;
    if (direct) {
      size_t written = 0;
      // Blocks until a DMA buffer is free: this is what paces the loop.
      const esp_err_t e = i2s_channel_write(s.tx, out, bytes, &written, 200 /* ms */);
      ok = (e == ESP_OK && written == bytes);
    } else {
      if (M5.Speaker.isRunning()) {
        // Waits (1 ms steps) until the channel's queue slot is free.
        ok = M5.Speaker.playRaw(out, kBlockFrames * 2, s.cfg.sampleRate, true, 1, 0, false);
        rot = (rot + 1) % kRotBuffers;
      } else {
        ok = false;
        vTaskDelay(pdMS_TO_TICKS(3));
      }
    }
    if (!ok) {
      s.writeErrors = s.writeErrors + 1;
      vTaskDelay(2);                       // never spin on a dead driver
    }
    s.blocks = s.blocks + 1;

    if (warmup < 16 && ++warmup == 16) {   // ignore start-up while the DMA ring fills
      s_underruns = 0;
      s_underrunArmed = true;
    }

    // Overload guard. If rendering needs the whole period the driver never
    // blocks us, the idle task on this core starves and the task watchdog
    // (5 s, panic; it watches the core-0 idle task) would reboot the device;
    // on core 1 the UI loop would freeze instead. Give the core away for 2
    // ticks every 8 such blocks (~8% of the time): audible as drop-outs, but
    // the system stays alive and the UI can show what happened.
    if (dt + (period >> 4) >= period) {
      if (++busyBlocks >= 8) {
        busyBlocks = 0;
        s.overloadYields = s.overloadYields + 1;
        vTaskDelay(2);
        havePrev = false;
      }
    } else {
      busyBlocks = 0;
    }

    if ((s.blocks & 0xFF) == 0) {
      s.stackFreeMin = uxTaskGetStackHighWaterMark(nullptr);
    }
  }

  s.taskCore = -1;
  xSemaphoreGive(s.done);
  vTaskDelete(nullptr);
}

bool startTask() {
  if (!s.done) s.done = xSemaphoreCreateBinary();
  if (!s.done) { setError("no memory for task semaphore"); return false; }
  xSemaphoreTake(s.done, 0);
  s.stopReq = false;
  s.resetReq = true;
  s_underrunArmed = false;
  s_underruns = 0;
  s.stackFreeMin = kTaskStack;
  const BaseType_t core = (s.cfg.core < portNUM_PROCESSORS) ? s.cfg.core : 0;
  if (xTaskCreatePinnedToCore(audioTask, "pg_audio", kTaskStack, nullptr, kTaskPrio, &s.task, core) != pdPASS) {
    s.task = nullptr;
    setError("audio task create failed");
    return false;
  }
  return true;
}

void stopTask() {
  if (!s.task) return;
  s.stopReq = true;
  // The task wakes at the latest when its 200 ms write timeout expires.
  if (xSemaphoreTake(s.done, pdMS_TO_TICKS(1000)) != pdTRUE) {
    vTaskDelete(s.task);                   // last resort; should never happen
  }
  s.task = nullptr;
  s.taskCore = -1;
  s_underrunArmed = false;
}

// ---- backends ------------------------------------------------------------------
bool startDirect() {
  // Make sure M5.Speaker is not holding I2S_NUM_1.
  if (M5.Speaker.isRunning()) M5.Speaker.end();

  i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
  chan.dma_desc_num  = s.cfg.dmaBufCount;
  chan.dma_frame_num = kBlockFrames;
  chan.auto_clear    = true;               // underrun -> silence, not a stuck buffer
  esp_err_t e = i2s_new_channel(&chan, &s.tx, nullptr);
  if (e != ESP_OK) { s.tx = nullptr; setError("i2s_new_channel", e); return false; }

  i2s_std_config_t cfg = {};
  cfg.clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(s.cfg.sampleRate);       // PLL 160 MHz source, 256 fs internal MCLK
  cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
  cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;     // no MCLK trace on this board
  cfg.gpio_cfg.bclk = static_cast<gpio_num_t>(kPinBclk);
  cfg.gpio_cfg.ws   = static_cast<gpio_num_t>(kPinWs);
  cfg.gpio_cfg.dout = static_cast<gpio_num_t>(kPinDout);
  cfg.gpio_cfg.din  = I2S_GPIO_UNUSED;
  e = i2s_channel_init_std_mode(s.tx, &cfg);
  if (e != ESP_OK) { setError("i2s_channel_init_std_mode", e); i2s_del_channel(s.tx); s.tx = nullptr; return false; }

  i2s_event_callbacks_t cbs = {};
  cbs.on_send_q_ovf = onSendQueueOverflow;
  e = i2s_channel_register_event_callback(s.tx, &cbs, nullptr);
  if (e != ESP_OK) {
    // Not fatal: we only lose the underrun counter.
    setError("underrun callback not registered", e);
  }

  e = i2s_channel_enable(s.tx);            // BCLK/WS start here, DMA sends zeros
  if (e != ESP_OK) { setError("i2s_channel_enable", e); i2s_del_channel(s.tx); s.tx = nullptr; return false; }

  // Codec after the clock is up (it is clocked from BCLK). A missing or mute
  // codec is reported but does not stop the I2S stream: timing tests still work.
  delay(10);
  s.codecOk = es8311DacInit(s.cfg.volumeReg);
  if (!s.codecOk) setError("ES8311 did not ACK on I2C 0x18");
  return true;
}

void stopDirect() {
  if (!s.tx) return;
  if (s.codecOk) regWrite(0x32, 0x00);     // mute before the clock stops (the codec clicks otherwise)
  i2s_channel_disable(s.tx);
  i2s_del_channel(s.tx);
  s.tx = nullptr;
}

bool startM5() {
  if (!s_rot) {
    s_rot = static_cast<int16_t*>(heap_caps_malloc(kRotBuffers * kBlockFrames * 2 * sizeof(int16_t),
                                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (!s_rot) { setError("no memory for fallback buffers"); return false; }
  }
  memset(s_rot, 0, kRotBuffers * kBlockFrames * 2 * sizeof(int16_t));

  if (M5.Speaker.isRunning()) M5.Speaker.end();
  auto spk = M5.Speaker.config();          // board defaults stored by M5.begin(): mono, I2S_NUM_1, magnification 16
  if (spk.pin_data_out < 0) {              // board not recognised as a Cardputer: set the pins ourselves
    spk.pin_bck = kPinBclk; spk.pin_ws = kPinWs; spk.pin_data_out = kPinDout;
    spk.i2s_port = I2S_NUM_1;
  }
  spk.sample_rate      = s.cfg.sampleRate;
  // M5Unified's board default is stereo=false: it sums L+R into one value and
  // sends it in both slots (tx_mono / tx_chan_equal). We want the slot tests
  // to mean the same thing on both backends, so send real stereo frames.
  // Gain per channel is unchanged: the mixer doubles magnification for stereo
  // output (Speaker_Class.inl, "magnification << out_stereo").
  spk.stereo           = true;
  // The mixer pulls dma_buf_len frames per pass and only one request can wait
  // behind the playing one, so it must not want more than one of our blocks.
  spk.dma_buf_len      = kBlockFrames;
  spk.dma_buf_count    = 8;                // 8 x 128 = the 4 x 256 depth device-tested projects use
  spk.magnification    = 16;
  spk.task_priority    = kTaskPrio - 1;    // feeder (us) preempts the mixer, never the reverse
  spk.task_pinned_core = (s.cfg.core == 0) ? 1 : 0;   // mixer + resampler on the other core
  M5.Speaker.config(spk);
  // gain = magnification * master^2 * channel^2 / 2^36 (Speaker_Class.inl), x2
  // for stereo output: 2 * 16 * 180^2 * 255^2 / 2^36 = 0.98 per channel.
  M5.Speaker.setVolume(kM5MasterVol);
  M5.Speaker.setChannelVolume(0, 255);
  if (!M5.Speaker.begin()) {               // runs M5Unified's ES8311 init (0 dB), then installs I2S
    setError("M5.Speaker.begin failed");
    return false;
  }
  uint8_t id = 0;
  s.codecOk = regRead(0xFD, &id) && regWrite(0x32, s.cfg.volumeReg);
  if (!s.codecOk) setError("ES8311 did not ACK on I2C 0x18");
  return true;
}

void stopM5() {
  if (s.codecOk) regWrite(0x32, 0x00);
  M5.Speaker.end();
}

}  // namespace

// ---- public API ------------------------------------------------------------------
bool begin(const Config& cfgIn, RenderFn render, void* user) {
  if (s.running) end();
  s.err[0] = 0;
  if (!render) { setError("no render callback"); return false; }
  s.cfg = cfgIn;
  if (s.cfg.sampleRate < 8000 || s.cfg.sampleRate > 48000) s.cfg.sampleRate = 44100;
  if (s.cfg.dmaBufCount < 3)  s.cfg.dmaBufCount = 3;
  if (s.cfg.dmaBufCount > 16) s.cfg.dmaBufCount = 16;
  if (s.cfg.core > 1) s.cfg.core = 0;
  s.render = render;
  s.user = user;
  s.codecOk = false;
  s.cyclesPerUs = esp_rom_get_cpu_ticks_per_us();
  s.periodCycles = static_cast<uint32_t>((static_cast<uint64_t>(kBlockFrames) * s.cyclesPerUs * 1000000ull) /
                                         s.cfg.sampleRate);

  const bool ok = (s.cfg.backend == Backend::DirectI2S) ? startDirect() : startM5();
  if (!ok) return false;
  if (!startTask()) {
    if (s.cfg.backend == Backend::DirectI2S) stopDirect(); else stopM5();
    return false;
  }
  s.running = true;
  return true;
}

void end() {
  if (!s.running) return;
  stopTask();                              // join first: nobody is inside the driver afterwards
  if (s.cfg.backend == Backend::DirectI2S) stopDirect(); else stopM5();
  s.running = false;
}

bool reconfigure(const Config& cfg) {
  RenderFn r = s.render;
  void* u = s.user;
  end();
  return begin(cfg, r, u);
}

bool running() { return s.running; }
const Config& config() { return s.cfg; }
const char* lastError() { return s.err; }

bool codecPresent() { return s.codecOk; }

bool setVolumeReg(uint8_t reg) {
  s.cfg.volumeReg = reg;
  if (!s.running) return false;
  return regWrite(0x32, reg);
}
uint8_t volumeReg() { return s.cfg.volumeReg; }
float volumeDb() { return (static_cast<int>(s.cfg.volumeReg) - 0xBF) * 0.5f; }

bool codecInit() {
  s.codecOk = es8311DacInit(s.cfg.volumeReg);
  return s.codecOk;
}
bool codecWrite(uint8_t reg, uint8_t value) { return regWrite(reg, value); }
bool codecRead(uint8_t reg, uint8_t* value) { return regRead(reg, value); }

Stats stats() {
  Stats st;
  const float perUs = 1.0f / static_cast<float>(s.cyclesPerUs ? s.cyclesPerUs : 240);
  st.blocks         = s.blocks;
  st.underruns      = s_underruns;
  st.underrunsValid = (s.cfg.backend == Backend::DirectI2S);
  st.lateBlocks     = s.lateBlocks;
  st.writeErrors    = s.writeErrors;
  st.overloadYields = s.overloadYields;
  st.lastUs         = static_cast<float>(s.lastCyc) * perUs;
  st.maxUs          = static_cast<float>(s.maxCyc) * perUs;
  st.avgUs          = s.avgCyc * perUs;
  st.maxPeriodUs    = static_cast<float>(s.maxPeriodCyc) * perUs;
  st.blockPeriodUs  = static_cast<float>(s.periodCycles) * perUs;
  st.cpuPercent     = (s.periodCycles != 0) ? 100.0f * s.avgCyc / static_cast<float>(s.periodCycles) : 0.0f;
  st.stackFreeMin   = s.stackFreeMin;
  st.taskCore       = s.taskCore;
  return st;
}

void resetStats() {
  if (s.running) s.resetReq = true;
}

uint32_t latencyFrames() {
  if (s.cfg.backend == Backend::DirectI2S) {
    // one block being rendered + the whole DMA ring ahead of it
    return static_cast<uint32_t>(kBlockFrames) * (1u + s.cfg.dmaBufCount);
  }
  // rendered block + 1 queued + 1 playing in M5's mixer + its 8 x 128 DMA ring + 1-sample resampler delay
  return static_cast<uint32_t>(kBlockFrames) * (3u + 8u) + 1u;
}

}  // namespace audio
}  // namespace plat
