// THE TAPE (docs/MANUAL.md chapter 21, docs/UX.md 10.11, docs/ARCHITECTURE.md "The
// tape"): four mono audio tracks beside the patterns, mixed into the master bus with
// the instruments, rolling with the transport and locked to the sequencer's bars.
//
// POSITION. The tape is addressed in samples at the engine's rate. Its grid is the
// sequencer's: at every BAR LINE (the playing pattern's master step 1, 17, 33, 49,
// engine.h OUTPUT TAP) the true tape address is set to where that bar starts on the
// tape, the steps played so far times the length of a step at the TAPE'S TEMPO (the
// tempo of its first take; Q32 samples, as the sequencer counts), and between bar
// lines it advances one sample per frame. So a bar line on tape lands on the frame
// the sequencer fires it, every bar, whatever the phase of the quantum grid, and a
// tempo change after recording neither stretches the audio nor lets it drift: each
// bar plays from its own start (the tail cut when the tempo is faster, the next bar's
// start read on when slower). Pattern mode: the tape loops over loopBars bar lines
// from the bar the transport started on (tape address 0). Song mode: the address is
// the song position (the steps of the slots before the playing one), no loop.
// Where a bar line moves the address (by up to a quantum at the tape's own tempo,
// more at another tempo) playback crossfades from the old read position to the new
// one (64 frames, 256 when the jump is larger): never a click.
//
// STORAGE. Sparse: a pool of fixed blocks of kBlock samples (int16 at the engine's
// rate) in the platform's memory (init(): one pointer and a size; the tape's tables
// are carved from it too, the core allocates nothing). Each track has a table with one
// entry per block of its address space: a BASE block and a TAKE block (16 bits each,
// kNoBlock = none; one 32-bit atomic, so the audio thread reads both at once). Silence
// costs nothing. A take writes into take blocks only (zeroed blocks the main thread
// keeps ready for the audio thread); what is heard is guard(base + take). After the
// take the main thread merges each take block into a new block, swaps it in as the
// base, and keeps the replaced base: that is the one-level UNDO (and redo: swapping
// back). The block layer is the only code that turns a block id into memory
// (blockPtr()); an SD-card backing in a later phase replaces it with a cache the main
// thread fills ahead of the playhead (docs/STATE.md "TAPE PHASE 1").
//
// RECORDING. One track is armed, from one source: TRACK (an instrument track's dry
// signal as heard: after its level, pan, mute and ducking, before the send effects
// and the master bus), MIX (the master bus input: every instrument track, the send
// effects' return and the other tape tracks; not the armed tape track, whose sends
// are held at 0 while it records MIX so it cannot feed itself back), LINE (the
// platform's audio input through lineIn(), shifted back by the platform's round-trip
// latency). A take starts on a bar line (from stop: a one-bar count-in, then the
// transport starts from the top with the take) and ends on the bar line after
// RecordEnd, at Stop, at the end of the tape or when the memory runs out (a clean
// end: what was recorded stays). Overdub sums; REPLACE writes (input - base) into the
// take, so base + take is the input. Both are guarded: the sum is soft-limited above
// 75 % of full scale and never wraps.
//
// TRICKS (held keys, playback only): BRAKE slows the tape to a stop over 0.6 s with
// falling pitch and spins it back up over up to 0.25 s on release, HALF plays at half
// speed (an octave down), REVERSE plays backwards from the press. The read head is an
// offset from the true address, which keeps advancing underneath: on release the
// head comes back to it exactly (BRAKE's spin-up starts ahead of it so that it lands
// on it), so a trick never leaves the tape out of sync. Every change of head is a
// crossfade.
//
// THREADS. Audio thread: the Engine's calls below (from its quantum). Commands from
// the UI go through the engine's queue (CmdType::Tape, tapeCommand()). Main thread:
// service() every UI frame (merges, recycles and zeroes blocks), undo / erase / clear,
// read / write for the files and the lift, and the view getters (atomics the audio
// thread publishes). A block the main thread takes out of use is recycled only after
// the audio thread has finished two more quanta. No locks, no allocation.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "../config.h"
#include "../engine.h"

namespace pg {
namespace tape {

constexpr int kTracks = 4;
constexpr int kBlockShift = 14;
constexpr uint32_t kBlock = 1u << kBlockShift;          // samples per block: 16,384 (32 KB, 0.34 s at 48 kHz)
constexpr int kPeakShift = 8;                           // the overview: one peak byte per 256 samples
constexpr int kPeaksPerBlock = int(kBlock >> kPeakShift);
constexpr uint16_t kNoBlock = 0xFFFF;
constexpr uint32_t kMaxBlocks = 0xFFF0;
constexpr float kHeadroom = 2.0f;                       // int16 full scale is 2.0 on the bus (+6 dB)
constexpr int kNumLoops = 6;
constexpr uint8_t kLoopBars[kNumLoops] = {1, 2, 4, 8, 16, 32};
constexpr uint8_t kDefaultLoop = 4;
constexpr int kReady = 64;            // zeroed blocks waiting for the audio thread
constexpr uint32_t kLineRing = 16384; // line-in ring, samples
constexpr int kHist = 1024;           // quanta of address history (LINE's latency: 0.34 s at 48 kHz)
constexpr int kXfadeShort = 64, kXfadeLong = 256;
constexpr float kBrakeSec = 0.6f, kSpinSec = 0.25f;

enum Source : uint8_t { kSrcTrack = 0, kSrcMix, kSrcLine, kNumSources };
enum Trick : uint8_t { kTrickNone = 0, kTrickBrake, kTrickHalf, kTrickReverse, kNumTricks };
enum Param : uint8_t { kPLevel = 0, kPPan, kPDelay, kPReverb, kPMute, kNumParams };
constexpr uint8_t kParamDefault[kNumParams] = {100, 64, 0, 0, 0};
constexpr uint8_t kParamMax[kNumParams] = {127, 127, 127, 127, 1};

enum class Rec : uint8_t {
  Idle = 0,
  CountIn,   // stopped when armed: one bar, then Play from the top
  Waiting,   // armed, the transport runs: starts at the next bar line
  Rec,       // recording
  Ending,    // RecordEnd seen: recording to the next bar line
  Done,      // ended: the main thread merges it (service())
};
enum EndWhy : uint8_t { kEndNone = 0, kEndBar, kEndStop, kEndFull, kEndTapeEnd, kEndCancel };
const char* endWhyName(EndWhy w);   // "", "BAR", "STOP", "MEMORY FULL", "TAPE END", "CANCELLED"

// Commands (Engine CmdType::Tape; a = op).
enum Op : uint8_t {
  kOpArm = 0,   // b = tape track, c = source | kArmReplace, step.note = the instrument track (TRACK)
  kOpEnd,       // end the take on the next bar line (a take that has not started is cancelled)
  kOpCancel,    // end it now (a project is about to load)
  kOpParam,     // b = tape track, c = value, step.note = Param
  kOpLoop,      // c = loop length in bars (kLoopBars)
  kOpTrick,     // c = Trick (kTrickNone = released)
  kOpLatency,   // LINE's round trip in frames: step.note | step.vel << 8 | step.gate << 16
};
constexpr uint8_t kArmReplace = 0x80;
Command tapeCommand(Op op, int b = 0, int c = 0, int note = 0);

// What the audio thread publishes (Tape::view()).
struct View {
  Rec rec = Rec::Idle;
  uint8_t armed = 0, source = kSrcMix, srcTrack = 0;
  bool replace = false;
  bool rolling = false, song = false;
  uint8_t trick = kTrickNone;
  uint8_t countBeats = 0;   // count-in: beats to the start, 4 3 2 1
  EndWhy why = kEndNone;    // the last take's end
  uint32_t addr = 0;        // the true tape address (samples)
  uint32_t head = 0;        // where the read head is (a trick moves it)
  uint32_t takeStart = 0;   // the take's first address
  uint32_t recorded = 0;    // samples recorded in this take (all passes)
  uint32_t loopLen = 0;     // the loop's length on tape (samples): measured at the last wrap, else nominal
  uint16_t bpm = 0;         // the tape's tempo (0 = nothing recorded yet)
};

class Tape {
 public:
  // ---- setup (main thread, before the engine uses it) ----
  // mem/bytes: the platform's memory (aligned to 8; tables, the overview's peaks and
  // the blocks); rate: the engine's sample rate. Everything empty. false: too small.
  bool init(void* mem, size_t bytes, uint32_t rate);
  // The memory a tape of `seconds` of track time (all tracks together) needs at `rate`.
  static size_t bytesFor(uint32_t seconds, uint32_t rate);
  bool ready() const { return blocks_ != 0; }
  uint32_t rate() const { return rate_; }
  uint32_t blocks() const { return blocks_; }
  // samples one track can address (the tape's nominal length)
  uint32_t trackCapacity() const { return idx_ * kBlock; }

  // ---- audio thread (the Engine) ----
  void apply(const Command& c);
  void setTempo(int bpm, uint64_t stepFP);   // the project's tempo and step (Q32 samples)
  // Play: from the top (songSteps = the song position in steps; pattern mode 0) or
  // continue (cont: keep the position).
  void start(bool song, uint32_t songSteps, bool cont);
  void stop();
  // Every master step the sequencer fires: bar = a bar line (master step % 16 == 0);
  // stepsInBar = the steps from it to the next bar line; songSteps >= 0 at a song
  // pattern boundary: the song position in steps (resync, song mode).
  // gridFP: where the step's exact grid time is from this quantum's first frame (Q32
  // samples, the sequencer's gridCd: -half a quantum .. +half a quantum).
  void masterStep(bool bar, int stepsInBar, int32_t songSteps, int64_t gridFP);
  // The count-in: true in the quantum it ends (the engine then plays from the top).
  bool countInDone();
  bool active() const { return active_; }   // anything to do in this quantum
  // The instrument track whose dry signal is being recorded (TRACK), -1 = none.
  int captureTrack() const { return capTrack_; }
  void capture(const float* dL, const float* dR, bool mono);   // that track's contribution
  // Playback into the bus and the sends, before the send effects.
  void mix(float* busL, float* busR, float* sendD, float* sendR, bool mono, bool* dSend, bool* rSend);
  // After the send effects' return: MIX capture, the armed track's dry, the write.
  void post(float* busL, float* busR, bool mono);
  void endQuantum();   // every quantum (also when not active): the clock blocks are recycled by

  // ---- the platform's audio input (LINE): one writer thread ----
  void lineIn(const int16_t* mono, int n);
  bool lineWanted() const { return lineOpen_.load(std::memory_order_relaxed); }
  uint32_t lineUnderruns() const { return lineUnder_.load(std::memory_order_relaxed); }

  // ---- main thread ----
  void service();
  bool idle() const;                 // no take armed or running, nothing to merge
  bool canRecord() const;            // idle and blocks are ready for the audio thread
  void prepareTake();                // before posting kOpArm: the old undo goes
  bool canUndo() const { return undoN_ > 0 || undoBpmOnly_; }
  bool undoIsRedo() const { return undone_; }
  bool undo();                       // undo / redo the last take or erase
  bool erase(int track, uint32_t from, uint32_t to);   // [from, to) samples; undoable
  void clear();                      // empty (a new project, before a load); not undoable
  uint32_t freeBlocks() const;
  uint32_t freeSamples() const { return freeBlocks() * kBlock; }
  bool empty() const;
  bool trackEmpty(int t) const;
  uint32_t trackEnd(int t) const;    // the end of its last block in use (samples)
  // n samples of track t from pos (silence where it has none); returns n.
  uint32_t read(int t, uint32_t pos, int16_t* out, uint32_t n) const;
  // Loading: writes into an empty region (blocks of all-zero samples are left out).
  bool write(int t, uint32_t pos, const int16_t* in, uint32_t n);
  // The overview: the largest peak (0..255) of track t over [pos, pos + n).
  uint8_t peak(int t, uint32_t pos, uint32_t n) const;
  uint32_t revision() const { return rev_.load(std::memory_order_relaxed) + aRev_.load(std::memory_order_relaxed); }
  View view() const;
  uint8_t param(int t, int p) const { return set_[t & 3][p % kNumParams].load(std::memory_order_relaxed); }
  uint8_t loopBars() const { return loopBars_.load(std::memory_order_relaxed); }
  uint16_t bpm() const { return bpm_.load(std::memory_order_relaxed); }
  // loading (main thread, the transport stopped): the settings straight in
  void setBpm(uint16_t b) { bpm_.store(b, std::memory_order_relaxed); }
  void setParam(int t, int p, uint8_t v) {
    if (t >= 0 && t < kTracks && p >= 0 && p < kNumParams) set_[t][p].store(v, std::memory_order_relaxed);
  }
  void setLoop(uint8_t bars) {
    for (int i = 0; i < kNumLoops; ++i)
      if (kLoopBars[i] == bars) loopBars_.store(bars, std::memory_order_relaxed);
  }
  // A fingerprint of everything a save writes (the audio's revision and the settings):
  // the UI compares it to see unsaved changes.
  uint32_t signature() const;
  // the nominal loop length at the tape's tempo (16-step bars), samples
  uint32_t nominalLoop(int bars, uint64_t projectStepFP) const;

  // tests
  uint32_t mergesDone() const { return merges_; }
  uint32_t quarantined() const { return qN_; }

 private:
  struct Head {
    int64_t off = 0;    // Q32 samples from the true address
    int64_t vel = 0;    // Q32 per frame (0: the tape's own speed)
    int64_t acc = 0;    // Q32 per frame^2
    int32_t accLeft = 0;
    bool identity() const { return off == 0 && vel == 0 && acc == 0; }
  };
  struct Hist { int64_t addr; int64_t limit; uint8_t rec; };

  int16_t* blockPtr(uint16_t id) const { return blockMem_ + size_t(id) * kBlock; }
  uint8_t* peaksOf(uint16_t id) const { return peaks_ + size_t(id) * kPeaksPerBlock; }
  std::atomic<uint32_t>& entry(int t, uint32_t i) const { return table_[size_t(t) * idx_ + i]; }
  // audio thread
  void beginTake();
  void finish(EndWhy why);
  void runHeads(int t, float* out);
  void readFast(int t, int64_t a, float* out) const;
  float sampleAt(int t, int64_t idx) const;
  void writeQuantum(const float* src, int64_t addr, int64_t limit);
  bool writeAt(uint32_t a, const int16_t* v, int n);
  int64_t wrap(int64_t a) const {   // pattern mode: the loop is circular
    if (song_ || loopLen_ == 0) return a;
    const int64_t L = loopLen_;
    a %= L;
    return a < 0 ? a + L : a;
  }
  int64_t addrNow() const { return (addrFP_ + (int64_t(1) << 31)) >> 32; }
  void jump(int64_t j);
  void startTrick(uint8_t want);
  void recomputeGains(int t, bool mono);
  void prepareHeads();
  void updateActive();
  // main thread
  uint16_t allocMain();
  void release(uint16_t id);
  void mergeSome(int budget);
  void publishUndo();

  uint32_t rate_ = 0;
  uint32_t blocks_ = 0, idx_ = 0;
  std::atomic<uint32_t>* table_ = nullptr;
  uint8_t* peaks_ = nullptr;
  int16_t* blockMem_ = nullptr;
  uint16_t* free_ = nullptr;
  uint32_t freeN_ = 0;
  uint16_t* qId_ = nullptr;
  uint32_t* qStamp_ = nullptr;
  uint32_t qN_ = 0;
  uint32_t* touched_ = nullptr;      // take: (track << 16) | index
  uint32_t* undoKey_ = nullptr;      // (track << 16) | index
  uint16_t* undoBlk_ = nullptr;      // the block an undo swaps in
  uint32_t undoN_ = 0;
  bool undone_ = false, undoBpmOnly_ = false;
  uint16_t undoBpm_ = 0, undoBpmSaved_ = 0;
  uint32_t mergeAt_ = 0, mergeN_ = 0; // touched_[mergeAt_..mergeN_) are still to merge
  bool merging_ = false, wasEmpty_ = false;
  std::atomic<uint32_t> takeSeq_{0}, mergedSeq_{0};   // audio: takes ended; main: takes merged
  uint32_t merges_ = 0;
  std::atomic<uint32_t> rev_{0};     // main thread's changes

  // the ready ring (main -> audio)
  uint16_t ready_[kReady] = {};
  std::atomic<uint32_t> rHead_{0}, rTail_{0};

  // the line-in ring (platform thread -> audio)
  int16_t* line_ = nullptr;
  std::atomic<uint32_t> lHead_{0}, lTail_{0}, lineUnder_{0};
  std::atomic<bool> lineOpen_{false};
  bool linePrimed_ = false;
  uint32_t lineTarget_ = 0;          // the ring's fill the audio thread keeps (frames)
  Hist* hist_ = nullptr;
  uint32_t latencyQ_ = 0;            // LINE: quanta between hearing and the input arriving

  // settings (written by the audio thread from commands; read anywhere)
  std::atomic<uint8_t> set_[kTracks][kNumParams];
  std::atomic<uint8_t> loopBars_{kDefaultLoop};
  std::atomic<uint16_t> bpm_{0};

  // audio thread
  bool active_ = false;
  bool rolling_ = false, posValid_ = false, song_ = false, first_ = false;
  int64_t addrFP_ = 0;               // the true address of this quantum's first frame (Q32; unwrapped)
  int64_t limit_ = 0;                 // the end of the bar on tape (samples, unwrapped)
  uint64_t barFP_ = 0;
  uint32_t barIdx_ = 0;
  int stepsInBar_ = 0;
  uint32_t lastLoop_ = 0;
  int projBpm_ = 120;
  uint64_t projStepFP_ = 0, stepFP_ = 0;
  int stopFade_ = 0;                  // quanta of fade-out after Stop
  int32_t countIn_ = 0;               // frames left
  bool countEnd_ = false;
  Rec rec_ = Rec::Idle;
  uint8_t armT_ = 0, src_ = kSrcMix, srcT_ = 0;
  bool replace_ = false;
  int capTrack_ = -1;
  uint32_t takeBars_ = 0;             // replace, loop mode: the loop's bars written in this take
  uint32_t takeStart_ = 0, recorded_ = 0, touchedN_ = 0;
  int drainQ_ = 0;                    // LINE: quanta still to write after the end
  EndWhy why_ = kEndNone;
  uint32_t histQ_ = 0;
  float cap_[kQuantum] = {};          // the source this quantum (bus units)
  bool capHave_ = false;
  float out_[kTracks][kQuantum] = {};
  std::atomic<bool> trackHas_[kTracks];   // the track holds audio (the audio thread sets it in a take, the main thread when idle)
  // mixer
  float gL_[kTracks] = {}, gR_[kTracks] = {}, tL_[kTracks] = {}, tR_[kTracks] = {};
  float sD_[kTracks] = {}, sR_[kTracks] = {}, tD_[kTracks] = {}, tRv_[kTracks] = {};
  float gainK_ = 0.1f;
  float tC_[kTracks] = {};            // the centred (mono) gain
  uint8_t cache_[kTracks][kNumParams] = {};   // the settings the targets were computed from
  bool cacheMono_ = false, gainsInit_ = false, wasActive_ = false;
  uint8_t prevBar_ = 0;
  bool takeLive_ = false;             // this quantum belongs to the take (TRACK / MIX; LINE follows hist)
  uint32_t loopLen_ = 0;              // the loop on tape (pattern mode): wraps the trick head
  // the heads this quantum: positions (Q32), crossfade weight of cur, rate gains
  int64_t pc_[kQuantum] = {}, po_[kQuantum] = {};
  float wc_[kQuantum] = {}, gc_[kQuantum] = {}, go_[kQuantum] = {};
  bool fast_ = true;
  float fadeGain_ = 1.0f;
  // heads
  Head cur_, old_;
  int xf_ = 0, xfLen_ = 0;
  bool punch_ = false;                // a bar line's jump: the new head at full level, the old fades out
  float ov_[kQuantum] = {};           // past the bar's end on tape (a slower tempo): the fade to silence
  bool ovOn_ = false;
  uint8_t trick_ = kTrickNone, wantTrick_ = kTrickNone;
  int32_t brakeFrames_ = 1, spinFrames_ = 1;

  // published
  std::atomic<uint8_t> aRec_{0}, aArmed_{0}, aSrc_{kSrcMix}, aSrcT_{0}, aTrick_{0}, aBeats_{0}, aWhy_{0};
  std::atomic<bool> aRolling_{false}, aSong_{false}, aReplace_{false};
  std::atomic<uint32_t> aAddr_{0}, aHead_{0}, aTakeStart_{0}, aRecorded_{0}, aLoop_{0}, aQ_{0}, aTouched_{0};
  std::atomic<uint32_t> aRev_{0};
};

}  // namespace tape
}  // namespace pg
