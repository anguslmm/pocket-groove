// The tape (tape.h): storage, the bar-locked position, recording, tricks, mixing.
#include "tape.h"

#if PG_TAPE

#include <cmath>
#include <cstring>
#include <new>

namespace pg {
namespace tape {

namespace {

constexpr int64_t kOne = int64_t(1) << 32;   // one sample, Q32
constexpr uint32_t kNoEntry = 0xFFFFFFFFu;   // base and take both kNoBlock
constexpr uint32_t kReserve = 2;             // free blocks the merge keeps for itself
constexpr float kToTape = 32767.0f / kHeadroom;
constexpr float kFromTape = kHeadroom / 32767.0f;

inline uint16_t baseOf(uint32_t e) { return uint16_t(e & 0xFFFFu); }
inline uint16_t takeOf(uint32_t e) { return uint16_t(e >> 16); }
inline uint32_t mkEntry(uint16_t base, uint16_t take) { return uint32_t(base) | (uint32_t(take) << 16); }
inline int16_t sat16(int32_t x) { return int16_t(x > 32767 ? 32767 : (x < -32767 ? -32767 : x)); }
// The overdub guard: identity up to 75 % of full scale, then a soft knee toward full
// scale it never reaches (so a sum of two loud layers does not wrap or clip hard).
inline int32_t guardSum(int32_t x) {
  constexpr int32_t K = 24576, R = 32767 - K;
  const int32_t a = x < 0 ? -x : x;
  if (a <= K) return x;
  const int32_t d = a - K;
  const int32_t y = K + int32_t((int64_t(R) * d) / (R + d));
  return x < 0 ? -y : y;
}
inline int16_t toTape(float x) {
  const float v = x * kToTape;
  const float c = v > 32767.0f ? 32767.0f : (v < -32767.0f ? -32767.0f : v);
  return int16_t(c < 0.0f ? c - 0.5f : c + 0.5f);
}
inline uint8_t peakOf(int v) { return uint8_t((v < 0 ? -v : v) >> 7); }
inline size_t alignUp(size_t x, size_t a) { return (x + a - 1) & ~(a - 1); }
inline uint32_t roundFP(uint64_t fp) { return uint32_t((fp + (uint64_t(1) << 31)) >> 32); }
constexpr size_t kPerBlock = kBlock * 2 + kPeaksPerBlock + kTracks * 4 + 2 + 2 + 4 + 4 + 4 + 2;
constexpr size_t kFixed = kLineRing * 2 + kHist * 24 + 256;

}  // namespace

const char* endWhyName(EndWhy w) {
  switch (w) {
    case kEndBar: return "BAR";
    case kEndStop: return "STOP";
    case kEndFull: return "MEMORY FULL";
    case kEndTapeEnd: return "TAPE END";
    case kEndCancel: return "CANCELLED";
    default: return "";
  }
}

Command tapeCommand(Op op, int b, int c, int note) {
  Command x;
  std::memset(&x, 0, sizeof(x));
  x.type = CmdType::Tape;
  x.a = uint8_t(op);
  x.b = uint8_t(b);
  x.c = uint8_t(c);
  x.step.note = uint8_t(note);
  return x;
}

// ---------------------------------------------------------------- setup ----

size_t Tape::bytesFor(uint32_t seconds, uint32_t rate) {
  const uint64_t samples = uint64_t(seconds) * rate;
  uint64_t n = (samples + kBlock - 1) / kBlock;
  if (n > kMaxBlocks) n = kMaxBlocks;
  return kFixed + size_t(n) * kPerBlock + 64;
}

bool Tape::init(void* mem, size_t bytes, uint32_t rate) {
  blocks_ = 0;
  if (!mem || bytes < kFixed + kPerBlock + 64 || rate < 8000) return false;
  uint64_t n = (bytes - kFixed - 64) / kPerBlock;
  if (n > kMaxBlocks) n = kMaxBlocks;
  rate_ = rate;
  idx_ = uint32_t(n);
  uint8_t* p = static_cast<uint8_t*>(mem);
  size_t at = alignUp(reinterpret_cast<uintptr_t>(p), 16) - reinterpret_cast<uintptr_t>(p);
  auto take = [&](size_t sz, size_t al) {
    at = alignUp(at + reinterpret_cast<uintptr_t>(p), al) - reinterpret_cast<uintptr_t>(p);
    uint8_t* r = p + at;
    at += sz;
    return r;
  };
  blockMem_ = reinterpret_cast<int16_t*>(take(size_t(n) * kBlock * 2, 16));
  table_ = reinterpret_cast<std::atomic<uint32_t>*>(take(size_t(n) * kTracks * 4, 8));
  for (size_t i = 0; i < size_t(n) * kTracks; ++i) new (&table_[i]) std::atomic<uint32_t>(kNoEntry);
  peaks_ = take(size_t(n) * kPeaksPerBlock, 8);
  free_ = reinterpret_cast<uint16_t*>(take(size_t(n) * 2, 8));
  qId_ = reinterpret_cast<uint16_t*>(take(size_t(n) * 2, 8));
  qStamp_ = reinterpret_cast<uint32_t*>(take(size_t(n) * 4, 8));
  touched_ = reinterpret_cast<uint32_t*>(take(size_t(n) * 4, 8));
  undoKey_ = reinterpret_cast<uint32_t*>(take(size_t(n) * 4, 8));
  undoBlk_ = reinterpret_cast<uint16_t*>(take(size_t(n) * 2, 8));
  line_ = reinterpret_cast<int16_t*>(take(kLineRing * 2, 8));
  hist_ = reinterpret_cast<Hist*>(take(kHist * sizeof(Hist), 8));
  if (at > bytes) return false;
  std::memset(peaks_, 0, size_t(n) * kPeaksPerBlock);
  std::memset(line_, 0, kLineRing * 2);
  std::memset(hist_, 0, kHist * sizeof(Hist));
  for (uint32_t i = 0; i < n; ++i) free_[i] = uint16_t(n - 1 - i);
  freeN_ = uint32_t(n);
  qN_ = 0;
  undoN_ = 0;
  undone_ = undoBpmOnly_ = false;
  rHead_.store(0);
  rTail_.store(0);
  lHead_.store(0);
  lTail_.store(0);
  lineOpen_.store(false);
  for (int t = 0; t < kTracks; ++t) {
    for (int q = 0; q < kNumParams; ++q) set_[t][q].store(kParamDefault[q]);
    trackHas_[t].store(false);
  }
  loopBars_.store(kDefaultLoop);
  bpm_.store(0);
  const float qps = float(rate) / float(kQuantum);
  gainK_ = 1.0f - expf(-1.0f / (0.004f * qps));
  brakeFrames_ = int32_t(kBrakeSec * float(rate));
  spinFrames_ = int32_t(kSpinSec * float(rate));
  lineTarget_ = 0;
  latencyQ_ = 0;
  rolling_ = posValid_ = false;
  rec_ = Rec::Idle;
  why_ = kEndNone;
  merges_ = 0;
  merging_ = false;
  takeSeq_.store(0);
  mergedSeq_.store(0);
  touchedN_ = 0;
  histQ_ = 0;
  aRec_.store(0);
  aWhy_.store(0);
  aBeats_.store(0);
  aTouched_.store(0);
  aRecorded_.store(0);
  gainsInit_ = false;
  blocks_ = uint32_t(n);
  service();   // the first ready blocks
  return true;
}

// --------------------------------------------------------- audio thread ----

void Tape::apply(const Command& c) {
  switch (Op(c.a)) {
    case kOpArm: {
      if (rec_ != Rec::Idle) break;
      armT_ = uint8_t(c.b & 3);
      src_ = uint8_t(c.c & 0x7F) < kNumSources ? uint8_t(c.c & 0x7F) : uint8_t(kSrcMix);
      replace_ = (c.c & kArmReplace) != 0;
      srcT_ = uint8_t(c.step.note & 7);
      why_ = kEndNone;
      recorded_ = 0;
      if (rolling_) {
        rec_ = Rec::Waiting;
      } else {
        rec_ = Rec::CountIn;
        countIn_ = int32_t((16 * projStepFP_) >> 32);
        countEnd_ = false;
      }
      if (src_ == kSrcLine) {
        for (int i = 0; i < kHist; ++i) hist_[i].rec = 0;
        linePrimed_ = false;
        lTail_.store(lHead_.load(std::memory_order_acquire), std::memory_order_release);
        lineOpen_.store(true, std::memory_order_relaxed);
      }
      aWhy_.store(0, std::memory_order_relaxed);
      break;
    }
    case kOpEnd:
      if (rec_ == Rec::Rec && takeLive_) rec_ = Rec::Ending;
      else if (rec_ == Rec::Waiting || rec_ == Rec::CountIn) {
        rec_ = Rec::Idle;
        why_ = kEndCancel;
        lineOpen_.store(false, std::memory_order_relaxed);
      }
      break;
    case kOpCancel:
      if (rec_ == Rec::Rec || rec_ == Rec::Ending) finish(kEndStop);
      else if (rec_ == Rec::Waiting || rec_ == Rec::CountIn) {
        rec_ = Rec::Idle;
        why_ = kEndCancel;
        lineOpen_.store(false, std::memory_order_relaxed);
      }
      break;
    case kOpParam:
      if (c.step.note < kNumParams) {
        const int v = c.c > kParamMax[c.step.note] ? kParamMax[c.step.note] : c.c;
        set_[c.b & 3][c.step.note].store(uint8_t(v), std::memory_order_relaxed);
      }
      break;
    case kOpLoop:
      for (int i = 0; i < kNumLoops; ++i)
        if (kLoopBars[i] == c.c) loopBars_.store(c.c, std::memory_order_relaxed);
      break;
    case kOpTrick: wantTrick_ = c.c < kNumTricks ? c.c : uint8_t(kTrickNone); break;
    case kOpLatency: {
      const uint32_t f = uint32_t(c.step.note) | uint32_t(c.step.vel) << 8 | uint32_t(c.step.gate) << 16;
      latencyQ_ = (f + kQuantum / 2) / kQuantum;
      if (latencyQ_ > uint32_t(kHist - 2)) latencyQ_ = kHist - 2;
      lineTarget_ = uint32_t(c.c) * 64;
      if (lineTarget_ > kLineRing / 2) lineTarget_ = kLineRing / 2;
      break;
    }
  }
  aRec_.store(uint8_t(rec_), std::memory_order_release);
  updateActive();
}

void Tape::setTempo(int bpm, uint64_t stepFP) {
  projBpm_ = bpm;
  projStepFP_ = stepFP;
  const uint16_t tb = bpm_.load(std::memory_order_relaxed);
  if (tb == 0 || tb == bpm) {
    stepFP_ = stepFP;
  } else {
    const uint64_t srQ8 = uint64_t(rate_) * 256u;
    stepFP_ = ((srQ8 * 15u) << 24) / uint64_t(tb);
  }
}

void Tape::start(bool song, uint32_t songSteps, bool cont) {
  setTempo(projBpm_, projStepFP_);   // the tape's tempo may have been set since
  if (cont && posValid_) {
    rolling_ = true;
    stopFade_ = 0;
  } else {
    rolling_ = true;
    posValid_ = false;
    first_ = true;
    song_ = song;
    barFP_ = song ? uint64_t(songSteps) * stepFP_ : 0;
    barIdx_ = 0;
    stepsInBar_ = 0;
    addrFP_ = int64_t(barFP_);
    limit_ = addrNow();
    stopFade_ = 0;
    cur_ = Head{};
    old_ = Head{};
    xf_ = 0;
    trick_ = kTrickNone;
    lastLoop_ = 0;
    loopLen_ = song ? 0 : roundFP(uint64_t(loopBars_.load(std::memory_order_relaxed)) * 16u * stepFP_);
  }
  fadeGain_ = 1.0f;
  if (rec_ == Rec::CountIn) rec_ = Rec::Waiting;
  aRec_.store(uint8_t(rec_), std::memory_order_release);
  aSong_.store(song_, std::memory_order_relaxed);
  updateActive();
}

void Tape::stop() {
  if (rec_ == Rec::Rec || rec_ == Rec::Ending) finish(kEndStop);
  else if (rec_ == Rec::Waiting || rec_ == Rec::CountIn) {
    rec_ = Rec::Idle;
    why_ = kEndCancel;
    lineOpen_.store(false, std::memory_order_relaxed);
  }
  if (rolling_ && active_) stopFade_ = 4;
  rolling_ = false;
  wantTrick_ = kTrickNone;
  aRec_.store(uint8_t(rec_), std::memory_order_release);
  updateActive();
}

bool Tape::countInDone() {
  if (rec_ != Rec::CountIn) {
    aBeats_.store(0, std::memory_order_relaxed);
    return false;
  }
  // the transport starts one bar (in whole quanta) after the quantum that armed it
  if (countIn_ <= 0) return true;
  const int32_t beat = int32_t((4 * projStepFP_) >> 32);
  aBeats_.store(uint8_t(beat > 0 ? (countIn_ + beat - 1) / beat : 0), std::memory_order_relaxed);
  countIn_ -= kQuantum;
  return false;
}

// A bar line: where this quantum's first frame is on tape = where the bar starts on
// tape minus how far the bar's exact time lies after this quantum's first frame. At
// the tape's own tempo that is exactly where the tape already is (the sequencer's grid
// and the tape's are the same Q32 numbers): it never jumps. At another tempo, after a
// song position that moved, or at the loop's end it is a new place.
void Tape::masterStep(bool bar, int stepsInBar, int32_t songSteps, int64_t gridFP) {
  if (!rolling_) return;
  if (bar) {
    uint64_t fp = barFP_;
    const uint32_t prev = barIdx_;
    if (first_) {
      first_ = false;
      barIdx_ = 0;
    } else {
      fp = barFP_ + uint64_t(stepsInBar_) * stepFP_;
      ++barIdx_;
      if (!song_ && barIdx_ >= loopBars_.load(std::memory_order_relaxed)) {
        lastLoop_ = roundFP(fp);
        loopLen_ = lastLoop_;
        barIdx_ = 0;
        fp = 0;
      }
    }
    if (song_ && songSteps >= 0) fp = uint64_t(songSteps) * stepFP_;
    barFP_ = fp;
    stepsInBar_ = 0;
    const int64_t nfp = int64_t(fp) - gridFP;
    if (posValid_) {
      int64_t j = addrFP_ - nfp;   // > 0: the tape is ahead of where the bar wants it
      if (!song_ && loopLen_ > 0) {   // around the loop, the continued place is the same place
        const int64_t L = int64_t(loopLen_) << 32;
        j %= L;
        if (j > L / 2) j -= L;
        if (j < -L / 2) j += L;
      }
      if (j > kOne / 2 || j < -kOne / 2) jump(j);
    }
    posValid_ = true;
    addrFP_ = nfp;
    limit_ = int64_t(roundFP(fp + uint64_t(stepsInBar > 0 ? stepsInBar : 16) * stepFP_));
    if (rec_ == Rec::Rec || rec_ == Rec::Ending) takeBars_ |= 1u << (prev & 31);
    if (rec_ == Rec::Waiting) {
      beginTake();
    } else if (rec_ == Rec::Ending) {
      if (src_ == kSrcLine && latencyQ_ > 0) {
        takeLive_ = false;   // the input still to come belongs to the take: drain it
        drainQ_ = int(latencyQ_);
        rec_ = Rec::Rec;     // (published as recording until the drain ends)
        why_ = kEndBar;
      } else {
        finish(kEndBar);
      }
    }
  }
  ++stepsInBar_;
}

void Tape::beginTake() {
  rec_ = Rec::Rec;
  takeLive_ = true;
  drainQ_ = 0;
  takeStart_ = uint32_t(wrap(addrNow() < 0 ? 0 : addrNow()));
  recorded_ = 0;
  touchedN_ = 0;
  takeBars_ = 0;
  if (bpm_.load(std::memory_order_relaxed) == 0) {
    bpm_.store(uint16_t(projBpm_), std::memory_order_relaxed);
    stepFP_ = projStepFP_;
  }
  capTrack_ = src_ == kSrcTrack ? int(srcT_) : -1;
  capHave_ = false;
  aTakeStart_.store(takeStart_, std::memory_order_relaxed);
}

void Tape::finish(EndWhy why) {
  rec_ = Rec::Done;
  takeLive_ = false;
  drainQ_ = 0;
  why_ = why;
  capTrack_ = -1;
  lineOpen_.store(false, std::memory_order_relaxed);
  aTouched_.store(touchedN_, std::memory_order_relaxed);
  aWhy_.store(uint8_t(why), std::memory_order_relaxed);
  takeSeq_.fetch_add(1, std::memory_order_release);
  aRec_.store(uint8_t(Rec::Done), std::memory_order_release);
}

// A bar line moved the true address by -j (Q32): every head keeps reading where it
// was; the plain head then goes over to the new place (prepareHeads: the new place at
// full level from its first frame, the old one fading out, so a downbeat stays whole).
void Tape::jump(int64_t j) {
  cur_.off += j;
  if (xf_) old_.off += j;
}

void Tape::startTrick(uint8_t want) {
  const uint8_t from = trick_;
  trick_ = want;
  punch_ = false;
  if (want == kTrickBrake && from == kTrickNone && cur_.identity()) {   // a continuous slow-down
    cur_.acc = -(kOne / brakeFrames_);
    cur_.accLeft = brakeFrames_;
    return;
  }
  old_ = cur_;
  Head h;
  if (want == kTrickNone && from == kTrickBrake) {
    // spin up from where the brake has the speed, starting ahead of the true address
    // by as much as the spin-up lags, so that it lands on it
    float r0 = 1.0f + float(cur_.vel) * (1.0f / 4294967296.0f);
    r0 = r0 < 0.0f ? 0.0f : (r0 > 1.0f ? 1.0f : r0);
    const int32_t T = int32_t(float(spinFrames_) * (1.0f - r0));
    if (T >= 32) {
      const float d = 1.0f - r0;
      h.off = int64_t(d * float(T + 1) * 0.5f * 4294967296.0f);
      h.vel = -int64_t(d * 4294967296.0f);
      h.acc = int64_t(d * 4294967296.0f / float(T));
      h.accLeft = T;
    }
  } else if (want == kTrickBrake) {
    h.acc = -(kOne / brakeFrames_);
    h.accLeft = brakeFrames_;
  } else if (want == kTrickHalf) {
    h.vel = -(kOne / 2);
  } else if (want == kTrickReverse) {
    h.vel = -2 * kOne;
  }
  cur_ = h;
  xf_ = xfLen_ = kXfadeLong;
}

// The heads for this quantum: where each frame reads, the crossfade, the rate gains.
void Tape::prepareHeads() {
  if (xf_ == 0) {
    if (wantTrick_ != trick_) {
      startTrick(wantTrick_);
    } else if (trick_ == kTrickNone && cur_.vel == 0 && cur_.acc == 0 && cur_.off != 0) {
      // onto the true address: a bar line moved it, or a spin-up has landed
      const int64_t a = cur_.off < 0 ? -cur_.off : cur_.off;
      old_ = cur_;
      cur_ = Head{};
      punch_ = a > 2 * kOne;   // a real jump: the new place at once (a landing's rounding: a plain crossfade)
      xf_ = xfLen_ = punch_ ? kXfadeLong : kXfadeShort;
    }
  }
  // Past the bar's end on tape: at the tape's own tempo only by the grid's rounding (up
  // to half a quantum: those frames are the next bar's first, as they should be); at a
  // slower tempo the bar is longer than its tape: its last 64 frames fade out before the
  // end, then silence to the bar line (never the next bar's start early).
  const int64_t a0 = addrNow();
  const int64_t fadeEnd = stepFP_ == projStepFP_ ? limit_ + kQuantum + 64 : limit_;
  ovOn_ = a0 + kQuantum > fadeEnd - 64;
  if (ovOn_) {
    for (int i = 0; i < kQuantum; ++i) {
      const int64_t u = fadeEnd - (a0 + i);
      ov_[i] = u >= 64 ? 1.0f : (u <= 0 ? 0.0f : float(u) * (1.0f / 64.0f));
    }
  }
  fast_ = xf_ == 0 && cur_.identity();
  if (fast_) return;
  const float inv = xfLen_ ? 1.0f / float(xfLen_) : 0.0f;
  for (int i = 0; i < kQuantum; ++i) {
    pc_[i] = addrFP_ + (int64_t(i) << 32) + cur_.off;
    float r = 1.0f + float(cur_.vel) * (1.0f / 4294967296.0f);
    r = r < 0.0f ? -r : r;
    gc_[i] = r * 4.0f > 1.0f ? 1.0f : r * 4.0f;
    if (xf_ > 0) {
      po_[i] = addrFP_ + (int64_t(i) << 32) + old_.off;
      float ro = 1.0f + float(old_.vel) * (1.0f / 4294967296.0f);
      ro = ro < 0.0f ? -ro : ro;
      const float w = float(xf_) * inv;   // the old head's weight, 1 -> 0
      go_[i] = (ro * 4.0f > 1.0f ? 1.0f : ro * 4.0f) * w;
      wc_[i] = punch_ ? 1.0f : 1.0f - w;
      old_.off += old_.vel;
      if (old_.accLeft > 0) {
        old_.vel += old_.acc;
        if (--old_.accLeft == 0) { old_.vel = old_.acc < 0 ? -kOne : 0; old_.acc = 0; }
      }
      --xf_;
    } else {
      wc_[i] = 1.0f;
      go_[i] = 0.0f;
      po_[i] = pc_[i];
    }
    cur_.off += cur_.vel;
    if (cur_.accLeft > 0) {
      cur_.vel += cur_.acc;
      if (--cur_.accLeft == 0) {
        cur_.vel = cur_.acc < 0 ? -kOne : 0;   // the brake has stopped the tape / the spin-up has landed
        cur_.acc = 0;
      }
    }
  }
}

float Tape::sampleAt(int t, int64_t idx) const {
  idx = wrap(idx);
  if (idx < 0 || idx >= int64_t(idx_) * kBlock) return 0.0f;
  const uint32_t a = uint32_t(idx);
  const uint32_t e = entry(t, a >> kBlockShift).load(std::memory_order_acquire);
  const uint16_t b = baseOf(e), k = takeOf(e);
  const uint32_t o = a & (kBlock - 1);
  int32_t v = 0;
  if (k != kNoBlock) {
    v = blockPtr(k)[o];
    if (b != kNoBlock) v = guardSum(v + blockPtr(b)[o]);
  } else if (b != kNoBlock) {
    v = blockPtr(b)[o];
  }
  return float(v) * kFromTape;
}

void Tape::readFast(int t, int64_t a0, float* out) const {
  const int64_t a = wrap(a0);
  if (a < 0 || a + kQuantum > int64_t(idx_) * kBlock || (!song_ && loopLen_ > 0 && a + kQuantum > int64_t(loopLen_))) {
    for (int i = 0; i < kQuantum; ++i) out[i] = sampleAt(t, a0 + i);   // across an edge: frame by frame
    return;
  }
  int i = 0;
  while (i < kQuantum) {
    const uint32_t ai = uint32_t(a) + uint32_t(i);
    const uint32_t bi = ai >> kBlockShift;
    const uint32_t o = ai & (kBlock - 1);
    int n = int(kBlock - o);
    if (n > kQuantum - i) n = kQuantum - i;
    const uint32_t e = entry(t, bi).load(std::memory_order_acquire);
    const uint16_t b = baseOf(e), k = takeOf(e);
    const int16_t* bp = b != kNoBlock ? blockPtr(b) + o : nullptr;
    const int16_t* kp = k != kNoBlock ? blockPtr(k) + o : nullptr;
    if (kp && bp) {
      for (int j = 0; j < n; ++j) out[i + j] = float(guardSum(int32_t(kp[j]) + bp[j])) * kFromTape;
    } else if (kp) {
      for (int j = 0; j < n; ++j) out[i + j] = float(kp[j]) * kFromTape;
    } else if (bp) {
      for (int j = 0; j < n; ++j) out[i + j] = float(bp[j]) * kFromTape;
    } else {
      for (int j = 0; j < n; ++j) out[i + j] = 0.0f;
    }
    i += n;
  }
}

void Tape::runHeads(int t, float* out) {
  if (fast_) {
    readFast(t, addrNow(), out);
    if (ovOn_) for (int i = 0; i < kQuantum; ++i) out[i] *= ov_[i];
    return;
  }
  for (int i = 0; i < kQuantum; ++i) {
    const int64_t pc = pc_[i];
    const float fc = float(uint32_t(pc & 0xFFFFFFFF)) * (1.0f / 4294967296.0f);
    const int64_t ic = pc >> 32;
    const float s0 = sampleAt(t, ic), s1 = sampleAt(t, ic + 1);
    float y = (s0 + (s1 - s0) * fc) * gc_[i] * wc_[i];
    if (go_[i] > 0.0f) {
      const int64_t po = po_[i];
      const float fo = float(uint32_t(po & 0xFFFFFFFF)) * (1.0f / 4294967296.0f);
      const int64_t io = po >> 32;
      const float o0 = sampleAt(t, io), o1 = sampleAt(t, io + 1);
      y += (o0 + (o1 - o0) * fo) * go_[i];
    }
    out[i] = y;
  }
}

void Tape::recomputeGains(int t, bool mono) {
  uint8_t s[kNumParams];
  for (int p = 0; p < kNumParams; ++p) s[p] = set_[t][p].load(std::memory_order_relaxed);
  if (gainsInit_ && mono == cacheMono_ && std::memcmp(s, cache_[t], kNumParams) == 0) return;
  std::memcpy(cache_[t], s, kNumParams);
  // the instrument tracks' laws: level 100 = unity, cubic; constant-power pan, unity
  // at the centre; sends (v / 127)^2
  const float lv = float(s[kPLevel]) * (1.0f / 100.0f);
  const float g = s[kPMute] ? 0.0f : lv * lv * lv;
  tC_[t] = g;
  if (s[kPPan] == 64 || mono) {
    tL_[t] = tR_[t] = g;
  } else {
    float p = float(int(s[kPPan]) - 64) * (1.0f / 63.0f);
    p = p < -1.0f ? -1.0f : (p > 1.0f ? 1.0f : p);
    const float th = (p + 1.0f) * 0.78539816f;
    tL_[t] = g * 1.41421356f * cosf(th);
    tR_[t] = g * 1.41421356f * sinf(th);
  }
  const float d = float(s[kPDelay]) * (1.0f / 127.0f), r = float(s[kPReverb]) * (1.0f / 127.0f);
  tD_[t] = s[kPMute] ? 0.0f : d * d;
  tRv_[t] = s[kPMute] ? 0.0f : r * r;
}

void Tape::mix(float* busL_, float* busR_, float* sendD_, float* sendR_, bool mono, bool* dSend, bool* rSend) {
  float* __restrict busL = busL_;
  float* __restrict busR = busR_;
  float* __restrict sendD = sendD_;
  float* __restrict sendR = sendR_;
  for (int t = 0; t < kTracks; ++t) recomputeGains(t, mono);
  if (!gainsInit_ || !wasActive_) {   // nothing was flowing: the gains are where they should be
    for (int t = 0; t < kTracks; ++t) {
      gL_[t] = tL_[t]; gR_[t] = tR_[t]; sD_[t] = tD_[t]; sR_[t] = tRv_[t];
    }
    gainsInit_ = true;
  }
  cacheMono_ = mono;
  const bool playing = rolling_ || stopFade_ > 0;
  if (playing) prepareHeads();
  const bool recording = rec_ == Rec::Rec || rec_ == Rec::Ending;
  const float fade0 = fadeGain_;
  const float fade1 = rolling_ ? 1.0f : float(stopFade_ > 0 ? stopFade_ - 1 : 0) * 0.25f;
  fadeGain_ = fade1;
  const float inv = 1.0f / float(kQuantum);
  for (int t = 0; t < kTracks; ++t) {
    const bool armed = recording && t == armT_;
    // REPLACE: the armed track is not heard where this take has not written yet
    const bool mute = armed && replace_ && (song_ || !((takeBars_ >> (barIdx_ & 31)) & 1u));
    if (!playing || mute || !(trackHas_[t].load(std::memory_order_relaxed) || armed)) {
      for (int i = 0; i < kQuantum; ++i) out_[t][i] = 0.0f;
      if (!(armed && src_ == kSrcMix)) continue;
    } else {
      runHeads(t, out_[t]);
    }
    if (fade0 != 1.0f || fade1 != 1.0f) {
      float f = fade0;
      const float df = (fade1 - fade0) * inv;
      for (int i = 0; i < kQuantum; ++i) { f += df; out_[t][i] *= f; }
    }
    if (armed && src_ == kSrcMix) continue;   // added after the MIX capture (post), without sends
    // gains, smoothed as the instrument tracks' (4 ms)
    const float l0 = gL_[t], r0 = gR_[t], d0 = sD_[t], v0 = sR_[t];
    float l1 = l0 + (tL_[t] - l0) * gainK_, r1 = r0 + (tR_[t] - r0) * gainK_;
    float d1 = d0 + (tD_[t] - d0) * gainK_, v1 = v0 + (tRv_[t] - v0) * gainK_;
    if (std::fabs(tL_[t] - l1) < 1.0e-5f) l1 = tL_[t];
    if (std::fabs(tR_[t] - r1) < 1.0e-5f) r1 = tR_[t];
    if (std::fabs(tD_[t] - d1) < 1.0e-5f) d1 = tD_[t];
    if (std::fabs(tRv_[t] - v1) < 1.0e-5f) v1 = tRv_[t];
    gL_[t] = l1; gR_[t] = r1; sD_[t] = d1; sR_[t] = v1;
    const float* __restrict x = out_[t];
    const bool sends = d0 > 0.0f || d1 > 0.0f || v0 > 0.0f || v1 > 0.0f;
    if (sends) {
      if (d0 > 0.0f || d1 > 0.0f) *dSend = true;
      if (v0 > 0.0f || v1 > 0.0f) *rSend = true;
    }
    if (l0 == l1 && r0 == r1 && d0 == d1 && v0 == v1) {   // steady gains: the plain sums
      if (!sends) {
        if (mono) for (int i = 0; i < kQuantum; ++i) busL[i] += x[i] * l1;
        else for (int i = 0; i < kQuantum; ++i) { busL[i] += x[i] * l1; busR[i] += x[i] * r1; }
      } else if (mono) {
        const float gd = l1 * d1, gv = l1 * v1;
        for (int i = 0; i < kQuantum; ++i) {
          busL[i] += x[i] * l1;
          sendD[i] += x[i] * gd;
          sendR[i] += x[i] * gv;
        }
      } else {   // the send takes (a + b) / 2 of the panned pair, as the instrument tracks' do
        const float gd = 0.5f * (l1 + r1) * d1, gv = 0.5f * (l1 + r1) * v1;
        for (int i = 0; i < kQuantum; ++i) {
          busL[i] += x[i] * l1;
          busR[i] += x[i] * r1;
          sendD[i] += x[i] * gd;
          sendR[i] += x[i] * gv;
        }
      }
      continue;
    }
    float gl = l0, gr = r0, gd = d0, gv = v0;
    const float dl = (l1 - l0) * inv, dr = (r1 - r0) * inv, dd = (d1 - d0) * inv, dv = (v1 - v0) * inv;
    for (int i = 0; i < kQuantum; ++i) {
      gl += dl; gr += dr; gd += dd; gv += dv;
      const float a = x[i] * gl;
      if (mono) {
        busL[i] += a;
        sendD[i] += a * gd;
        sendR[i] += a * gv;
      } else {
        const float b = x[i] * gr;
        busL[i] += a;
        busR[i] += b;
        const float m = (a + b) * 0.5f;
        sendD[i] += m * gd;
        sendR[i] += m * gv;
      }
    }
  }
}

void Tape::capture(const float* dL, const float* dR, bool mono) {
  if (mono) for (int i = 0; i < kQuantum; ++i) cap_[i] = dL[i];
  else for (int i = 0; i < kQuantum; ++i) cap_[i] = 0.5f * (dL[i] + dR[i]);
  capHave_ = true;
}

void Tape::post(float* busL, float* busR, bool mono) {
  const bool recording = rec_ == Rec::Rec || rec_ == Rec::Ending;
  if (recording && takeLive_ && src_ == kSrcMix) {
    if (mono) for (int i = 0; i < kQuantum; ++i) cap_[i] = busL[i];
    else for (int i = 0; i < kQuantum; ++i) cap_[i] = 0.5f * (busL[i] + busR[i]);
    capHave_ = true;
  }
  if (recording && src_ == kSrcMix) {   // the armed track, heard dry (its sends are held at 0)
    const int t = armT_;
    const float l0 = gL_[t], r0 = gR_[t];
    float l1 = l0 + (tL_[t] - l0) * gainK_, r1 = r0 + (tR_[t] - r0) * gainK_;
    if (std::fabs(tL_[t] - l1) < 1.0e-5f) l1 = tL_[t];
    if (std::fabs(tR_[t] - r1) < 1.0e-5f) r1 = tR_[t];
    gL_[t] = l1; gR_[t] = r1;
    float gl = l0, gr = r0;
    const float dl = (l1 - l0) * (1.0f / kQuantum), dr = (r1 - r0) * (1.0f / kQuantum);
    for (int i = 0; i < kQuantum; ++i) {
      gl += dl; gr += dr;
      busL[i] += out_[t][i] * gl;
      if (!mono) busR[i] += out_[t][i] * gr;
    }
  }
  // the line input: the ring is kept at its target fill, so its latency is steady
  float line[kQuantum];
  if (lineOpen_.load(std::memory_order_relaxed)) {
    const uint32_t h = lHead_.load(std::memory_order_acquire);
    uint32_t tl = lTail_.load(std::memory_order_relaxed);
    uint32_t fill = h - tl;
    if (fill > lineTarget_ + 4096 + kQuantum) {   // the platform got ahead: back to the target
      tl = h - lineTarget_;
      fill = lineTarget_;
    }
    if (!linePrimed_ && fill >= lineTarget_ + kQuantum) linePrimed_ = true;
    if (linePrimed_ && fill >= uint32_t(kQuantum)) {
      for (int i = 0; i < kQuantum; ++i) line[i] = float(line_[(tl + uint32_t(i)) & (kLineRing - 1)]) * (1.0f / 32768.0f);
      tl += kQuantum;
    } else {
      for (int i = 0; i < kQuantum; ++i) line[i] = 0.0f;
      if (linePrimed_ && recording) lineUnder_.fetch_add(1, std::memory_order_relaxed);
      linePrimed_ = false;
    }
    lTail_.store(tl, std::memory_order_release);
  } else {
    linePrimed_ = false;
    for (int i = 0; i < kQuantum; ++i) line[i] = 0.0f;
  }
  // the history LINE writes from
  Hist& hq = hist_[histQ_ % kHist];
  hq.addr = addrNow();
  hq.limit = limit_;
  hq.rec = uint8_t(recording && takeLive_ ? 1 : 0);
  if (recording) {
    if (src_ == kSrcLine) {
      const Hist& h = hist_[(histQ_ + kHist - latencyQ_) % kHist];
      if (h.rec) writeQuantum(line, h.addr, h.limit);
      if (!takeLive_ && rec_ == Rec::Rec && drainQ_ > 0 && --drainQ_ == 0) finish(why_);
    } else if (takeLive_) {
      static const float kZero[kQuantum] = {};
      writeQuantum(capHave_ ? cap_ : kZero, addrNow(), limit_);
    }
  }
  capHave_ = false;
}

void Tape::writeQuantum(const float* src, int64_t addr, int64_t limit) {
  if (rec_ != Rec::Rec && rec_ != Rec::Ending) return;
  int16_t v[kQuantum];
  for (int i = 0; i < kQuantum; ++i) v[i] = toTape(src[i]);
  // a bar runs past its end on tape only at another tempo than the tape's: what does
  // not fit is left out (within the grid's rounding it is the next bar's first frames)
  int n = kQuantum;
  const int64_t room = limit + (stepFP_ == projStepFP_ ? kQuantum : 0) - addr;
  if (room < n) n = room < 0 ? 0 : int(room);
  int i = 0;
  if (addr < 0 && song_) i = int(-addr) < n ? int(-addr) : n;   // before the song's start
  while (i < n) {   // in runs that do not cross the loop's end
    const int64_t a = wrap(addr + i);
    if (a < 0) { ++i; continue; }
    int m = n - i;
    if (!song_ && loopLen_ > 0 && a + m > int64_t(loopLen_)) m = int(int64_t(loopLen_) - a);
    if (!writeAt(uint32_t(a), v + i, m)) return;
    i += m;
  }
  recorded_ += kQuantum;
}

bool Tape::writeAt(uint32_t a, const int16_t* v, int n) {
  const int t = armT_;
  int i = 0;
  while (i < n) {
    const uint32_t ai = a + uint32_t(i);
    const uint32_t bi = ai >> kBlockShift;
    if (bi >= idx_) {
      finish(kEndTapeEnd);
      return false;
    }
    const uint32_t o = ai & (kBlock - 1);
    int m = int(kBlock - o);
    if (m > n - i) m = n - i;
    std::atomic<uint32_t>& en = entry(t, bi);
    uint32_t e = en.load(std::memory_order_relaxed);
    uint16_t k = takeOf(e);
    if (k == kNoBlock) {
      const uint32_t h = rHead_.load(std::memory_order_relaxed);
      if (h == rTail_.load(std::memory_order_acquire)) {
        finish(kEndFull);
        return false;
      }
      k = ready_[h % kReady];
      rHead_.store(h + 1, std::memory_order_release);
      e = mkEntry(baseOf(e), k);
      en.store(e, std::memory_order_release);
      touched_[touchedN_++] = (uint32_t(t) << 16) | bi;
      trackHas_[t].store(true, std::memory_order_relaxed);
    }
    int16_t* kp = blockPtr(k) + o;
    uint8_t* pk = peaksOf(k);
    const uint16_t b = baseOf(e);
    const int16_t* bp = b != kNoBlock ? blockPtr(b) + o : nullptr;
    for (int j = 0; j < m; ++j) {
      int16_t w;
      uint8_t pv;
      if (replace_) {
        w = sat16(int32_t(v[i + j]) - (bp ? bp[j] : 0));
        pv = peakOf(v[i + j]);
      } else {
        w = sat16(int32_t(kp[j]) + v[i + j]);
        pv = peakOf(w);
      }
      uint8_t& pp = pk[(o + uint32_t(j)) >> kPeakShift];
      if (pv > pp) pp = pv;
      kp[j] = w;
    }
    i += m;
  }
  return true;
}

void Tape::updateActive() {
  bool has = false;
  for (int t = 0; t < kTracks; ++t) has = has || trackHas_[t].load(std::memory_order_relaxed);
  active_ = ((rolling_ || stopFade_ > 0) && has) || (rec_ != Rec::Idle && rec_ != Rec::Done && rec_ != Rec::CountIn) ||
            lineOpen_.load(std::memory_order_relaxed);
  if (!active_ && !rolling_) {
    fadeGain_ = 1.0f;
    xf_ = 0;
    cur_ = Head{};
    old_ = Head{};
    trick_ = kTrickNone;
  }
}

void Tape::endQuantum() {
  const bool moving = rolling_ || stopFade_ > 0;
  if (stopFade_ > 0) --stopFade_;
  if (moving) addrFP_ += int64_t(kQuantum) << 32;
  ++histQ_;
  if (rec_ == Rec::Done && mergedSeq_.load(std::memory_order_acquire) == takeSeq_.load(std::memory_order_relaxed))
    rec_ = Rec::Idle;
  const std::memory_order rx = std::memory_order_relaxed;
  aRec_.store(uint8_t(rec_), std::memory_order_release);
  aArmed_.store(armT_, rx);
  aSrc_.store(src_, rx);
  aSrcT_.store(srcT_, rx);
  aReplace_.store(replace_, rx);
  aRolling_.store(rolling_, rx);
  aTrick_.store(trick_, rx);
  const int64_t a = wrap(addrNow());
  aAddr_.store(a < 0 ? 0u : uint32_t(a), rx);
  {
    const int64_t h = wrap((addrFP_ + cur_.off + (int64_t(1) << 31)) >> 32);
    aHead_.store(h < 0 ? 0u : uint32_t(h), rx);
  }
  aRecorded_.store(recorded_, rx);
  aLoop_.store(loopLen_, rx);
  aQ_.fetch_add(1, std::memory_order_release);
  wasActive_ = active_;
  updateActive();
}

void Tape::lineIn(const int16_t* mono, int n) {
  if (!lineOpen_.load(std::memory_order_relaxed)) return;
  uint32_t h = lHead_.load(std::memory_order_relaxed);
  const uint32_t tl = lTail_.load(std::memory_order_acquire);
  for (int i = 0; i < n; ++i) {
    if (h - tl >= kLineRing) break;   // full: the rest is dropped
    line_[h & (kLineRing - 1)] = mono[i];
    ++h;
  }
  lHead_.store(h, std::memory_order_release);
}

// ---------------------------------------------------------- main thread ----

View Tape::view() const {
  const std::memory_order rx = std::memory_order_relaxed;
  View v;
  v.rec = Rec(aRec_.load(std::memory_order_acquire));
  v.armed = aArmed_.load(rx);
  v.source = aSrc_.load(rx);
  v.srcTrack = aSrcT_.load(rx);
  v.replace = aReplace_.load(rx);
  v.rolling = aRolling_.load(rx);
  v.song = aSong_.load(rx);
  v.trick = aTrick_.load(rx);
  v.countBeats = aBeats_.load(rx);
  v.why = EndWhy(aWhy_.load(rx));
  v.addr = aAddr_.load(rx);
  v.head = aHead_.load(rx);
  v.takeStart = aTakeStart_.load(rx);
  v.recorded = aRecorded_.load(rx);
  v.loopLen = aLoop_.load(rx);
  v.bpm = bpm_.load(rx);
  return v;
}

uint32_t Tape::nominalLoop(int bars, uint64_t projectStepFP) const {
  const uint16_t b = bpm_.load(std::memory_order_relaxed);
  uint64_t step = projectStepFP;
  if (b != 0) {
    const uint64_t srQ8 = uint64_t(rate_) * 256u;
    step = ((srQ8 * 15u) << 24) / uint64_t(b);
  }
  return roundFP(uint64_t(bars) * 16u * step);
}

uint32_t Tape::signature() const {
  uint32_t h = rev_.load(std::memory_order_relaxed) * 2654435761u;
  for (int t = 0; t < kTracks; ++t)
    for (int p = 0; p < kNumParams; ++p) h = (h ^ set_[t][p].load(std::memory_order_relaxed)) * 16777619u;
  h = (h ^ loopBars_.load(std::memory_order_relaxed)) * 16777619u;
  return (h ^ bpm_.load(std::memory_order_relaxed)) * 16777619u;
}

bool Tape::idle() const { return Rec(aRec_.load(std::memory_order_acquire)) == Rec::Idle && !merging_; }

bool Tape::canRecord() const {
  return blocks_ && idle() && rTail_.load(std::memory_order_relaxed) != rHead_.load(std::memory_order_acquire);
}

uint32_t Tape::freeBlocks() const {
  return freeN_ + qN_ + (rTail_.load(std::memory_order_relaxed) - rHead_.load(std::memory_order_relaxed));
}

uint16_t Tape::allocMain() {
  if (freeN_ == 0) return kNoBlock;
  return free_[--freeN_];
}

void Tape::release(uint16_t id) {
  if (id == kNoBlock) return;
  qId_[qN_] = id;
  qStamp_[qN_] = aQ_.load(std::memory_order_acquire);
  ++qN_;
}

bool Tape::trackEmpty(int t) const {
  for (uint32_t i = 0; i < idx_; ++i)
    if (entry(t, i).load(std::memory_order_relaxed) != kNoEntry) return false;
  return true;
}

bool Tape::empty() const {
  for (int t = 0; t < kTracks; ++t)
    if (!trackEmpty(t)) return false;
  return true;
}

uint32_t Tape::trackEnd(int t) const {
  for (uint32_t i = idx_; i-- > 0;)
    if (entry(t, i).load(std::memory_order_relaxed) != kNoEntry) return (i + 1) * kBlock;
  return 0;
}

void Tape::service() {
  if (!blocks_ && !blockMem_) return;
  // blocks out of use for two quanta of the audio thread are free again
  const uint32_t aq = aQ_.load(std::memory_order_acquire);
  for (uint32_t i = 0; i < qN_;) {
    if (aq - qStamp_[i] >= 2u) {
      free_[freeN_++] = qId_[i];
      qId_[i] = qId_[qN_ - 1];
      qStamp_[i] = qStamp_[qN_ - 1];
      --qN_;
    } else {
      ++i;
    }
  }
  // a take that ended: merge it
  if (!merging_ && Rec(aRec_.load(std::memory_order_acquire)) == Rec::Done &&
      mergedSeq_.load(std::memory_order_relaxed) != takeSeq_.load(std::memory_order_acquire)) {
    merging_ = true;
    mergeAt_ = 0;
    mergeN_ = aTouched_.load(std::memory_order_relaxed);
    undoN_ = 0;
    undone_ = false;
    undoBpmOnly_ = false;
  }
  if (merging_) mergeSome(8);
  // zeroed blocks for the audio thread
  int budget = 16;
  while (budget-- > 0 && freeN_ > kReserve) {
    const uint32_t tl = rTail_.load(std::memory_order_relaxed);
    if (tl - rHead_.load(std::memory_order_acquire) >= uint32_t(kReady)) break;
    const uint16_t id = free_[--freeN_];
    std::memset(blockPtr(id), 0, kBlock * 2);
    std::memset(peaksOf(id), 0, kPeaksPerBlock);
    ready_[tl % kReady] = id;
    rTail_.store(tl + 1, std::memory_order_release);
  }
}

void Tape::mergeSome(int budget) {
  while (mergeAt_ < mergeN_ && budget > 0) {
    const uint32_t key = touched_[mergeAt_];
    const int t = int(key >> 16);
    const uint32_t i = key & 0xFFFFu;
    std::atomic<uint32_t>& en = entry(t, i);
    const uint32_t e = en.load(std::memory_order_relaxed);
    const uint16_t b = baseOf(e), k = takeOf(e);
    if (k == kNoBlock) { ++mergeAt_; continue; }
    const int16_t* kp = blockPtr(k);
    if (b == kNoBlock) {
      bool zero = true;
      for (uint32_t j = 0; j < kBlock && zero; ++j) zero = kp[j] == 0;
      if (zero) {
        en.store(kNoEntry, std::memory_order_release);
        release(k);
      } else {
        if (replace_) {   // the take's peaks were the input's: they are the content's now
          uint8_t* pk = peaksOf(k);
          std::memset(pk, 0, kPeaksPerBlock);
          for (uint32_t j = 0; j < kBlock; ++j) { const uint8_t p = peakOf(kp[j]); if (p > pk[j >> kPeakShift]) pk[j >> kPeakShift] = p; }
        }
        en.store(mkEntry(k, kNoBlock), std::memory_order_release);
        undoKey_[undoN_] = key;
        undoBlk_[undoN_] = kNoBlock;
        ++undoN_;
      }
      ++mergeAt_;
      --budget;
      continue;
    }
    const uint16_t m = allocMain();
    if (m == kNoBlock) return;   // every block is in quarantine: the next frames
    int16_t* mp = blockPtr(m);
    const int16_t* bp = blockPtr(b);
    uint8_t* pk = peaksOf(m);
    std::memset(pk, 0, kPeaksPerBlock);
    bool zero = true;
    for (uint32_t j = 0; j < kBlock; ++j) {
      const int16_t v = int16_t(guardSum(int32_t(kp[j]) + bp[j]));
      mp[j] = v;
      zero = zero && v == 0;
      const uint8_t p = peakOf(v);
      if (p > pk[j >> kPeakShift]) pk[j >> kPeakShift] = p;
    }
    if (zero) {
      en.store(kNoEntry, std::memory_order_release);
      free_[freeN_++] = m;
    } else {
      en.store(mkEntry(m, kNoBlock), std::memory_order_release);
    }
    release(k);
    undoKey_[undoN_] = key;
    undoBlk_[undoN_] = b;   // the replaced base: the undo swaps it back
    ++undoN_;
    ++mergeAt_;
    --budget;
  }
  if (mergeAt_ < mergeN_) return;
  merging_ = false;
  ++merges_;
  if (wasEmpty_) { undoBpm_ = bpm_.load(std::memory_order_relaxed); undoBpmOnly_ = undoN_ == 0; }
  else undoBpm_ = 0;
  for (int t = 0; t < kTracks; ++t) trackHas_[t].store(!trackEmpty(t), std::memory_order_relaxed);
  if (empty()) bpm_.store(0, std::memory_order_relaxed);   // nothing was recorded after all
  rev_.fetch_add(1, std::memory_order_relaxed);
  mergedSeq_.store(takeSeq_.load(std::memory_order_acquire), std::memory_order_release);
}

void Tape::prepareTake() {
  for (uint32_t i = 0; i < undoN_; ++i) release(undoBlk_[i]);
  undoN_ = 0;
  undone_ = false;
  undoBpmOnly_ = false;
  wasEmpty_ = empty();
}

bool Tape::undo() {
  if (!idle() || !canUndo()) return false;
  for (uint32_t r = 0; r < undoN_; ++r) {
    const uint32_t key = undoKey_[r];
    std::atomic<uint32_t>& en = entry(int(key >> 16), key & 0xFFFFu);
    const uint16_t now = baseOf(en.load(std::memory_order_relaxed));
    en.store(mkEntry(undoBlk_[r], kNoBlock), std::memory_order_release);
    undoBlk_[r] = now;
  }
  // a take onto an empty tape set its tempo: it goes and comes back with it
  if (undoBpm_ != 0) {
    if (!undone_) bpm_.store(0, std::memory_order_relaxed);
    else bpm_.store(undoBpm_, std::memory_order_relaxed);
  } else if (undoBpmSaved_ != 0) {
    const uint16_t cur = bpm_.load(std::memory_order_relaxed);
    bpm_.store(undoBpmSaved_, std::memory_order_relaxed);
    undoBpmSaved_ = cur;
  }
  undone_ = !undone_;
  for (int t = 0; t < kTracks; ++t) trackHas_[t].store(!trackEmpty(t), std::memory_order_relaxed);
  rev_.fetch_add(1, std::memory_order_relaxed);
  return true;
}

bool Tape::erase(int t, uint32_t from, uint32_t to) {
  if (!idle() || t < 0 || t >= kTracks || to <= from) return false;
  prepareTake();
  undoBpm_ = 0;
  undoBpmSaved_ = 0;
  const uint16_t bpmBefore = bpm_.load(std::memory_order_relaxed);
  bool ok = true;
  for (uint32_t i = from >> kBlockShift; i < idx_ && i * kBlock < to; ++i) {
    std::atomic<uint32_t>& en = entry(t, i);
    const uint16_t b = baseOf(en.load(std::memory_order_relaxed));
    if (b == kNoBlock) continue;
    const uint32_t bs = i * kBlock;
    const uint32_t s = from > bs ? from - bs : 0;
    const uint32_t e = to - bs >= kBlock ? kBlock : to - bs;
    uint16_t nb = kNoBlock;
    if (s > 0 || e < kBlock) {
      nb = allocMain();
      if (nb == kNoBlock) { ok = false; break; }
      int16_t* np = blockPtr(nb);
      std::memcpy(np, blockPtr(b), kBlock * 2);
      std::memset(np + s, 0, (e - s) * 2);
      bool zero = true;
      uint8_t* pk = peaksOf(nb);
      std::memset(pk, 0, kPeaksPerBlock);
      for (uint32_t j = 0; j < kBlock; ++j) {
        zero = zero && np[j] == 0;
        const uint8_t p = peakOf(np[j]);
        if (p > pk[j >> kPeakShift]) pk[j >> kPeakShift] = p;
      }
      if (zero) { free_[freeN_++] = nb; nb = kNoBlock; }
    }
    en.store(mkEntry(nb, kNoBlock), std::memory_order_release);
    undoKey_[undoN_] = (uint32_t(t) << 16) | i;
    undoBlk_[undoN_] = b;
    ++undoN_;
  }
  trackHas_[t].store(!trackEmpty(t), std::memory_order_relaxed);
  if (empty()) {
    bpm_.store(0, std::memory_order_relaxed);
    undoBpmSaved_ = bpmBefore;   // the undo brings the tempo back with the audio
  }
  rev_.fetch_add(1, std::memory_order_relaxed);
  return ok;
}

void Tape::clear() {
  if (!blocks_) return;
  for (uint32_t i = 0; i < undoN_; ++i) release(undoBlk_[i]);
  undoN_ = 0;
  undone_ = undoBpmOnly_ = false;
  undoBpm_ = undoBpmSaved_ = 0;
  for (int t = 0; t < kTracks; ++t) {
    for (uint32_t i = 0; i < idx_; ++i) {
      std::atomic<uint32_t>& en = entry(t, i);
      const uint32_t e = en.load(std::memory_order_relaxed);
      if (e == kNoEntry) continue;
      en.store(kNoEntry, std::memory_order_release);
      release(baseOf(e));
      release(takeOf(e));
    }
    trackHas_[t].store(false, std::memory_order_relaxed);
    for (int q = 0; q < kNumParams; ++q) set_[t][q].store(kParamDefault[q], std::memory_order_relaxed);
  }
  loopBars_.store(kDefaultLoop, std::memory_order_relaxed);
  bpm_.store(0, std::memory_order_relaxed);
  rev_.fetch_add(1, std::memory_order_relaxed);
}

uint32_t Tape::read(int t, uint32_t pos, int16_t* out, uint32_t n) const {
  uint32_t i = 0;
  while (i < n) {
    const uint32_t a = pos + i;
    const uint32_t bi = a >> kBlockShift, o = a & (kBlock - 1);
    uint32_t m = kBlock - o;
    if (m > n - i) m = n - i;
    const uint32_t e = bi < idx_ ? entry(t, bi).load(std::memory_order_acquire) : kNoEntry;
    const uint16_t b = baseOf(e), k = takeOf(e);
    if (b == kNoBlock && k == kNoBlock) {
      std::memset(out + i, 0, m * 2);
    } else if (k == kNoBlock) {
      std::memcpy(out + i, blockPtr(b) + o, m * 2);
    } else {
      const int16_t* kp = blockPtr(k) + o;
      const int16_t* bp = b != kNoBlock ? blockPtr(b) + o : nullptr;
      for (uint32_t j = 0; j < m; ++j) out[i + j] = bp ? int16_t(guardSum(int32_t(kp[j]) + bp[j])) : kp[j];
    }
    i += m;
  }
  return n;
}

bool Tape::write(int t, uint32_t pos, const int16_t* in, uint32_t n) {
  if (t < 0 || t >= kTracks) return false;
  uint32_t i = 0;
  while (i < n) {
    const uint32_t a = pos + i;
    const uint32_t bi = a >> kBlockShift, o = a & (kBlock - 1);
    uint32_t m = kBlock - o;
    if (m > n - i) m = n - i;
    if (bi >= idx_) return false;
    std::atomic<uint32_t>& en = entry(t, bi);
    uint16_t b = baseOf(en.load(std::memory_order_relaxed));
    if (b == kNoBlock) {
      bool zero = true;
      for (uint32_t j = 0; j < m && zero; ++j) zero = in[i + j] == 0;
      if (zero) { i += m; continue; }
      b = allocMain();
      if (b == kNoBlock) return false;
      std::memset(blockPtr(b), 0, kBlock * 2);
      std::memset(peaksOf(b), 0, kPeaksPerBlock);
      std::memcpy(blockPtr(b) + o, in + i, m * 2);
      en.store(mkEntry(b, kNoBlock), std::memory_order_release);
      trackHas_[t].store(true, std::memory_order_relaxed);
    } else {
      std::memcpy(blockPtr(b) + o, in + i, m * 2);
    }
    uint8_t* pk = peaksOf(b);
    for (uint32_t j = 0; j < m; ++j) {
      const uint8_t p = peakOf(in[i + j]);
      uint8_t& pp = pk[(o + j) >> kPeakShift];
      if (p > pp) pp = p;
    }
    i += m;
  }
  rev_.fetch_add(1, std::memory_order_relaxed);
  return true;
}

uint8_t Tape::peak(int t, uint32_t pos, uint32_t n) const {
  if (n == 0) return 0;
  const uint32_t c0 = pos >> kPeakShift, c1 = (pos + n - 1) >> kPeakShift;
  uint8_t best = 0;
  const bool rep = replace_ && Rec(aRec_.load(std::memory_order_relaxed)) != Rec::Idle;
  for (uint32_t c = c0; c <= c1; ++c) {
    const uint32_t bi = c >> (kBlockShift - kPeakShift);
    if (bi >= idx_) break;
    const uint32_t e = entry(t, bi).load(std::memory_order_acquire);
    if (e == kNoEntry) {   // skip the rest of an empty block
      c = ((bi + 1) << (kBlockShift - kPeakShift)) - 1;
      continue;
    }
    const uint32_t j = c & uint32_t(kPeaksPerBlock - 1);
    const uint16_t b = baseOf(e), k = takeOf(e);
    int v = b != kNoBlock ? peaksOf(b)[j] : 0;
    if (k != kNoBlock) {
      const int w = peaksOf(k)[j];
      v = rep ? (w > v ? w : v) : (v + w > 255 ? 255 : v + w);
    }
    if (v > best) best = uint8_t(v);
  }
  return best;
}

}  // namespace tape
}  // namespace pg

#endif  // PG_TAPE
