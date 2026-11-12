// Pocket Groove hardware spike -- full-duplex audio: ES8311 DAC + ADC on one
// I2S port (I2S_NUM_1, BCLK 41, WS 43, DOUT 42, DIN 46, no MCLK).
//
// Why not plat::audio: it is TX only (din unused). Recording needs RX on the
// same port, sharing the clock, which is what espp and the device-tested Rust
// BSP do (docs/hardware-notes.md 2.5, 3.2). Everything here is self-contained
// so no platform file changes.
//
// The audio task (core 0, priority 20, like plat::audio) runs:
//     i2s_channel_read(rx)  ->  process(in, out)  ->  i2s_channel_write(tx)
// in 128-frame stereo blocks; both directions run off the same clock, so the
// TX write paces the loop and RX is read in lockstep.
//
// No Arduino / ESP types in this header.
#pragma once
#include <cstddef>
#include <cstdint>

#include "report.h"   // PinSnap (portable)

namespace hws {
namespace duplex {

constexpr int kBlockFrames = 128;

// in: kBlockFrames stereo frames from the ADC (L,R,...); out: the same for the DAC.
using ProcessFn = void (*)(const int16_t* in, int16_t* out, int frames, void* user);

// Codec setup hook: writes the ES8311 registers (except the DAC volume, which
// begin() ramps afterwards). nullptr = defaultCodecInit().
using CodecFn = bool (*)(void* user);

// What begin() does to GPIO46 (I2S data in) after the RX channel is set up.
// M5.begin() on any ESP32-S3 drives GPIO46 HIGH as an output (power hold for
// Capsule / Dial / DinMeter) and restores it only on a few boards, not the
// Cardputer-Adv (M5Unified 0.2.29 M5Unified.hpp, begin()). ESP-IDF 5.4 set
// the I2S input pin with gpio_set_direction(INPUT), which switched that driver
// off; 5.5 only calls gpio_input_enable() (esp_driver_i2s/i2s_common.c,
// i2s_gpio_check_and_set), so the ESP keeps driving the line itself.
enum DinMode : uint8_t {
  kDinAsFound = 0,      // leave it as the driver left it (the original spike)
  kDinOutputOff = 1,    // gpio_output_disable(46): the fix
  kDinForcedHigh = 2,   // drive it high on purpose (reproduces M5.begin's state)
  kDinOffPulldown = 3,  // output off + weak pull-down (a floating line then reads 0)
};

struct Config {
  uint32_t sampleRate = 44100;
  uint8_t  dmaDesc = 6;       // DMA buffers of kBlockFrames each direction (6 x 128 = 17.4 ms at 44.1 kHz)
  uint8_t  volumeReg = 0x80;  // ES8311 reg 0x32 (0xBF = 0 dB, 0.5 dB/step); 0x80 = -31.5 dB, safe for earbuds
  uint8_t  slotBits = 16;     // 16: 32 BCLK per frame (32 fs); 32: 64 fs (data stays 16-bit, MSB first)
  uint8_t  dinMode = kDinAsFound;
  CodecFn  codec = nullptr;
  void*    codecUser = nullptr;
  bool     codecBeforeClock = false;   // write the codec before the I2S clocks start (M5Unified's order)
};

struct Stats {
  uint32_t blocks;
  uint32_t rxOverflows;   // RX DMA buffers overwritten before we read them = dropped input blocks
  uint32_t txUnderruns;   // TX DMA buffers sent without fresh data
  uint32_t rxErrors, txErrors;
  float    avgUs, maxUs;  // process() time per block
  float    maxRxGapUs;    // longest time between two consecutive RX block returns
  float    blockPeriodUs;
  float    cpuPercent;    // avgUs / blockPeriodUs
  uint32_t stackFree;
};

bool begin(const Config& cfg, ProcessFn fn, void* user);
bool restart(const Config& cfg);   // end() + begin() with the same process callback
void end();
bool running();
const char* lastError();
const Config& config();

// Codec (UI thread only: I2C).
bool    codecPresent();
bool    setVolumeReg(uint8_t reg);
uint8_t volumeReg();
// Microphone path. pgaNibble 0..10 = 0..30 dB analog PGA (reg 0x14 = 0x10 | n);
// reg16 < 0 restores the value read at begin() (the references never write it).
bool    setMicGain(uint8_t pgaNibble, int reg16);
uint8_t bootReg16();
bool    codecRead(uint8_t reg, uint8_t* v);
bool    codecWrite(uint8_t reg, uint8_t v);
// The spike's own sequence (DAC: the instrument's; ADC: the Rust BSP's), with
// reg 0x14 = reg14Hi | PGA. Usable as a building block by other CodecFns.
bool    defaultCodecInit();
void    setReg14Hi(uint8_t hi);    // input select / DMIC bits kept by setMicGain (default 0x10)
uint8_t micPga();
int     micReg16();                // -1 = power-on value

// GPIO46 register snapshots: at the first begin() before any I2S call (as
// M5.begin() left it), and right after the RX channel was initialised (before
// dinMode is applied). readPin() reads it now.
PinSnap pinBoot();
PinSnap pinAfterI2s();
PinSnap readPin();
void    applyDinMode(uint8_t mode);
// Fraction of `reads` back-to-back reads of the GPIO46 pad that are 1.
float   sampleDinOnes(uint32_t reads);
void    setDinPulls(bool up, bool down);

Stats stats();
void  resetStats();          // applied by the audio task at its next block
void  resetRxGap();          // only the max-gap / overflow counters (recording start)
// Worst-case render-to-DAC latency in frames (one block + the TX DMA ring).
uint32_t latencyFrames();

}  // namespace duplex
}  // namespace hws
