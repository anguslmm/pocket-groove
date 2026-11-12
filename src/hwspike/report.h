// Pocket Groove hardware spike -- the two serial report blocks. Portable
// (snprintf only), unit-tested on the host (host/hwspike).
//
// Format: one "key: field=value ..." line per topic between fixed BEGIN / END
// markers, like the profiler's. Values that were not measured print as "na".
// Every line is at most kReportLineMax characters.
#pragma once
#include <cstddef>
#include <cstdint>

#include "tone.h"

namespace hws {

constexpr int kReportLineMax = 200;
constexpr const char* kReportVersion = "hwspike-2";

using LineSink = void (*)(const char* line, void* user);   // line without '\n'

// ---- TILT --------------------------------------------------------------------
struct KeyLatency {
  uint32_t n = 0;
  float    avgUs = 0, maxUs = 0;
};

struct TiltReport {
  // IMU identity and the registers read back after configuration
  uint8_t  addr = 0, chipId = 0, errReg = 0;
  uint8_t  accConf = 0, accRange = 0, gyrConf = 0, gyrRange = 0, pwrConf = 0, pwrCtrl = 0;
  bool     imuOk = false;
  float    accLsbPerG = 0, gyrLsbPerDps = 0;
  // rates
  uint32_t setHz = 0;
  float    pollHz = 0, newHz = 0, dupPct = 0, odrSensortimeHz = 0, ticksPerSample = 0;
  uint32_t gaps = 0;                 // new samples whose sensortime step was > 1.5 periods
  // bus
  float    i2cAvgUs = 0, i2cMaxUs = 0, i2c1ByteUs = 0;
  uint32_t i2cErrors = 0;
  // CPU (core 1 unless noted)
  float    taskUsPerPoll = 0, mathCyclesPerSample = 0, imuCorePct = 0;
  float    audioAvgUs = 0, audioMaxUs = 0, audioCpuPct = 0;   // audio task, core 0
  // noise at rest (std over 2 s windows: quietest, degrees) and offsets
  float    noiseAccA = -1, noiseAccB = -1, noiseFusedA = -1, noiseFusedB = -1;
  float    gyroNoiseDps = -1;
  float    biasDps[3] = {0, 0, 0};
  bool     biasCalibrated = false;
  float    accMeanG[3] = {0, 0, 0};
  float    accMeanNormG = 0;
  // drift test
  float    driftSecs = 0, driftGyroDeg = -1, driftFusedDeg = -1;
  // gyro sign check: +1 = gyro and accelerometer agree, -1 = flipped, 0 = no motion seen
  float    signCheck = 0;
  float    gyroSign = 1;
  // latency estimate components, ms
  float    latReadAgeMs = 0, latBlockMs = 0, latDmaMs = 0;
  float    latSmoothMs[4] = {0, 0, 0, 0};
  // keyboard impact
  KeyLatency keysImuOff, keysImuOn;
  uint32_t keyDrops = 0, keyFifoOvf = 0, keyI2cErr = 0, keyUnknown = 0, keyDowns = 0, keyUps = 0;
  uint32_t keyMaxIntToReadUs = 0;
  // play
  int      preset = 0;
  uint32_t shakes = 0;
  float    lastShakeG = 0;
  // memory
  uint32_t heapFree = 0, heapMin = 0;
  uint32_t uptimeS = 0;
};

// ---- SAMPLER -----------------------------------------------------------------
struct GainRow {
  bool     used = false;
  uint8_t  reg14 = 0, reg16 = 0;
  int      pgaDb = 0;
  // silence measurement ('n')
  bool     silValid = false;
  float    silAcRmsDb = 0, silPeakDb = 0, silDcLsb = 0;
  uint32_t silClips = 0;
  bool     silStuck = false;
  // last recording made with this gain
  bool     recValid = false;
  float    recPeakDb = 0, recRmsDb = 0, recDcLsb = 0, recSecs = 0;
  uint32_t recClips = 0;
};

struct BenchRow {
  const char* src = "";       // "ram48k", "flash48k", "flash1m"
  int      voices = 0;
  int      pollKb = 0;        // data-cache polluter per 128-frame block
  float    cycPerSample = 0;  // per OUTPUT sample, all voices
  float    cycPerVoiceSample = 0;
  uint32_t maxBlockCyc = 0;
  float    corePct = 0;       // of one core at the sample rate
};

constexpr int kNumGainRows = 4;
constexpr int kMaxBenchRows = 40;

// ---- microphone quality (SAMPLER key n, mic_quality.cpp) ------------------------
// 2 s of room silence per gain preset, then 1 s of the 1 kHz speaker tone at
// the current preset. No stimulus timing needed: hands off for ~12 s.
struct NoiseRow {
  bool     valid = false;
  int      pgaDb = 0;
  uint8_t  reg14 = 0, reg16 = 0;
  float    rmsDb = -120;      // noise floor: RMS with DC removed, dBFS
  float    peakDb = -120, dcLsb = 0;
  bool     stuck = false;
  uint32_t clips = 0;
  // averaged spectrum (Hann): the largest line at or above 40 Hz and the median bin
  bool     spurValid = false;
  float    spurHz = 0, spurDb = -120, floorDb = -120, spurOverFloorDb = 0;
  uint32_t frames = 0;        // FFT frames averaged
};

struct MicQuality {
  bool     valid = false;
  int      slot = 0;          // I2S slot measured (0 L, 1 R)
  int      gain = 0;          // preset of the tone measurement
  uint32_t rate = 0;
  float    silSecs = 0;
  int      fftN = 0;
  NoiseRow noise[kNumGainRows];
  bool     toneValid = false;
  uint8_t  vol = 0;           // DAC volume register during the tone
  float    stimDb = 0;        // digital level of the tone sent to the DAC, dBFS
  uint32_t toneHz = 0, toneN = 0;
  HarmonicResult tone;
  float    secs = 0;          // whole run
};

struct SamplerReport {
  // storage
  bool     storeOk = false;
  const char* storeWhy = "";
  uint32_t partAddr = 0, partSize = 0, fsAddr = 0, fsSize = 0, runningAddr = 0;
  uint32_t recRegionOff = 0, recRegionSize = 0, benchRegionOff = 0, benchRegionSize = 0;
  bool     benchReady = false;
  // codec read-back
  uint8_t  regs[10] = {0};    // 0x01 0x09 0x0A 0x0D 0x0E 0x14 0x15 0x16 0x17 0x1C
  bool     regsValid = false;
  uint32_t rate = 0;
  int      channel = 0;       // 0 = left slot, 1 = right slot
  // live input, last full second per slot
  float    liveRmsDb[2] = {-120, -120}, livePeakDb[2] = {-120, -120}, liveDc[2] = {0, 0};
  bool     liveStuck[2] = {false, false};
  int      gain = 0;
  GainRow  gains[kNumGainRows];
  // last recording
  bool     recValid = false;
  uint32_t recSamples = 0;
  float    recSecs = 0;
  uint32_t recChunk = 0, recWrites = 0;
  float    recWriteAvgUs = 0, recWriteMaxUs = 0;
  uint32_t recRxOverflows = 0, recRingDrops = 0, recBlocks = 0;
  float    recMaxRxGapUs = 0, recEraseMs = 0, recBlockPeriodUs = 0;
  // playback cost
  int      benchRows = 0;
  BenchRow bench[kMaxBenchRows];
  float    missCycPerLine = 0;      // polluter: cycles per data-cache line from flash
  float    playAvgUs = 0, playMaxUs = 0, playCpuPct = 0;
  // microphone quality (key n)
  MicQuality quality;
  // memory
  uint32_t heapFree = 0, heapMin = 0, heapLargest = 0, staticDram = 0;
  uint32_t uptimeS = 0;
};

// ---- MICMATRIX (sampler page key m, mic_matrix.cpp) ----------------------------
// GPIO46 (I2S data in) as the registers say: output enable / level / output
// signal, input enable, pulls, IO_MUX function, pad level, and which GPIO the
// I2S1 data-in signal is routed from (in_sel; 46 expected).
struct PinSnap {
  bool     valid = false;
  uint8_t  oe = 0, out = 0, ie = 0, wpu = 0, wpd = 0, mcuSel = 0, level = 0, inSel = 0;
  uint16_t outSel = 0;
};

constexpr int kMaxMicCfgs = 36;
constexpr int kMaxRegDiffs = 40;
constexpr int kNumCodecRegs = 0x46;   // 0x00 .. 0x45

struct MicCfgRow {
  const char* name = "";
  int      slotBits = 16;
  int      din = 0;            // 0 as found, 1 output driver off (fix), 2 forced high (repro), 3 off + pull-down
  bool     tone = true;        // 1 kHz stimulus played on the DAC / speaker during the capture
  bool     loopbackR = false;  // reg 0x44 routes the DAC to the R slot: judged on L only
  bool     m5mic = false;      // M5.Mic control (our driver shut down)
  bool     ran = false;
  uint32_t rate = 0;
  uint8_t  r01 = 0, r02 = 0, r0A = 0, r14 = 0, r44 = 0;
  bool     regsValid = false;
  ToneResult res[2];           // L, R (M5.Mic: both = its mono stream)
  MicVerdict v[2];
  bool     micOk[2] = {true, true};   // slot may be ranked as the microphone (not the DAC reference, see micSlots())
  // registers (0x00-0x1C, 0x31-0x37, 0x44, 0x45; not 0x32) that differ from
  // the run's baseline after the capture
  bool     diffValid = false;
  int      nDiff = 0;
  uint8_t  diffReg[kMaxRegDiffs] = {0};
  uint8_t  diffVal[kMaxRegDiffs] = {0};
  int      bestSlot = 0;
  float    score = -1.0e9f;
};

struct MicMatrixReport {
  PinSnap  pinBoot, pinAfterI2s;
  bool     pinTestValid = false;
  float    pinOnes[5] = {-1, -1, -1, -1, -1};   // as found, pull-up, pull-down, SDP muted + pull-up, muted + pull-down
  const char* pinVerdict = "na";
  bool     chipValid = false;
  uint8_t  chip[3] = {0, 0, 0};                 // regs 0xFD 0xFE 0xFF
  bool     regsBeforeValid = false, regsAfterValid = false;
  uint8_t  regsBefore[kNumCodecRegs] = {0};     // = the baseline every config starts from
  uint8_t  regsAfter[kNumCodecRegs] = {0};
  int      n = 0;
  MicCfgRow cfg[kMaxMicCfgs];
  int      rank[kMaxMicCfgs] = {0};             // config indices, best first
  int      best = -1;                           // -1: no config works
  bool     applied = false;                     // the sampler now runs the best config
  float    secs = 0;
};

// The one-line verdict the page shows: "MIC WORKS (config N)" or "MIC DEAD IN ALL N CONFIGS".
void micMatrixVerdict(const MicMatrixReport& r, char* out, size_t n);
// Fills r.rank (by score, stable) and r.best (first whose best slot is a
// microphone slot that is alive, else -1).
void rankMicMatrix(MicMatrixReport& r);

// Each emits the whole block (BEGIN ... END) through sink, line by line.
// Returns the number of lines written.
int formatTiltReport(const TiltReport& r, LineSink sink, void* user);
int formatSamplerReport(const SamplerReport& r, LineSink sink, void* user);
int formatMicMatrixReport(const MicMatrixReport& r, LineSink sink, void* user);

// Picks row.bestSlot among the microphone slots (row.micOk) and sets row.score;
// a row with no microphone slot scores below every dead one.
void scoreMicRow(MicCfgRow& row);

}  // namespace hws
