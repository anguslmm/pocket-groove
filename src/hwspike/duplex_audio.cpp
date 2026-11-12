// Full-duplex ES8311 audio for the hardware spike. See duplex_audio.h.
//
// Boot default (docs/hardware-notes.md 11.7): esp_codec_dev's es8311_open +
// set_fs + start (espCodecDevInit), exactly the sequence the device recorded
// with as mic-matrix config 6 on 2026-11-12, then the gain preset (0x14 /
// 0x16), with GPIO46's output driver switched off after the RX channel is set
// up (kDinOutputOff: the confirmed root cause of the constant -1). Loopback
// off (0x44 = 0x08, both slots carry the ADC; the page records the left one).
//
// spikeCodecInit(), the first default, is kept for the matrix. Its sources
// (docs/hardware-notes.md cites them; read in an earlier session):
//  * DAC: M5Unified 0.2.29 _speaker_enabled_cb_cardputer_adv, as
//    src/platform/audio_out.cpp uses it (the instrument's proven sequence,
//    including reg 0x09 = 0x00 and the volume ramp).
//  * ADC: the device-tested Rust BSP (tszarzynski/cardputer-adv @2de5578,
//    src/codec.rs set_microphone): 0x01 = 0xBF (DAC + ADC clocks, also espp's
//    value), 0x0A = 0x0C (ADC serial port I2S, 16-bit), 0x0E = 0x02 (PGA and
//    ADC modulator on), 0x14 = 0x10 | gain (MIC1P/MIC1N, PGA 0..10 = 0..30 dB
//    in 3 dB steps; guitar-audio-tools, also tested on the ADV, says the
//    same), 0x17 = 0xBF (ADC volume 0 dB), 0x1C = 0x6A (ADC equaliser bypass,
//    DC offset removed). Same set as M5Unified's microphone callback minus
//    its reset of the DAC clocks.
//  * reg 0x16 ("mic gain scale", 0..7 = 0..42 dB in Espressif's es8311
//    driver as copied by espp; that driver writes 0x24 at init): not written
//    by the device-tested references, so it is left at its power-on value
//    unless a gain preset asks for Espressif's 0x24 explicitly.
//  * The Rust BSP reads stereo frames and takes the LEFT sample ("the codec's
//    one ADC is in both channels"); this firmware measures both slots.
//
// ESP-IDF v5.5.5 (esp_driver_i2s/include/driver/i2s_common.h):
//  * i2s_new_channel(&cfg, &tx, &rx) with both handles = full duplex on one
//    port, shared clock; both channels are initialised with the same std config.
//  * on_recv_q_ovf fires when the RX queue is full and a DMA buffer is
//    overwritten: that is a dropped input block. CONFIG_I2S_ISR_IRAM_SAFE=1, so
//    the callbacks are IRAM_ATTR and touch DRAM only.
//  * read/write timeouts are milliseconds.
#include "duplex_audio.h"

#include <Arduino.h>
#include <M5Unified.h>

#include <driver/gpio.h>
#include <driver/i2s_std.h>
#include <esp_cpu.h>
#include <soc/gpio_reg.h>
#include <soc/gpio_sig_map.h>
#include <soc/io_mux_reg.h>
#include <esp_rom_sys.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <cstdio>
#include <cstring>

namespace hws {
namespace duplex {

namespace {

constexpr uint8_t  kCodecAddr = 0x18;
constexpr uint32_t kCodecHz = 100000;
constexpr int kPinBclk = 41, kPinWs = 43, kPinDout = 42, kPinDin = 46;
constexpr int kTaskPrio = 20;
constexpr uint32_t kTaskStack = 6144;

Config s_cfg;
ProcessFn s_fn = nullptr;
void* s_user = nullptr;
bool s_running = false, s_codecOk = false;
char s_err[64] = "";
i2s_chan_handle_t s_tx = nullptr, s_rx = nullptr;
TaskHandle_t s_task = nullptr;
SemaphoreHandle_t s_done = nullptr;
volatile bool s_stop = false, s_reset = false, s_resetGap = false;
uint8_t s_boot16 = 0x00;
bool s_boot16Read = false;

int16_t s_in[kBlockFrames * 2];
int16_t s_out[kBlockFrames * 2];

volatile uint32_t s_rxOvf = 0, s_txUnd = 0;
volatile bool s_armed = false;
volatile uint32_t s_blocks = 0, s_rxErr = 0, s_txErr = 0, s_maxCyc = 0, s_maxGapUs = 0, s_stackFree = 0;
volatile float s_avgCyc = 0;
uint32_t s_periodUs = 0;

bool IRAM_ATTR onRxOvf(i2s_chan_handle_t, i2s_event_data_t*, void*) {
  if (s_armed) s_rxOvf = s_rxOvf + 1;
  return false;
}
bool IRAM_ATTR onTxOvf(i2s_chan_handle_t, i2s_event_data_t*, void*) {
  if (s_armed) s_txUnd = s_txUnd + 1;
  return false;
}

void setErr(const char* what, int code = 0) {
  if (code) snprintf(s_err, sizeof(s_err), "%s (0x%x)", what, code);
  else snprintf(s_err, sizeof(s_err), "%s", what);
}

bool wr(uint8_t reg, uint8_t val) {
  for (int i = 0; i < 3; ++i) {
    if (M5.In_I2C.writeRegister(kCodecAddr, reg, &val, 1, kCodecHz)) return true;
    delay(1);
  }
  return false;
}
bool rd(uint8_t reg, uint8_t* v) { return M5.In_I2C.readRegister(kCodecAddr, reg, v, 1, kCodecHz); }


PinSnap s_pinBoot, s_pinI2s;
bool s_pinBootTaken = false;

PinSnap snapPin() {
  constexpr int pin = kPinDin;
  constexpr uint32_t bit = 1u << (pin - 32);
  PinSnap p;
  p.valid = true;
  p.oe = (REG_READ(GPIO_ENABLE1_REG) & bit) ? 1 : 0;
  p.out = (REG_READ(GPIO_OUT1_REG) & bit) ? 1 : 0;
  p.level = (REG_READ(GPIO_IN1_REG) & bit) ? 1 : 0;
  p.outSel = static_cast<uint16_t>(REG_READ(GPIO_FUNC0_OUT_SEL_CFG_REG + 4 * pin) & 0xFFF);
  const uint32_t mux = REG_READ(IO_MUX_GPIO46_REG);
  p.ie = (mux & FUN_IE) ? 1 : 0;
  p.wpu = (mux & FUN_PU) ? 1 : 0;
  p.wpd = (mux & FUN_PD) ? 1 : 0;
  p.mcuSel = static_cast<uint8_t>((mux >> MCU_SEL_S) & MCU_SEL);
  // I2S1 data-in signal: which GPIO feeds it (bit 7 = through the matrix)
  const uint32_t in = REG_READ(GPIO_FUNC0_IN_SEL_CFG_REG + 4 * I2S1I_SD_IN_IDX);
  p.inSel = static_cast<uint8_t>(in & 0xFF);
  return p;
}

uint8_t s_pga = 10;        // reg 0x14 low nibble
uint8_t s_reg14Hi = 0x10;  // reg 0x14 high bits: MIC1P/MIC1N, no DMIC
int s_reg16 = 0x24;        // esp_codec_dev's value; < 0 = the value read before the first init

void readBoot16() {
  if (s_boot16Read) return;
  uint8_t v = 0;
  if (rd(0x16, &v)) {
    s_boot16 = v;
    s_boot16Read = true;
  }
}

bool writeGainRegs() {
  bool ok = wr(0x14, static_cast<uint8_t>(s_reg14Hi | (s_pga & 0x0F)));
  ok = wr(0x16, s_reg16 < 0 ? s_boot16 : static_cast<uint8_t>(s_reg16)) && ok;
  return ok;
}

bool codecInit() {
  if (s_cfg.codec) return s_cfg.codec(s_cfg.codecUser);
  return defaultCodecInit();
}

}  // namespace

bool spikeCodecInit() {
  readBoot16();
  static const uint8_t seq[][2] = {
      {0x00, 0x80},   // reset released, chip state machine on
      {0x01, 0xBF},   // clock manager: MCLK from BCLK, DAC and ADC clocks on
      {0x02, 0x18},   // clock manager: multiply x8 (256 fs at 32 BCLK per frame)
      {0x32, 0x00},   // DAC volume muted until the ramp
      {0x0D, 0x01},   // power up analog circuitry
      {0x12, 0x00},   // power up DAC
      {0x13, 0x10},   // output to the headphone driver
      {0x37, 0x08},   // bypass DAC equaliser
      {0x09, 0x00},   // DAC serial port: power-on default (the instrument writes the same)
      {0x0A, 0x0C},   // ADC serial port: I2S, 16-bit
      {0x0E, 0x02},   // PGA and ADC modulator on
      {0x17, 0xBF},   // ADC volume 0 dB
      {0x1C, 0x6A},   // ADC equaliser bypass, DC offset removal
  };
  bool ok = true;
  for (auto& r : seq) ok = wr(r[0], r[1]) && ok;
  return writeGainRegs() && ok;
}

bool espCodecDevInit(bool dacRef) {
  readBoot16();
  uint8_t v = 0;
  auto R = [&v](uint8_t reg) -> uint8_t {
    v = 0;
    rd(reg, &v);
    return v;
  };
  bool ok = true;
  // es8311_open (slave, use_mclk = false, no inversion)
  if (R(0x0D) != 0xFA) ok = wr(0x0D, 0xFA) && ok;
  ok = wr(0x44, 0x08) && ok;   // "enhance I2C noise immunity", written twice by the driver
  ok = wr(0x44, 0x08) && ok;
  static const uint8_t open1[][2] = {{0x01, 0x30}, {0x02, 0x00}, {0x03, 0x10}, {0x16, 0x24}, {0x04, 0x10}, {0x05, 0x00},
                                     {0x0B, 0x00}, {0x0C, 0x00}, {0x10, 0x1F}, {0x11, 0x7F}, {0x00, 0x80}};
  for (auto& r : open1) ok = wr(r[0], r[1]) && ok;
  ok = wr(0x00, static_cast<uint8_t>(R(0x00) & 0xBF)) && ok;             // slave
  ok = wr(0x01, 0xBF) && ok;                                             // 0x3F | MCLK from SCLK
  ok = wr(0x06, static_cast<uint8_t>(R(0x06) & ~0x20)) && ok;            // SCLK not inverted
  ok = wr(0x13, 0x10) && ok;
  ok = wr(0x1B, 0x0A) && ok;
  ok = wr(0x1C, 0x6A) && ok;
  ok = wr(0x44, dacRef ? 0x58 : 0x08) && ok;                             // 0x58: R slot = DAC reference
  // es8311_set_fs: 16 bit, I2S normal, 44.1 kHz with MCLK = 256 fs
  const uint8_t d = static_cast<uint8_t>(R(0x09) | 0x0C), a = static_cast<uint8_t>(R(0x0A) | 0x0C);
  ok = wr(0x09, d) && ok;
  ok = wr(0x0A, a) && ok;
  ok = wr(0x09, static_cast<uint8_t>(R(0x09) & 0xFC)) && ok;
  ok = wr(0x0A, static_cast<uint8_t>(R(0x0A) & 0xFC)) && ok;
  ok = wr(0x02, static_cast<uint8_t>((R(0x02) & 0x07) | (0 << 5) | (3 << 3))) && ok;
  ok = wr(0x05, 0x00) && ok;
  ok = wr(0x03, static_cast<uint8_t>((R(0x03) & 0x80) | (0 << 6) | 0x10)) && ok;
  ok = wr(0x04, static_cast<uint8_t>((R(0x04) & 0x80) | 0x10)) && ok;
  ok = wr(0x07, static_cast<uint8_t>((R(0x07) & 0xC0) | 0x00)) && ok;
  ok = wr(0x08, 0xFF) && ok;
  ok = wr(0x06, static_cast<uint8_t>((R(0x06) & 0xE0) | (4 - 1))) && ok;
  // es8311_start (BOTH, analog mic)
  ok = wr(0x00, 0x80) && ok;
  ok = wr(0x01, 0xBF) && ok;
  ok = wr(0x09, static_cast<uint8_t>(R(0x09) & 0xBF)) && ok;
  ok = wr(0x0A, static_cast<uint8_t>(R(0x0A) & 0xBF)) && ok;
  static const uint8_t start[][2] = {{0x17, 0xBF}, {0x0E, 0x02}, {0x12, 0x00}, {0x14, 0x1A}};
  for (auto& r : start) ok = wr(r[0], r[1]) && ok;
  ok = wr(0x14, static_cast<uint8_t>(R(0x14) & ~0x40)) && ok;
  static const uint8_t start2[][2] = {{0x0D, 0x01}, {0x15, 0x40}, {0x37, 0x08}, {0x45, 0x00}};
  for (auto& r : start2) ok = wr(r[0], r[1]) && ok;
  return ok;
}

bool defaultCodecInit() {
  const bool ok = espCodecDevInit(false);
  return writeGainRegs() && ok;
}

namespace {

bool ramp(uint8_t from, uint8_t to) {
  int v = from;
  bool ok = true;
  while (v != to) {
    const int d = static_cast<int>(to) - v;
    v += (d > 8) ? 8 : (d < -8) ? -8 : d;
    ok = wr(0x32, static_cast<uint8_t>(v)) && ok;
    delay(1);
  }
  return ok;
}

void audioTask(void*) {
  const size_t bytes = sizeof(s_in);
  int64_t prevRx = 0;
  uint32_t warm = 0;
  while (!s_stop) {
    if (s_reset) {
      s_reset = false;
      s_blocks = 0; s_rxErr = 0; s_txErr = 0; s_maxCyc = 0; s_maxGapUs = 0; s_avgCyc = 0;
      s_rxOvf = 0; s_txUnd = 0;
      prevRx = 0;
    }
    if (s_resetGap) {
      s_resetGap = false;
      s_maxGapUs = 0;
      s_rxOvf = 0;
    }
    size_t got = 0;
    const esp_err_t er = i2s_channel_read(s_rx, s_in, bytes, &got, 100);
    const int64_t now = esp_timer_get_time();
    if (er != ESP_OK || got != bytes) {
      s_rxErr = s_rxErr + 1;
      memset(s_in, 0, bytes);
    }
    if (prevRx != 0 && warm >= 16) {
      const uint32_t gap = static_cast<uint32_t>(now - prevRx);
      if (gap > s_maxGapUs) s_maxGapUs = gap;
    }
    prevRx = now;

    const uint32_t t0 = esp_cpu_get_cycle_count();
    s_fn(s_in, s_out, kBlockFrames, s_user);
    const uint32_t dt = esp_cpu_get_cycle_count() - t0;
    if (dt > s_maxCyc && warm >= 16) s_maxCyc = dt;
    const float a = s_avgCyc;
    s_avgCyc = (a == 0.0f) ? static_cast<float>(dt) : a + (static_cast<float>(dt) - a) * (1.0f / 64.0f);

    size_t put = 0;
    const esp_err_t ew = i2s_channel_write(s_tx, s_out, bytes, &put, 100);
    if (ew != ESP_OK || put != bytes) {
      s_txErr = s_txErr + 1;
      vTaskDelay(1);
    }
    s_blocks = s_blocks + 1;
    if (warm < 16 && ++warm == 16) {
      s_rxOvf = 0;
      s_txUnd = 0;
      s_armed = true;
    }
    if ((s_blocks & 0xFF) == 0) s_stackFree = uxTaskGetStackHighWaterMark(nullptr);
  }
  xSemaphoreGive(s_done);
  vTaskDelete(nullptr);
}

}  // namespace

bool begin(const Config& cfg, ProcessFn fn, void* user) {
  if (s_running) end();
  s_err[0] = 0;
  if (!fn) { setErr("no process callback"); return false; }
  s_cfg = cfg;
  if (s_cfg.dmaDesc < 3) s_cfg.dmaDesc = 3;
  if (s_cfg.dmaDesc > 16) s_cfg.dmaDesc = 16;
  s_fn = fn;
  s_user = user;
  s_periodUs = static_cast<uint32_t>(1000000ull * kBlockFrames / s_cfg.sampleRate);
  if (M5.Speaker.isRunning()) M5.Speaker.end();
  if (!s_pinBootTaken) {
    s_pinBoot = snapPin();
    s_pinBootTaken = true;
  }
  if (s_cfg.codecBeforeClock) {
    s_codecOk = codecInit();   // no clock yet: the codec takes I2C without one (M5Unified's order)
    delay(5);
  }

  i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
  chan.dma_desc_num = s_cfg.dmaDesc;
  chan.dma_frame_num = kBlockFrames;
  chan.auto_clear = true;
  esp_err_t e = i2s_new_channel(&chan, &s_tx, &s_rx);
  if (e != ESP_OK) { s_tx = s_rx = nullptr; setErr("i2s_new_channel", e); return false; }

  i2s_std_config_t std = {};
  std.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(s_cfg.sampleRate);
  std.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
  if (s_cfg.slotBits == 32) {
    // 64 BCLK per frame; 16 data bits MSB first in each 32-bit slot, WS one slot wide
    std.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;
    std.slot_cfg.ws_width = 32;
  } else {
    s_cfg.slotBits = 16;
  }
  std.gpio_cfg.mclk = I2S_GPIO_UNUSED;
  std.gpio_cfg.bclk = static_cast<gpio_num_t>(kPinBclk);
  std.gpio_cfg.ws = static_cast<gpio_num_t>(kPinWs);
  std.gpio_cfg.dout = static_cast<gpio_num_t>(kPinDout);
  std.gpio_cfg.din = static_cast<gpio_num_t>(kPinDin);
  e = i2s_channel_init_std_mode(s_tx, &std);
  if (e == ESP_OK) e = i2s_channel_init_std_mode(s_rx, &std);
  if (e != ESP_OK) {
    setErr("i2s_channel_init_std_mode", e);
    i2s_del_channel(s_tx);
    i2s_del_channel(s_rx);
    s_tx = s_rx = nullptr;
    return false;
  }
  s_pinI2s = snapPin();
  applyDinMode(s_cfg.dinMode);
  i2s_event_callbacks_t rxcb = {};
  rxcb.on_recv_q_ovf = onRxOvf;
  i2s_channel_register_event_callback(s_rx, &rxcb, nullptr);
  i2s_event_callbacks_t txcb = {};
  txcb.on_send_q_ovf = onTxOvf;
  i2s_channel_register_event_callback(s_tx, &txcb, nullptr);

  e = i2s_channel_enable(s_tx);
  if (e == ESP_OK) e = i2s_channel_enable(s_rx);
  if (e != ESP_OK) {
    setErr("i2s_channel_enable", e);
    i2s_del_channel(s_tx);
    i2s_del_channel(s_rx);
    s_tx = s_rx = nullptr;
    return false;
  }
  delay(10);   // the codec is clocked from BCLK: clock first, then registers
  if (!s_cfg.codecBeforeClock) s_codecOk = codecInit();
  if (!s_codecOk) setErr("ES8311 did not ACK on I2C 0x18");

  if (!s_done) s_done = xSemaphoreCreateBinary();
  xSemaphoreTake(s_done, 0);
  s_stop = false;
  s_reset = true;
  s_armed = false;
  if (xTaskCreatePinnedToCore(audioTask, "hws_audio", kTaskStack, nullptr, kTaskPrio, &s_task, 0) != pdPASS) {
    setErr("audio task create failed");
    s_task = nullptr;
    return false;
  }
  s_running = true;
  if (s_codecOk) ramp(0x00, s_cfg.volumeReg);
  return true;
}

void end() {
  if (!s_running) return;
  if (s_codecOk) ramp(s_cfg.volumeReg, 0x00);
  s_stop = true;
  if (s_task && xSemaphoreTake(s_done, pdMS_TO_TICKS(1000)) != pdTRUE) vTaskDelete(s_task);
  s_task = nullptr;
  s_armed = false;
  i2s_channel_disable(s_rx);
  i2s_channel_disable(s_tx);
  i2s_del_channel(s_rx);
  i2s_del_channel(s_tx);
  s_rx = s_tx = nullptr;
  s_running = false;
}

bool running() { return s_running; }
const char* lastError() { return s_err; }
const Config& config() { return s_cfg; }
bool codecPresent() { return s_codecOk; }

bool setVolumeReg(uint8_t reg) {
  s_cfg.volumeReg = reg;
  return s_running && wr(0x32, reg);
}
uint8_t volumeReg() { return s_cfg.volumeReg; }

bool setMicGain(uint8_t pgaNibble, int reg16) {
  s_pga = pgaNibble > 10 ? 10 : pgaNibble;
  s_reg16 = reg16;
  return writeGainRegs();
}
uint8_t bootReg16() { return s_boot16; }
bool codecRead(uint8_t reg, uint8_t* v) { return rd(reg, v); }
bool codecWrite(uint8_t reg, uint8_t v) { return wr(reg, v); }
void setReg14Hi(uint8_t hi) { s_reg14Hi = static_cast<uint8_t>(hi & 0xF0); }
uint8_t micPga() { return s_pga; }
int micReg16() { return s_reg16; }

bool restart(const Config& cfg) {
  ProcessFn fn = s_fn;
  void* user = s_user;
  if (!fn) {
    setErr("restart before begin");
    return false;
  }
  end();
  return begin(cfg, fn, user);
}

PinSnap pinBoot() { return s_pinBoot; }
PinSnap pinAfterI2s() { return s_pinI2s; }
PinSnap readPin() { return snapPin(); }

void applyDinMode(uint8_t mode) {
  const gpio_num_t pin = static_cast<gpio_num_t>(kPinDin);
  switch (mode) {
    case kDinOutputOff:
      // The call ESP-IDF 5.4's i2s_gpio_check_and_set made: input on, output
      // driver off. The I2S data-in routing (GPIO matrix in_sel) is untouched.
      gpio_set_direction(pin, GPIO_MODE_INPUT);
      gpio_pullup_dis(pin);
      gpio_pulldown_dis(pin);
      break;
    case kDinForcedHigh:
      // what M5.begin() leaves: GPIO output, level 1, input still enabled for the I2S
      REG_WRITE(GPIO_FUNC0_OUT_SEL_CFG_REG + 4 * kPinDin, SIG_GPIO_OUT_IDX);
      gpio_set_level(pin, 1);
      REG_WRITE(GPIO_ENABLE1_W1TS_REG, 1u << (kPinDin - 32));
      break;
    case kDinOffPulldown:
      gpio_set_direction(pin, GPIO_MODE_INPUT);
      gpio_pullup_dis(pin);
      gpio_pulldown_en(pin);
      break;
    default:
      break;
  }
}

float sampleDinOnes(uint32_t reads) {
  if (reads == 0) return -1.0f;
  constexpr uint32_t bit = 1u << (kPinDin - 32);
  uint32_t ones = 0;
  for (uint32_t i = 0; i < reads; ++i) ones += (REG_READ(GPIO_IN1_REG) & bit) ? 1u : 0u;
  return static_cast<float>(ones) / static_cast<float>(reads);
}

void setDinPulls(bool up, bool down) {
  const gpio_num_t pin = static_cast<gpio_num_t>(kPinDin);
  if (up) gpio_pullup_en(pin); else gpio_pullup_dis(pin);
  if (down) gpio_pulldown_en(pin); else gpio_pulldown_dis(pin);
}

Stats stats() {
  Stats st;
  const float perUs = 1.0f / static_cast<float>(esp_rom_get_cpu_ticks_per_us());
  st.blocks = s_blocks;
  st.rxOverflows = s_rxOvf;
  st.txUnderruns = s_txUnd;
  st.rxErrors = s_rxErr;
  st.txErrors = s_txErr;
  st.avgUs = s_avgCyc * perUs;
  st.maxUs = static_cast<float>(s_maxCyc) * perUs;
  st.maxRxGapUs = static_cast<float>(s_maxGapUs);
  st.blockPeriodUs = static_cast<float>(s_periodUs);
  st.cpuPercent = s_periodUs ? 100.0f * st.avgUs / static_cast<float>(s_periodUs) : 0.0f;
  st.stackFree = s_stackFree;
  return st;
}

void resetStats() { s_reset = true; }
void resetRxGap() { s_resetGap = true; }

uint32_t latencyFrames() { return static_cast<uint32_t>(kBlockFrames) * (1u + s_cfg.dmaDesc); }

}  // namespace duplex
}  // namespace hws
