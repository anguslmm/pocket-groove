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

namespace hws {
namespace duplex {

constexpr int kBlockFrames = 128;

// in: kBlockFrames stereo frames from the ADC (L,R,...); out: the same for the DAC.
using ProcessFn = void (*)(const int16_t* in, int16_t* out, int frames, void* user);

struct Config {
  uint32_t sampleRate = 44100;
  uint8_t  dmaDesc = 6;       // DMA buffers of kBlockFrames each direction (6 x 128 = 17.4 ms at 44.1 kHz)
  uint8_t  volumeReg = 0x80;  // ES8311 reg 0x32 (0xBF = 0 dB, 0.5 dB/step); 0x80 = -31.5 dB, safe for earbuds
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

Stats stats();
void  resetStats();          // applied by the audio task at its next block
void  resetRxGap();          // only the max-gap / overflow counters (recording start)
// Worst-case render-to-DAC latency in frames (one block + the TX DMA ring).
uint32_t latencyFrames();

}  // namespace duplex
}  // namespace hws
