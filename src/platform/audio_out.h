// Pocket Groove platform layer -- audio output for the Cardputer-Adv.
//
// Signal path:   render callback -> int16 stereo block (128 frames) -> I2S1 -> ES8311 DAC
//                                                         BCLK 41 / WS 43 / DOUT 42, no MCLK
// Two backends behind one interface:
//   DirectI2S  (default) our own ES8311 init over I2C + ESP-IDF `i2s_std` driver.
//   M5Speaker  (fallback) M5.Speaker.playRaw() with rotating buffers; M5Unified
//              initialises the codec and owns the I2S port.
//
// Threads: begin/end/reconfigure/setVolumeReg/codec* are for the UI thread only
// (they talk I2C). The render callback runs on the dedicated audio task and
// must not block, allocate, print or touch I2C. stats() is safe from any thread.
//
// No Arduino / M5 / ESP-IDF types in this header.
#pragma once
#include <cstddef>
#include <cstdint>

namespace plat {
namespace audio {

// Fills `frames` stereo frames (L,R,L,R...) of int16. `frames` is always
// kBlockFrames (128 = 8 x pg::kQuantum).
using RenderFn = void (*)(int16_t* interleavedStereo, int frames, void* user);

constexpr int kBlockFrames = 128;

enum class Backend : uint8_t { DirectI2S = 0, M5Speaker = 1 };

struct Config {
  uint32_t sampleRate  = 44100;        // 44100 or 32000
  Backend  backend     = Backend::DirectI2S;
  uint8_t  core        = 0;            // core the audio task is pinned to (UI/loop runs on 1)
  uint8_t  dmaBufCount = 4;            // DirectI2S: DMA buffers of kBlockFrames frames (3..16)
  // ES8311 DAC volume, reg 0x32: 0x00=-95.5 dB, 0xBF=0 dB, 0xFF=+32 dB, 0.5 dB/step.
  // Boot default 0x80 = -31.5 dB: safe for earbuds (the codec drives the jack
  // directly, the firmware cannot tell whether headphones are in). Applied
  // with a short ramp at begin(), ramped to 0 at end().
  uint8_t  volumeReg   = 0x80;
};

struct Stats {
  uint32_t blocks;          // blocks rendered and handed to the driver since begin/resetStats
  uint32_t underruns;       // DirectI2S: DMA buffers that went out without fresh data
  bool     underrunsValid;  // false on the M5Speaker backend (M5Unified owns the driver)
  uint32_t lateBlocks;      // render alone took longer than one block period
  uint32_t writeErrors;     // driver write failed / timed out / was short
  uint32_t overloadYields;  // forced 2 ms sleeps while overloaded (keeps idle task + watchdog alive)
  float    lastUs;          // render callback time of the most recent block
  float    maxUs;
  float    avgUs;           // exponential moving average, time constant 64 blocks
  float    maxPeriodUs;     // longest time between two consecutive block starts
  float    blockPeriodUs;   // nominal: kBlockFrames / sampleRate
  float    cpuPercent;      // avgUs / blockPeriodUs * 100 (share of ONE core)
  uint32_t stackFreeMin;    // audio task stack bytes never used so far
  int      taskCore;        // core the audio task is really running on (-1 if stopped)
};

// Starts codec + driver + audio task. Returns false on failure (lastError()
// says why); the system keeps running without audio in that case.
bool begin(const Config& cfg, RenderFn render, void* user);
void end();
bool reconfigure(const Config& cfg);     // end() + begin() with the same callback
bool running();
const Config& config();
const char* lastError();                 // "" when fine

// Codec (UI thread only).
bool    codecPresent();                  // ES8311 answered on 0x18 at the last begin()
bool    setVolumeReg(uint8_t reg);       // writes ES8311 reg 0x32 now and remembers it
uint8_t volumeReg();
float   volumeDb();                      // (reg - 0xBF) * 0.5
bool    codecInit();                     // re-run the DAC init sequence (clock keeps running)
// Raw register write. A write to reg 0x09 (DAC serial format) is remembered
// and re-applied by every later init (begin, reconfigure, codecInit).
bool    codecWrite(uint8_t reg, uint8_t value);
bool    codecRead(uint8_t reg, uint8_t* value);

Stats stats();
void  resetStats();

// Worst-case time from the start of a render block to the DAC, in frames
// (render block + driver queue + DMA), for the current config.
uint32_t latencyFrames();

}  // namespace audio
}  // namespace plat
