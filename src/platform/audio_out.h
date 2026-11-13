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
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace plat {
namespace audio {

// Fills `frames` stereo frames (L,R,L,R...) of int16. `frames` is always
// kBlockFrames (128 = 8 x pg::kQuantum).
using RenderFn = void (*)(int16_t* interleavedStereo, int frames, void* user);

constexpr int kBlockFrames = 128;
constexpr unsigned kWarmupBlocks = 32;   // blocks after a start / statistics reset left out of the load figures (93 ms)
constexpr uint32_t kTaskStackBytes = 6144;   // the audio task's stack

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
  float    maxUs;           // longest render since begin()/resetStats() (max-ever)
  float    recentMaxUs;     // longest render over the last ~1-2 s (rolling)
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
// Longest render time since the previous takeMaxUs() call (first call: since
// begin), and resets it. For a periodic stats line; one caller only.
float takeMaxUs();
// The report's own window (independent of takeMaxUs): since the previous call,
// the highest AUD figure (the 64-block moving average, % of one core), the
// longest single block, and the blocks / underruns / late blocks in it. So
// "do the thing, then print the report" reads the peak of doing the thing.
struct Window {
  float    peakCpuPercent;
  float    peakBlockUs;
  uint32_t blocks, underruns, lateBlocks;
};
Window takeWindow();

// Profiling: called on the audio task after every block (keep it short, no
// blocking). Cycle-counter values of the audio core. nullptr removes it.
struct BlockInfo {
  uint32_t startCyc;    // cycle count when the render started
  uint32_t renderCyc;   // render callback duration
  uint32_t writeCyc;    // time spent handing the block to the driver (blocked = headroom)
  uint32_t gapCyc;      // start-to-start distance from the previous block (0 for the first)
  uint32_t periodCyc;   // nominal block period
};
void setBlockObserver(void (*fn)(const BlockInfo&, void*), void* user);

// "[audio] hot path: IRAM|FLASH ..." -- where Engine::process and
// LeadVoice::render really are in this image, IRAM text size, heap. Also
// printed at every begin().
int placementLine(char* buf, size_t cap);

// Worst-case time from the start of a render block to the DAC, in frames
// (render block + driver queue + DMA), for the current config.
uint32_t latencyFrames();

// ---- MICROPHONE (stage F2; docs/hardware-notes.md 11.6, 11.7; docs/STATE.md) ----
// Full duplex on the one I2S port, as the hw-spike recorded: the RX channel is
// registered together with TX at begin() (its channel object only: a few hundred
// bytes of heap) and initialised the first time micBegin() runs (its DMA ring, the
// same 4 x 128 frames as TX, ~2 KB of heap, then stays until end()). The first
// micBegin() also writes the codec's ADC registers (the set mic-matrix config 6
// recorded with on the device, MINUS everything that touches the DAC side: no reset
// pulse, no analog power cycle, no clock or divider rewrite; so the playing output
// keeps its level and does not click) and the ADC stays powered after that.
// Every micBegin() switches GPIO46's output driver off (M5.begin() leaves it
// driven high, which the RX samples as a constant -1).
//
// While a MicRing is attached the audio task, right after each TX write, reads the
// RX blocks that are ready (non-blocking) and copies their LEFT slot into the ring.
// That is the microphone's whole cost on the audio thread; the consumer (the writer
// task, src/platform/mic.*) does everything else. DirectI2S at 44.1 kHz only.
// UI thread only (I2C, the attach / detach handshake).
struct MicRing {
  int16_t* buf = nullptr;                  // mono, 44.1 kHz; size a power of two
  uint32_t mask = 0;                       // size - 1
  std::atomic<uint32_t> head{0};           // written by the audio task only
  std::atomic<uint32_t> tail{0};           // written by the consumer only
  std::atomic<uint32_t> dropped{0};        // frames that found the ring full (audio task)
};
bool micBegin(MicRing* ring);   // false: lastError() says why (the audio keeps running)
void micEnd();                  // detaches the ring (after the audio task let go) and stops RX
bool micActive();
bool micAvailable(const char** why);   // could micBegin() work now (backend, rate, RX registered)
struct MicStats {
  bool     rxRegistered, rxInitialised, adcOn, attached;
  uint32_t blocks;          // RX reads that returned data while attached
  uint32_t frames;          // frames handed to the ring
  uint32_t rxOverflows;     // RX DMA buffers overwritten before they were read (input lost)
  uint32_t readErrors;
  float    maxPullUs;       // the longest RX read + copy on the audio thread
  float    maxBlockUs;      // the longest render while attached
  float    maxGapUs;        // the longest start-to-start gap between two blocks while attached
};
MicStats micStats();
void micResetStats();           // the per-recording maxima (call before a micBegin)
// The ADC registers micBegin() writes, as (register, value) pairs.
const uint8_t (*micAdcRegs(int* n))[2];
// The DAC output faded to silence and back (UI thread: I2C, ~10-20 ms each), around a
// flash erase that stalls the audio task.
void fadeOut();
void fadeIn();
// GPIO46 (I2S data in) as the registers have it now.
struct PinState { uint8_t oe, out, level, ie, inSel; uint16_t outSel; };
PinState micPin();

}  // namespace audio
}  // namespace plat
