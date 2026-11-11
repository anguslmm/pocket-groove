// pg::Engine — sequencer, command queue, mixer, ducker, send FX, master bus.
//
// One quantum (16 frames) of work, in order:
//   1. drain commands (the only place the Project is written)
//   2. refresh values derived from the globals (only when a global changed)
//   3. sequencer: note-offs due, step boundary, ratchet hits, slide note-offs
//   4. Voice::setParams with each track's effective params (p-locks applied)
//   5. render voices -> per-track gain/pan/duck -> dry bus + two send buses
//   6. send FX at half rate (delay, reverb) -> ducked return
//   7. master bus -> int16
//
// Design decisions that the contract headers leave open are documented where
// they are implemented; search for "DECISION".
#include "../engine.h"

#include <atomic>
#include <cmath>
#include <cstring>

#include "delay.h"
#include "dsp_util.h"
#include "ducker.h"
#include "engine_hooks.h"
#include "master.h"
#include "reverb.h"
#include "tables.h"

namespace pg {
namespace engine_detail {
namespace {

constexpr int kQueueSize = 256;                 // power of two; holds exactly 256 commands
constexpr int kMaxPend = 4;                     // pending note-offs per track (lead polyphony)
constexpr int32_t kTick = 256;                  // intra-step timers: 1/256 sample
constexpr int32_t kQuantumTicks = kQuantum * kTick;
constexpr int32_t kDueTicks = (kQuantum / 2) * kTick;   // events round to the NEAREST quantum edge
constexpr int64_t kQuantumFP = int64_t(kQuantum) << 32;
constexpr int64_t kHalfQuantumFP = int64_t(kQuantum / 2) << 32;
constexpr int kHalf = kQuantum / 2;             // FX block (half-rate samples per quantum)
constexpr int kSuspendFadeQuanta = 8;           // ~3 ms fade to silence on suspend()
constexpr uint8_t kNoPattern = 0xFF;

inline bool isSynthTrack(int t) { return t >= kNumDrumTracks; }
inline int sanitizeLength(int len) { return len == 0 ? 16 : (len > kMaxSteps ? kMaxSteps : len); }

struct PendNote {
  int32_t cd;          // ticks until the note-off is due
  uint8_t note;
  uint8_t active;
  uint8_t slideArmed;  // originating step had kStepSlide: the next note-on on this track is legato
  uint8_t holdToStep;  // held at least until the track's next step is evaluated
};

struct TrackRt {
  // ---- sequencer ----
  uint8_t pos, next;                  // last evaluated step / next step index
  uint8_t lockParam, lockValue;       // active p-lock (lockParam = index + 1, 0 = none)
  uint8_t supValid, supPattern, supStep;   // one-shot "don't fire this step" after a recorded note
  uint8_t ratLeft, ratSlide;
  int32_t ratCd, ratInterval, ratGate;
  NoteEvent ratEv;
  PendNote pend[kMaxPend];
  uint8_t trig;
  // ---- params ----
  uint8_t eff[kParamsPerTrack];       // effective params this quantum (p-lock applied)
  uint8_t mute;                       // cached Project mute
  uint8_t paramsSent;
  uint8_t pMin[kSoundParams], pMax[kSoundParams];   // voice param ranges (for sanitising)
  // ---- mixer ----
  int cLevel, cPan;                   // raw values the targets were computed from (-1 = none)
  float tgtL, tgtR;                   // level * pan
  float gL, gR;                       // smoothed level * pan * mute
  float outL, outR;                   // gains at the end of the previous quantum (incl. duck)
  float sendD, sendR;                 // send gains at the end of the previous quantum
  uint64_t mixKey;                    // raw mixer bytes 24..28 + mute the gains above were computed from
  uint8_t settled;                    // gains reached their targets: nothing to recompute until mixKey or the ducker moves
  uint8_t hasSends;                   // a send gain is non-zero (valid while settled)
};

}  // namespace

struct State {
  // ---- setup ----
  Project* proj;
  Voice* voice[kNumTracks];
  float sr;
  bool inited;

  // ---- UI -> audio command queue (lock-free SPSC ring) ----
  Command queue[kQueueSize];
  std::atomic<uint32_t> qHead;        // consumer (audio thread)
  std::atomic<uint32_t> qTail;        // producer (UI thread)

  // ---- audio -> UI status ----
  std::atomic<bool> aPlaying, aRecording;
  std::atomic<uint8_t> aPattern, aQueued, aSongSlot;
  std::atomic<uint8_t> aStep[kNumTracks], aTrig[kNumTracks];
  std::atomic<float> aPeakL, aPeakR, aDuck;
  std::atomic<uint32_t> aRevision;

  // ---- suspend / resume ----
  std::atomic<bool> suspendReq, inProcess;
  std::atomic<uint32_t> resumeEpoch;
  uint32_t seenEpoch;
  int suspendFade;                    // quanta of fade-out left
  std::atomic<uint32_t> seedReq;

  // ---- sequencer ----
  bool playing, recording;
  uint8_t curPattern, queued;
  uint8_t songSlot, songRep;
  bool songFresh;                     // song mode was just switched on: enter slot 0 at the next boundary
  int masterPos, masterNext;
  int64_t gridCd;                     // Q32 samples until the next STRAIGHT 16th grid line
  uint64_t stepLenFP;                 // Q32 samples per 16th
  int32_t stepTicks;                  // same in ticks
  int32_t sinceFire;                  // ticks since the last step's ideal time
  uint32_t rng;
  uint32_t revision;
  TrackRt tr[kNumTracks];

  // ---- derived from globals ----
  uint8_t gCache[kNumGlobals];
  bool gValid;
  int bpm, swing;
  int duckSrc;
  float duckFxAmt;
  int mFilter, mReso, mDrive, mVolume;
  int delayHalfSamples;

  // ---- mixer / FX / master ----
  float gainK;                        // per-quantum smoothing coefficient for track gains
  float fxDuckPrev;
  float duckPrev;                     // ducker gain at the end of the previous quantum
  float peakL, peakR;
  int dSilent, rSilent;               // quanta since the delay / reverb send bus last carried signal
  bool fxRunning;
  uint32_t faults;
  Ducker duck;
  HalfbandDown decD, decR;
  HalfbandUp upL, upR;
  Delay delay;
  Reverb reverb;
  MasterBus master;

  // ================= methods =================
  void init(float sampleRate, Project* p);
  void resetRuntime();
  void resetFx();
  void sanitizeProject();
  void sanitizeTrackSound(int t);
  bool sanitizeStep(int track, Step& s) const;
  void paramRange(int track, int idx, int& lo, int& hi) const;
  void cacheVoiceRanges(int t);

  void quantum(int16_t* out);
  void render(int16_t* out);
  void publishStatus();
  void drainCommands();
  void apply(const Command& c);
  void refreshGlobals();
  void bump() { ++revision; }

  // sequencer
  void sequencerTick();
  void fireMasterStep();
  void fireTrackStep(int t, const Step& st, int stepIndex, int32_t frac, int32_t dur);
  void hit(int t, NoteEvent ev, int32_t offCd, bool armSlide);
  void boundary();
  uint8_t decideNext(bool commit);
  int64_t swingOffFP(int idx) const { return (idx & 1) ? int64_t(stepLenFP * uint64_t(swing) / 100u) : 0; }
  int curPatternLen() const { return sanitizeLength(proj->patterns[curPattern].length); }
  void play(bool cont);
  void stop();
  void releaseNotes(int t);
  void liveNoteOn(const Command& c);
  void recordNote(int t, const NoteEvent& ev);
  void afterNoteOn(int t);
  void buildEff(int t);
  void ensureParams(int t);
  uint32_t rand32() {
    uint32_t x = rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    rng = x;
    return x;
  }
  void vOn(int t, const NoteEvent& e) { if (voice[t]) voice[t]->noteOn(e); }
  void vOff(int t, uint8_t n) { if (voice[t]) voice[t]->noteOff(n); }
  void vChoke(int t) { if (voice[t]) voice[t]->choke(); }
};

namespace {
State* g_state = nullptr;
size_t g_stateBytes = 0;
}  // namespace

// ---------------------------------------------------------------- setup ----

void State::init(float sampleRate, Project* p) {
  proj = p;
  sr = sampleRate > 8000.0f ? sampleRate : 8000.0f;
  qHead.store(0);
  qTail.store(0);
  suspendReq.store(false);
  seenEpoch = resumeEpoch.load();
  suspendFade = 0;
  seedReq.store(0);
  rng = kDefaultSeed;
  revision = 0;
  faults = 0;

  const float quantaPerSec = sr / float(kQuantum);
  gainK = 1.0f - expf(-1.0f / (0.004f * quantaPerSec));     // 4 ms; init is not real-time
  duck.init(sr, kQuantum);
  delay.init(sr * 0.5f);
  reverb.init(sr * 0.5f);
  master.init(sr, kQuantum);
  resetFx();
  resetRuntime();
  for (int t = 0; t < kNumTracks; ++t) cacheVoiceRanges(t);
  if (proj) sanitizeProject();
  gValid = false;
  if (proj) refreshGlobals();
  inited = proj != nullptr;
  publishStatus();
  aPeakL.store(0.0f);
  aPeakR.store(0.0f);
}

// Sequencer and mixer state back to "stopped at the top of pattern 0".
void State::resetRuntime() {
  playing = false;
  recording = false;
  curPattern = 0;
  queued = kNoPattern;
  songSlot = 0;
  songRep = 0;
  songFresh = false;
  masterPos = 0;
  masterNext = 0;
  gridCd = 0;
  sinceFire = 0;
  for (int t = 0; t < kNumTracks; ++t) {
    TrackRt& r = tr[t];
    r.pos = r.next = 0;
    r.lockParam = r.lockValue = 0;
    r.supValid = 0;
    r.ratLeft = 0;
    for (int i = 0; i < kMaxPend; ++i) r.pend[i].active = 0;
    r.trig = 0;
    r.paramsSent = 0;
    r.mute = 0;
    r.cLevel = r.cPan = -1;
    r.tgtL = r.tgtR = 0.0f;
    r.gL = r.gR = 0.0f;
    r.outL = r.outR = 0.0f;
    r.sendD = r.sendR = 0.0f;
    r.mixKey = 0;
    r.settled = 0;
    r.hasSends = 0;
    std::memset(r.eff, 0, sizeof(r.eff));
  }
  duck.reset();
  fxDuckPrev = 1.0f;
  duckPrev = 1.0f;
  peakL = peakR = 0.0f;
}

void State::resetFx() {
  delay.reset();
  reverb.reset();
  decD.reset(); decR.reset(); upL.reset(); upR.reset();
  master.reset();
  dSilent = rSilent = 1000;
  fxRunning = false;
}

void State::cacheVoiceRanges(int t) {
  TrackRt& r = tr[t];
  for (int i = 0; i < kSoundParams; ++i) {
    const ParamInfo* info = voice[t] ? voice[t]->paramInfo(i) : nullptr;
    r.pMin[i] = info ? info->min : 0;
    r.pMax[i] = info ? info->max : 255;
  }
}

void State::paramRange(int track, int idx, int& lo, int& hi) const {
  lo = 0; hi = 255;
  if (idx < 0) return;
  if (idx < kSoundParams) { lo = tr[track].pMin[idx]; hi = tr[track].pMax[idx]; return; }
  const ParamInfo* info = mixParamInfo(track, idx);
  if (info) { lo = info->min; hi = info->max; }
}

// Clamp one step to the documented field ranges. Returns true if it changed.
bool State::sanitizeStep(int track, Step& s) const {
  Step o = s;
  if (s.note > 127) s.note = 127;
  if (s.vel < 1) s.vel = 1;
  if (s.vel > 127) s.vel = 127;
  if (s.prob > 100) s.prob = 100;
  if (s.chord >= kChordNum) s.chord = 0;
  if (s.lockParam > kParamsPerTrack) s.lockParam = 0;
  if (s.lockParam) {
    int lo, hi;
    paramRange(track, s.lockParam - 1, lo, hi);
    s.lockValue = uint8_t(clampi(s.lockValue, lo, hi));
  }
  return std::memcmp(&o, &s, sizeof(Step)) != 0;
}

void State::sanitizeTrackSound(int t) {
  bool changed = false;
  TrackSettings& ts = proj->tracks[t];
  for (int i = 0; i < kParamsPerTrack; ++i) {
    int lo, hi;
    paramRange(t, i, lo, hi);
    const uint8_t v = uint8_t(clampi(ts.params[i], lo, hi));
    if (v != ts.params[i]) { ts.params[i] = v; changed = true; }
  }
  if (ts.mute > 1) { ts.mute = 1; changed = true; }
  for (int p = 0; p < kNumPatterns; ++p) {
    TrackPattern& tp = proj->patterns[p].tracks[t];
    const uint8_t len = uint8_t(sanitizeLength(tp.length));
    if (len != tp.length) { tp.length = len; changed = true; }
    for (int s = 0; s < kMaxSteps; ++s) changed |= sanitizeStep(t, tp.steps[s]);
  }
  if (changed) bump();
}

// The engine trusts the Project after this: every field it indexes with or
// maps to a coefficient is inside its documented range. Runs at init(), after
// resume() (a new project may have been loaded) and per command afterwards.
void State::sanitizeProject() {
  bool changed = false;
  for (int i = 0; i < kGNumUsed; ++i) {
    const ParamInfo* info = Engine::globalParamInfo(i);
    const uint8_t v = uint8_t(clampi(proj->globals[i], info->min, info->max));
    if (v != proj->globals[i]) { proj->globals[i] = v; changed = true; }
  }
  if (proj->songMode > 1) { proj->songMode = 1; changed = true; }
  for (int i = 0; i < kSongSlots; ++i) {
    if (proj->song[i].pattern >= kNumPatterns) { proj->song[i].pattern = kNumPatterns - 1; changed = true; }
  }
  for (int p = 0; p < kNumPatterns; ++p) {
    const uint8_t len = uint8_t(sanitizeLength(proj->patterns[p].length));
    if (len != proj->patterns[p].length) { proj->patterns[p].length = len; changed = true; }
  }
  if (changed) bump();
  for (int t = 0; t < kNumTracks; ++t) sanitizeTrackSound(t);
}

// ------------------------------------------------------------- globals ----

void State::refreshGlobals() {
  const uint8_t* g = proj->globals;
  if (gValid && std::memcmp(g, gCache, kNumGlobals) == 0) return;
  std::memcpy(gCache, g, kNumGlobals);
  uint8_t v[kGNumUsed];
  for (int i = 0; i < kGNumUsed; ++i) {
    const ParamInfo* info = Engine::globalParamInfo(i);
    v[i] = uint8_t(clampi(gCache[i], info->min, info->max));
  }

  // Tempo. Step length is kept in Q32 samples so the grid never drifts:
  // stepLen = sr * 60 / (bpm * 4) = sr * 15 / bpm.
  const int newBpm = v[kGTempo];
  if (!gValid || newBpm != bpm) {
    if (gValid && bpm > 0 && gridCd > 0) gridCd = gridCd * int64_t(bpm) / int64_t(newBpm);   // keep phase
    bpm = newBpm;
    const uint64_t srQ8 = uint64_t(sr * 256.0f + 0.5f);
    stepLenFP = ((srQ8 * 15u) << 24) / uint64_t(bpm);
    stepTicks = int32_t(stepLenFP >> 24);
    for (int t = 0; t < kNumTracks; ++t)
      if (voice[t]) voice[t]->setTempo(float(bpm));
  }
  swing = v[kGSwing];

  duckSrc = v[kGDuckSource];
  duck.setParams(v[kGDuckDepth], v[kGDuckHold], v[kGDuckRelease], v[kGDuckCurve]);
  duckFxAmt = float(v[kGDuckFx]) * (1.0f / 127.0f);

  // Delay: tempo-synced time (half-rate samples), feedback, tone, width.
  const float stepSamples = float(stepLenFP >> 16) * (1.0f / 65536.0f);
  const float half = sr * 0.5f;
  delayHalfSamples = delay.setTime(stepSamples * kDelayDivisionSteps[v[kGDelayTime]] * 0.5f);
  {
    const float tone = float(v[kGDelayTone]);
    // Low-pass 400 Hz .. 9 kHz over 0..96 (then flat); high-pass 60 Hz up to 1.5 kHz over 40..127.
    const float lpT = clampf(tone * (1.0f / 96.0f), 0.0f, 1.0f);
    const float hpT = clampf((tone - 40.0f) * (1.0f / 87.0f), 0.0f, 1.0f);
    const float lpHz = 400.0f * fastPow2(4.4919f * lpT);
    const float hpHz = 60.0f * fastPow2(4.6439f * hpT);
    delay.setParams(0.95f * float(v[kGDelayFeedback]) * (1.0f / 127.0f), onePoleCoef(lpHz, half),
                    onePoleCoef(hpHz, half), float(v[kGDelayWidth]) * (1.0f / 127.0f));
  }
  // Reverb: RT60 0.3 s .. 5 s, damping 9 kHz .. 900 Hz.
  {
    const float rt = 0.3f * fastPow2(4.0589f * float(v[kGReverbSize]) * (1.0f / 127.0f));
    const float dampHz = 9000.0f * fastPow2(-3.3219f * float(v[kGReverbDamp]) * (1.0f / 127.0f));
    reverb.setParams(rt, onePoleCoef(dampHz, half));
  }
  mFilter = v[kGMasterFilter];
  mReso = v[kGMasterReso];
  mDrive = v[kGMasterDrive];
  mVolume = v[kGMasterVolume];
  gValid = true;
}

// ------------------------------------------------------------ commands ----

void State::drainCommands() {
  uint32_t h = qHead.load(std::memory_order_relaxed);
  const uint32_t t = qTail.load(std::memory_order_acquire);
  // Bounded work per quantum: bulk edits (4 KB copies) wait for the next one
  // instead of stacking up inside a single 0.36 ms block. Order is preserved.
  int budget = 64;
  while (h != t && budget > 0) {
    const Command c = queue[h & (kQueueSize - 1)];
    ++h;
    switch (c.type) {
      case CmdType::ClearPattern: case CmdType::CopyPattern: budget -= 64; break;
      case CmdType::ClearTrack: case CmdType::CopyTrack: budget -= 8; break;
      default: budget -= 1; break;
    }
    apply(c);
  }
  qHead.store(h, std::memory_order_release);
}

void State::apply(const Command& c) {
  switch (c.type) {
    case CmdType::Play: play(c.a == 1); break;
    case CmdType::Stop: stop(); break;
    case CmdType::SetStep: {
      if (c.a >= kNumPatterns || c.b >= kNumTracks || c.c >= kMaxSteps) break;
      Step s = c.step;
      sanitizeStep(c.b, s);
      proj->patterns[c.a].tracks[c.b].steps[c.c] = s;
      bump();
      break;
    }
    case CmdType::SetTrackParam: {
      if (c.a >= kNumTracks || c.b >= kParamsPerTrack) break;
      int lo, hi;
      paramRange(c.a, c.b, lo, hi);
      proj->tracks[c.a].params[c.b] = uint8_t(clampi(c.c, lo, hi));
      tr[c.a].paramsSent = 0;
      bump();
      break;
    }
    case CmdType::SetGlobal: {
      if (c.a >= kNumGlobals) break;
      const ParamInfo* info = Engine::globalParamInfo(c.a);
      proj->globals[c.a] = info ? uint8_t(clampi(c.c, info->min, info->max)) : c.c;
      bump();
      refreshGlobals();     // so commands later in this same drain already see it
      break;
    }
    case CmdType::SetTrackLength:
      if (c.a >= kNumPatterns || c.b >= kNumTracks) break;
      proj->patterns[c.a].tracks[c.b].length = uint8_t(clampi(c.c, 1, kMaxSteps));
      bump();
      break;
    case CmdType::SetPatternLength:
      if (c.a >= kNumPatterns) break;
      proj->patterns[c.a].length = uint8_t(clampi(c.c, 1, kMaxSteps));
      bump();
      break;
    case CmdType::SetMute:
      if (c.a >= kNumTracks) break;
      proj->tracks[c.a].mute = c.c ? 1 : 0;
      bump();
      break;
    case CmdType::NoteOn:
      if (c.a < kNumTracks) liveNoteOn(c);
      break;
    case CmdType::NoteOff:
      if (c.a < kNumTracks) vOff(c.a, uint8_t(c.b & 127));
      break;
    case CmdType::QueuePattern:
      if (c.a >= kNumPatterns) break;
      if (playing) {
        queued = c.a;
      } else {
        // Stopped: switch immediately and park at the top of the pattern.
        curPattern = c.a;
        queued = kNoPattern;
        masterPos = masterNext = 0;
        for (int t = 0; t < kNumTracks; ++t) { tr[t].pos = tr[t].next = 0; tr[t].supValid = 0; }
      }
      break;
    case CmdType::SetSongMode:
      proj->songMode = c.c ? 1 : 0;
      if (proj->songMode) {
        if (playing) {
          songFresh = true;         // enter slot 0 at the next pattern boundary
        } else {
          songSlot = 0; songRep = 0; songFresh = false;
          if (proj->song[0].repeats) {
            curPattern = proj->song[0].pattern;
            masterPos = masterNext = 0;
            for (int t = 0; t < kNumTracks; ++t) tr[t].pos = tr[t].next = 0;
          }
        }
      }
      bump();
      break;
    case CmdType::SetSongSlot:
      if (c.a >= kSongSlots || c.b >= kNumPatterns) break;
      proj->song[c.a].pattern = c.b;
      proj->song[c.a].repeats = c.c;
      bump();
      break;
    case CmdType::SetRecord:
      recording = c.c != 0;
      break;
    case CmdType::ClearTrack: {
      // DECISION: clears the steps, keeps the track's length.
      if (c.a >= kNumPatterns || c.b >= kNumTracks) break;
      TrackPattern& tp = proj->patterns[c.a].tracks[c.b];
      for (int s = 0; s < kMaxSteps; ++s) tp.steps[s] = kDefaultStep;
      bump();
      break;
    }
    case CmdType::ClearPattern: {
      // DECISION: back to the initProject state (all steps default, all lengths 16).
      if (c.a >= kNumPatterns) break;
      Pattern& p = proj->patterns[c.a];
      p.length = 16;
      for (int t = 0; t < kNumTracks; ++t) {
        p.tracks[t].length = 16;
        for (int s = 0; s < kMaxSteps; ++s) p.tracks[t].steps[s] = kDefaultStep;
      }
      bump();
      break;
    }
    case CmdType::CopyPattern:
      if (c.a >= kNumPatterns || c.b >= kNumPatterns || c.a == c.b) break;
      std::memcpy(&proj->patterns[c.b], &proj->patterns[c.a], sizeof(Pattern));
      bump();
      break;
    case CmdType::CopyTrack: {
      // DECISION ("same type only"): drum -> drum and synth -> synth are allowed,
      // drum <-> synth is ignored. Bass <- lead drops the chord field; p-locks
      // on sound params are dropped when the voices differ (different meaning).
      if (c.a >= kNumPatterns || c.b >= kNumTracks || c.c >= kNumTracks || c.b == c.c) break;
      if (isSynthTrack(c.b) != isSynthTrack(c.c)) break;
      TrackPattern& dst = proj->patterns[c.a].tracks[c.c];
      std::memcpy(&dst, &proj->patterns[c.a].tracks[c.b], sizeof(TrackPattern));
      for (int s = 0; s < kMaxSteps; ++s) {
        Step& st = dst.steps[s];
        if (c.c != kTrackLead) st.chord = 0;
        if (st.lockParam && st.lockParam <= kSoundParams) { st.lockParam = 0; st.lockValue = 0; }
        sanitizeStep(c.c, st);
      }
      bump();
      break;
    }
  }
}

// ----------------------------------------------------------- transport ----

void State::releaseNotes(int t) {
  TrackRt& r = tr[t];
  for (int i = 0; i < kMaxPend; ++i) {
    if (r.pend[i].active) {
      vOff(t, r.pend[i].note);
      r.pend[i].active = 0;
    }
  }
  r.ratLeft = 0;
}

void State::play(bool cont) {
  if (cont) {
    // Continue: the next step fires in this quantum, position is kept.
    if (!playing) { playing = true; gridCd = 0; }
    return;
  }
  for (int t = 0; t < kNumTracks; ++t) {
    releaseNotes(t);
    TrackRt& r = tr[t];
    r.pos = r.next = 0;
    r.lockParam = 0;
    r.supValid = 0;
    r.paramsSent = 0;
  }
  if (proj->songMode) {
    songSlot = 0; songRep = 0; songFresh = false;
    if (proj->song[0].repeats) curPattern = proj->song[0].pattern;
  }
  masterPos = masterNext = 0;
  gridCd = 0;
  sinceFire = 0;
  playing = true;
}

// DECISION: Stop sends noteOff for every sequenced note that is still gated and
// then choke()s every voice (voice.h names transport stop as a choke case), so
// stop is tight; the send FX are untouched and ring out. P-locks are dropped so
// the voices return to the params the UI shows.
void State::stop() {
  playing = false;
  for (int t = 0; t < kNumTracks; ++t) {
    releaseNotes(t);
    vChoke(t);
    tr[t].lockParam = 0;
    tr[t].supValid = 0;
    tr[t].paramsSent = 0;
  }
  queued = kNoPattern;
}

// ------------------------------------------------------------ sequencer ----

void State::buildEff(int t) {
  TrackRt& r = tr[t];
  const TrackSettings& ts = proj->tracks[t];
  std::memcpy(r.eff, ts.params, kParamsPerTrack);
  if (r.lockParam) r.eff[r.lockParam - 1] = r.lockValue;
  r.mute = ts.mute;
}

void State::ensureParams(int t) {
  TrackRt& r = tr[t];
  if (r.paramsSent) return;
  buildEff(t);
  if (voice[t]) voice[t]->setParams(r.eff);
  r.paramsSent = 1;
}

// Things every note-on does, sequenced or live.
void State::afterNoteOn(int t) {
  ++tr[t].trig;
  if (t == kTrackHatClosed) {
    // Hat choke. The closed hat is track 3 and the open hat track 4, so when
    // both fire on the same step the open hat is choked first and then plays.
    vChoke(kTrackHatOpen);
    TrackRt& oh = tr[kTrackHatOpen];
    for (int i = 0; i < kMaxPend; ++i) oh.pend[i].active = 0;
  }
  // DECISION (ducker source KICK): follows the AUDIBLE kick, i.e. every kick
  // note-on that actually happens: sequenced steps that fire (ratchet hits
  // retrigger), and live kick notes. A muted kick track, or a kick step that
  // loses its probability roll, does not pump. To keep pumping with the kick
  // muted (breakdowns), use source BEAT, which triggers on every quarter note
  // of the grid regardless of the kick pattern (only while playing).
  if (t == kTrackKick && duckSrc == kDuckKick) duck.trigger();
}

// One sequenced note-on (a step, or one ratchet hit of a step).
// DECISION (slide, TB-303 semantics): a sounding note from a step with
// kStepSlide stays gated at least until the track's next step is evaluated.
// If that step fires, its note-on goes out first with slide = true and the old
// note's note-off is never sent (also across pattern wraps, polymeter wraps,
// queued patterns and song slots, and into a ratchet step's first hit). If it
// does not fire (rest, mute, lost probability roll) the held note ends on that
// step's grid time, or later if its own gate is longer. With ratchets only the
// LAST hit of the slide step carries the slide. Drum tracks ignore the flag.
void State::hit(int t, NoteEvent ev, int32_t offCd, bool armSlide) {
  TrackRt& r = tr[t];
  bool legato = false;
  if (isSynthTrack(t)) {
    for (int i = 0; i < kMaxPend; ++i) {
      if (r.pend[i].active && r.pend[i].slideArmed) {
        // 303 slide: the previous note is still gated; the new note-on goes
        // out FIRST, flagged legato, and the old note's note-off is dropped.
        r.pend[i].active = 0;
        legato = true;
      }
    }
  }
  ev.slide = legato;
  if (!legato) {
    // Retrigger. Mono tracks release whatever is still gated; the lead only
    // releases a pending copy of the same pitch (other pitches may overlap).
    for (int i = 0; i < kMaxPend; ++i) {
      if (r.pend[i].active && (t != kTrackLead || r.pend[i].note == ev.note)) {
        vOff(t, r.pend[i].note);
        r.pend[i].active = 0;
      }
    }
  }
  int slot = -1;
  for (int i = 0; i < kMaxPend; ++i)
    if (!r.pend[i].active) { slot = i; break; }
  if (slot < 0) {
    // Out of slots (only possible on the lead with long overlapping gates):
    // release the note that was going to end soonest, before the new note-on.
    slot = 0;
    for (int i = 1; i < kMaxPend; ++i)
      if (r.pend[i].cd < r.pend[slot].cd) slot = i;
    vOff(t, r.pend[slot].note);
  }
  ensureParams(t);
  vOn(t, ev);
  PendNote& p = r.pend[slot];
  p.note = ev.note;
  p.cd = offCd;
  p.active = 1;
  p.slideArmed = armSlide ? 1 : 0;
  p.holdToStep = armSlide ? 1 : 0;
  afterNoteOn(t);
}

void State::fireTrackStep(int t, const Step& st, int stepIndex, int32_t frac, int32_t dur) {
  TrackRt& r = tr[t];
  const bool suppressed = r.supValid && r.supPattern == curPattern && r.supStep == stepIndex;
  r.supValid = 0;
  r.ratLeft = 0;                                   // ratchets never spill into the next step

  bool fires = (st.flags & kStepOn) && !proj->tracks[t].mute && !suppressed;
  if (fires && st.prob < 100) fires = (rand32() % 100u) < st.prob;

  if (!fires) {
    // Slide into a rest (step off, muted, or lost its probability roll): a
    // note that was only being held for the slide ends here, on the grid.
    // If its own gate is longer than a step it simply keeps running (and can
    // still slide into a later step while it sounds).
    for (int i = 0; i < kMaxPend; ++i) {
      PendNote& p = r.pend[i];
      if (p.active && p.holdToStep) {
        p.holdToStep = 0;
        if (p.cd < kDueTicks) { vOff(t, p.note); p.active = 0; }
      }
    }
    return;
  }

  // Parameter lock scope: from this step until the next step of this track
  // that FIRES (with or without a lock of its own).
  const uint8_t newLock = (st.lockParam >= 1 && st.lockParam <= kParamsPerTrack) ? st.lockParam : 0;
  if (newLock != r.lockParam || st.lockValue != r.lockValue) r.paramsSent = 0;
  r.lockParam = newLock;
  r.lockValue = st.lockValue;

  const int ratchets = stepRatchet(st);
  int32_t gate = int32_t((int64_t(st.gate) * stepTicks) / (kGateUnitsPerStep * ratchets));
  if (gate < kQuantumTicks) gate = kQuantumTicks;
  NoteEvent ev;
  ev.note = st.note > 127 ? 127 : st.note;
  ev.vel = st.vel < 1 ? 1 : (st.vel > 127 ? 127 : st.vel);
  ev.accent = (st.flags & kStepAccent) != 0;
  ev.slide = false;
  ev.chord = (t == kTrackLead && st.chord < kChordNum) ? st.chord : 0;
  const bool slide = isSynthTrack(t) && (st.flags & kStepSlide);

  hit(t, ev, frac + gate, slide && ratchets == 1);
  if (ratchets > 1) {
    // Remaining hits are evenly spaced over the step's real (swung) duration;
    // only the LAST hit carries the slide into the next step.
    r.ratLeft = uint8_t(ratchets - 1);
    r.ratInterval = dur / ratchets;
    r.ratCd = frac + r.ratInterval;
    r.ratGate = gate;
    r.ratEv = ev;
    r.ratSlide = slide ? 1 : 0;
  }
}

// What plays after the current pattern ends. commit=false only peeks (used by
// record-quantise to find the pattern the upcoming downbeat belongs to).
// DECISION: a queued pattern always wins and does not consume a song repeat
// (it is a detour; the song carries on from where it was at the boundary after).
uint8_t State::decideNext(bool commit) {
  if (queued != kNoPattern) return queued;
  uint8_t pattern = curPattern;
  if (proj->songMode) {
    int slot = songSlot, rep = songRep;
    if (songFresh) {
      slot = 0; rep = 0;
    } else {
      ++rep;
      if (rep >= proj->song[slot].repeats) {
        rep = 0;
        ++slot;
        if (slot >= kSongSlots || proj->song[slot].repeats == 0) slot = 0;   // repeats 0 = end: loop
      }
    }
    if (proj->song[slot].repeats != 0) {
      const uint8_t p = proj->song[slot].pattern;
      pattern = p < kNumPatterns ? p : uint8_t(kNumPatterns - 1);
    }
    if (commit) { songSlot = uint8_t(slot); songRep = uint8_t(rep); songFresh = false; }
  }
  return pattern;
}

void State::boundary() {
  curPattern = decideNext(true);
  queued = kNoPattern;
  masterNext = 0;
  for (int t = 0; t < kNumTracks; ++t) tr[t].next = 0;
}

void State::fireMasterStep() {
  if (masterNext >= curPatternLen()) boundary();
  const Pattern& pat = proj->patterns[curPattern];
  const int len = sanitizeLength(pat.length);
  const int idx = masterNext;
  masterPos = idx;
  masterNext = idx + 1;

  // `frac`: where the ideal (fractional) step time sits relative to this
  // quantum edge; note-offs and ratchets are timed from the ideal time.
  const int64_t offThis = swingOffFP(idx);
  int64_t fracFP = gridCd + offThis;
  if (fracFP < -kHalfQuantumFP) fracFP = -kHalfQuantumFP;
  const int32_t frac = int32_t(fracFP >> 24);
  const int nextIdx = (idx + 1 >= len) ? 0 : idx + 1;
  const int32_t dur = int32_t((int64_t(stepLenFP) + swingOffFP(nextIdx) - offThis) >> 24);
  sinceFire = -frac;

  if (duckSrc == kDuckBeat && (idx & 3) == 0) duck.trigger();

  for (int t = 0; t < kNumTracks; ++t) {
    const TrackPattern& tp = pat.tracks[t];
    const int tlen = sanitizeLength(tp.length);
    TrackRt& r = tr[t];
    int s = r.next;
    if (s >= tlen) s = 0;                          // polymeter: each track wraps on its own length
    r.pos = uint8_t(s);
    r.next = uint8_t(s + 1);
    fireTrackStep(t, tp.steps[s], s, frac, dur);
  }
  gridCd += int64_t(stepLenFP);
}

void State::sequencerTick() {
  // 1. Note-offs that are due, except notes armed for a slide (those must
  //    see whether a step fires in this same quantum first).
  for (int t = 0; t < kNumTracks; ++t) {
    TrackRt& r = tr[t];
    for (int i = 0; i < kMaxPend; ++i) {
      PendNote& p = r.pend[i];
      if (p.active && !p.slideArmed && p.cd < kDueTicks) { vOff(t, p.note); p.active = 0; }
    }
  }
  // 2. Step boundary. Odd 16ths are delayed by the swing offset; the grid
  //    itself stays straight so changing swing never shifts the bar.
  {
    const int idx = masterNext >= curPatternLen() ? 0 : masterNext;
    if (gridCd + swingOffFP(idx) < kHalfQuantumFP) fireMasterStep();
  }
  // 3. Ratchet hits.
  for (int t = 0; t < kNumTracks; ++t) {
    TrackRt& r = tr[t];
    if (r.ratLeft && r.ratCd < kDueTicks) {
      if (proj->tracks[t].mute) {
        r.ratLeft = 0;
      } else {
        const bool last = r.ratLeft == 1;
        hit(t, r.ratEv, r.ratCd + r.ratGate, last && r.ratSlide);
        --r.ratLeft;
        r.ratCd += r.ratInterval;
      }
    }
  }
  // 4. Slide-armed notes whose (long) gate ran out without anything to slide into.
  for (int t = 0; t < kNumTracks; ++t) {
    TrackRt& r = tr[t];
    for (int i = 0; i < kMaxPend; ++i) {
      PendNote& p = r.pend[i];
      if (p.active && p.slideArmed && !p.holdToStep && p.cd < kDueTicks) { vOff(t, p.note); p.active = 0; }
    }
  }
  // 5. Advance time.
  gridCd -= kQuantumFP;
  if (sinceFire < (1 << 30)) sinceFire += kQuantumTicks;
  for (int t = 0; t < kNumTracks; ++t) {
    TrackRt& r = tr[t];
    if (r.ratLeft) r.ratCd -= kQuantumTicks;
    for (int i = 0; i < kMaxPend; ++i)
      if (r.pend[i].active && r.pend[i].cd > -(1 << 30)) r.pend[i].cd -= kQuantumTicks;
  }
}

// ------------------------------------------------------------ live play ----

void State::liveNoteOn(const Command& c) {
  const int t = c.a;
  NoteEvent ev;
  ev.note = uint8_t(c.b > 127 ? 127 : c.b);
  ev.vel = uint8_t(clampi(c.c, 1, 127));
  ev.accent = (c.step.flags & kStepAccent) != 0;
  ev.slide = isSynthTrack(t) && (c.step.flags & kStepSlide);
  ev.chord = (t == kTrackLead && c.step.chord < kChordNum) ? c.step.chord : 0;
  ensureParams(t);
  vOn(t, ev);
  afterNoteOn(t);
  if (playing && recording) recordNote(t, ev);
}

// DECISION (record quantise): the note goes to whichever step's fire time
// (swing included) is nearer: the step this track last played, or the one it
// is about to play. Ties go to the step already played. If it lands on the
// upcoming step, that step is suppressed once, so the player hears their own
// live note and not a second trigger a few ms later. At a pattern boundary
// the upcoming step is step 0 of whatever pattern plays next (queued pattern /
// song slot), because that is the downbeat the player was aiming at.
// Drums: step on + velocity (tuning, p-lock, prob, ratchet untouched).
// Synths: note, velocity, accent/slide as played, default gate, chord (lead).
void State::recordNote(int t, const NoteEvent& ev) {
  TrackRt& r = tr[t];
  const int len = curPatternLen();
  const int nextMaster = masterNext >= len ? 0 : masterNext;
  const int32_t toNext = int32_t((gridCd + swingOffFP(nextMaster)) >> 24);
  int pattern = curPattern, step = r.pos;
  if (sinceFire > toNext) {
    if (masterNext >= len) {
      pattern = decideNext(false);
      step = 0;
    } else {
      step = r.next;
      if (step >= sanitizeLength(proj->patterns[pattern].tracks[t].length)) step = 0;
    }
    r.supValid = 1;
    r.supPattern = uint8_t(pattern);
    r.supStep = uint8_t(step);
  }
  Step& s = proj->patterns[pattern].tracks[t].steps[step];
  s.vel = ev.vel;
  if (isSynthTrack(t)) {
    s.note = ev.note;
    s.flags = uint8_t((s.flags & kStepRatchetMask) | kStepOn | (ev.accent ? kStepAccent : 0) |
                      (ev.slide ? kStepSlide : 0));
    s.gate = kDefaultStep.gate;
    s.chord = ev.chord;
  } else {
    s.flags |= kStepOn;
  }
  bump();
}

// --------------------------------------------------------------- render ----

// MONO OUTPUT (not built): the device's codec plays (L+R)/2. A "mono output"
// switch would live here as a flag on State and cut three things:
//   1. the track loop below accumulates one bus with gain (gl+gr)/2 (pan is
//      moot) instead of busL/busR;
//   2. the FX return needs only the mono sum: Delay emits e1+e2 (its two
//      taps, no width law), Reverb skips its side tap set (oR) and upR goes;
//   3. MasterBus::process runs filter/drive/limiter on one channel and writes
//      the sample to both int16 slots.
// The mono sum of the stereo engine is already exact for the delay and the
// reverb (see delay.h / reverb.h), so the switch changes cost, not sound.
void State::render(int16_t* out) {
  float busL[kQuantum], busR[kQuantum], sD[kQuantum], sR[kQuantum];
  for (int i = 0; i < kQuantum; ++i) { busL[i] = 0.0f; busR[i] = 0.0f; sD[i] = 0.0f; sR[i] = 0.0f; }
  const float duckEnd = duck.tick();
  const float invN = 1.0f / float(kQuantum);
  bool dSend = false, rSend = false;

  const bool duckMoved = duckEnd != duckPrev;
  duckPrev = duckEnd;

  for (int t = 0; t < kNumTracks; ++t) {
    TrackRt& r = tr[t];
    // ---- control: targets for this quantum ----
    // Fast path: when the raw mixer bytes, the mute and (for a ducked track)
    // the ducker gain are what they were, and the smoothed gains had already
    // arrived, the gains are exactly last quantum's: skip all of this.
    uint64_t key;
    std::memcpy(&key, r.eff + kMixLevel, sizeof(key));        // bytes 24..31
    key = (key & 0xFFFFFFFFFFull) | (uint64_t(r.mute ? 1 : 0) << 40);   // level, pan, dly, rev, duck + mute
    float endL, endR, endD, endR2;
    bool steady, sends;
    if (r.settled && key == r.mixKey && !(duckMoved && r.eff[kMixDuck] != 0)) {
      endL = r.outL; endR = r.outR; endD = r.sendD; endR2 = r.sendR;
      steady = true;
      sends = r.hasSends != 0;
    } else {
      r.mixKey = key;
      const int level = r.eff[kMixLevel], pan = r.eff[kMixPan];
      if (level != r.cLevel || pan != r.cPan) {
        r.cLevel = level;
        r.cPan = pan;
        // Level: 100 = unity, cubic taper. Pan: constant-power balance, unity at centre.
        const float lv = clampf(float(level) / 100.0f, 0.0f, 1.27f);
        const float g = lv * lv * lv;
        const float p = clampf(float(pan - 64) * (1.0f / 63.0f), -1.0f, 1.0f);
        const float th = (p + 1.0f) * 0.78539816f;
        r.tgtL = g * 1.41421356f * cosf(th);      // libm only when level/pan actually change
        r.tgtR = g * 1.41421356f * sinf(th);
        if (pan == 64) { r.tgtL = g; r.tgtR = g; }
      }
      const float tL = r.mute ? 0.0f : r.tgtL, tR = r.mute ? 0.0f : r.tgtR;
      r.gL += (tL - r.gL) * gainK;
      r.gR += (tR - r.gR) * gainK;
      if (absf(tL - r.gL) < 1.0e-5f) r.gL = tL;
      if (absf(tR - r.gR) < 1.0e-5f) r.gR = tR;
      const float duckF = 1.0f + float(r.eff[kMixDuck]) * (1.0f / 127.0f) * (duckEnd - 1.0f);
      endL = r.gL * duckF;
      endR = r.gR * duckF;
      const float dv = float(r.eff[kMixDelaySend]) * (1.0f / 127.0f);
      const float rv = float(r.eff[kMixReverbSend]) * (1.0f / 127.0f);
      endD = r.sendD + (dv * dv - r.sendD) * gainK;
      endR2 = r.sendR + (rv * rv - r.sendR) * gainK;
      if (absf(dv * dv - endD) < 1.0e-5f) endD = dv * dv;
      if (absf(rv * rv - endR2) < 1.0e-5f) endR2 = rv * rv;
      r.settled = (r.gL == tL && r.gR == tR && endD == dv * dv && endR2 == rv * rv) ? 1 : 0;
      r.hasSends = (endD > 0.0f || endR2 > 0.0f) ? 1 : 0;
      steady = endL == r.outL && endR == r.outR && endD == r.sendD && endR2 == r.sendR;
      sends = endD > 0.0f || endR2 > 0.0f || r.sendD > 0.0f || r.sendR > 0.0f;
    }

    Voice* v = voice[t];
    if (v && v->active()) {
      float L[kQuantum], R[kQuantum];
      v->render(L, R, kQuantum);
      // A voice that went non-finite (or absurdly large) is silenced and
      // choked instead of poisoning the buses and FX states.
      if (!(absf(L[kQuantum - 1]) < 64.0f) || !(absf(R[kQuantum - 1]) < 64.0f) ||
          !(absf(L[0]) < 64.0f) || !(absf(R[0]) < 64.0f)) {
        v->choke();
        ++faults;
      } else {
        if (steady && !sends) {
          const float gl = endL, gr = endR;
          for (int i = 0; i < kQuantum; ++i) {
            busL[i] += L[i] * gl;
            busR[i] += R[i] * gr;
          }
        } else if (steady) {
          const float gl = endL, gr = endR, gd = 0.5f * endD, gv = 0.5f * endR2;
          for (int i = 0; i < kQuantum; ++i) {
            const float a = L[i] * gl, b = R[i] * gr;
            busL[i] += a;
            busR[i] += b;
            const float m = a + b;
            sD[i] += m * gd;
            sR[i] += m * gv;
          }
        } else if (endD == r.sendD && endR2 == r.sendR) {
          // Level/pan/mute/duck moving, sends steady (the common ducking case): ramp the gains only.
          float gl = r.outL, gr = r.outR;
          const float dl = (endL - gl) * invN, dr = (endR - gr) * invN;
          if (sends) {
            const float gd = 0.5f * endD, gv = 0.5f * endR2;
            for (int i = 0; i < kQuantum; ++i) {
              gl += dl; gr += dr;
              const float a = L[i] * gl, b = R[i] * gr;
              busL[i] += a;
              busR[i] += b;
              const float m = a + b;
              sD[i] += m * gd;
              sR[i] += m * gv;
            }
          } else {
            for (int i = 0; i < kQuantum; ++i) {
              gl += dl; gr += dr;
              busL[i] += L[i] * gl;
              busR[i] += R[i] * gr;
            }
          }
        } else {
          // Sends moving too: ramp everything linearly.
          float gl = r.outL, gr = r.outR, gd = 0.5f * r.sendD, gv = 0.5f * r.sendR;
          const float dl = (endL - gl) * invN, dr = (endR - gr) * invN;
          const float dd = (0.5f * endD - gd) * invN, dvv = (0.5f * endR2 - gv) * invN;
          for (int i = 0; i < kQuantum; ++i) {
            gl += dl; gr += dr; gd += dd; gv += dvv;
            const float a = L[i] * gl, b = R[i] * gr;
            busL[i] += a;
            busR[i] += b;
            const float m = a + b;
            sD[i] += m * gd;
            sR[i] += m * gv;
          }
        }
        if (sends) {
          if (endD > 0.0f || r.sendD > 0.0f) dSend = true;
          if (endR2 > 0.0f || r.sendR > 0.0f) rSend = true;
        }
      }
    }
    r.outL = endL;
    r.outR = endR;
    r.sendD = endD;
    r.sendR = endR2;
  }

  // A voice that emits a NaN/inf (or absurd values) somewhere inside a block
  // passes the cheap first/last-sample check above; catch it on the summed
  // buses before it reaches the recirculating FX and filter states (where it
  // would latch). Rare by construction: drop the block and choke everything.
  {
    float acc = 0.0f;
    for (int i = 0; i < kQuantum; ++i) acc += (busL[i] + busR[i]) + (sD[i] + sR[i]);
    if (!(absf(acc) < 1.0e5f)) {
      for (int i = 0; i < kQuantum; ++i) { busL[i] = 0.0f; busR[i] = 0.0f; sD[i] = 0.0f; sR[i] = 0.0f; }
      for (int t = 0; t < kNumTracks; ++t) vChoke(t);
      ++faults;
    }
  }

  // ---- send FX at half rate; skipped entirely once silent ----
  if (dSend) dSilent = 0; else if (dSilent < 1000) ++dSilent;
  if (rSend) rSilent = 0; else if (rSilent < 1000) ++rSilent;
  const bool runDelay = dSilent < 16 || !delay.idle();
  const bool runReverb = rSilent < 16 || !reverb.idle();
  const float fxEnd = 1.0f + duckFxAmt * (duckEnd - 1.0f);
  if (runDelay || runReverb) {
    float in[kHalf], wL[kHalf], wR[kHalf], fL[kQuantum], fR[kQuantum];
    for (int i = 0; i < kHalf; ++i) { wL[i] = 0.0f; wR[i] = 0.0f; }
    if (runDelay) {
      decD.process(sD, in, kHalf);
      delay.process(in, wL, wR, kHalf);
    }
    if (runReverb) {
      decR.process(sR, in, kHalf);
      reverb.process(in, wL, wR, kHalf);
    }
    if (!(absf(wL[kHalf - 1]) < 64.0f) || !(absf(wR[kHalf - 1]) < 64.0f)) {
      delay.reset();
      reverb.reset();
      decD.reset(); decR.reset(); upL.reset(); upR.reset();
      ++faults;
    } else {
      upL.process(wL, fL, kHalf);
      upR.process(wR, fR, kHalf);
      float g = fxDuckPrev;
      const float dg = (fxEnd - g) * invN;
      for (int i = 0; i < kQuantum; ++i) {
        g += dg;
        busL[i] += fL[i] * g;
        busR[i] += fR[i] * g;
      }
    }
    fxRunning = true;
  } else if (fxRunning) {
    decD.reset(); decR.reset(); upL.reset(); upR.reset();
    fxRunning = false;
  }
  fxDuckPrev = fxEnd;

  master.setParams(mFilter, mReso, mDrive, mVolume);
  master.process(busL, busR, out, kQuantum, &peakL, &peakR);
}

void State::publishStatus() {
  const std::memory_order rx = std::memory_order_relaxed;
  aPlaying.store(playing, rx);
  aRecording.store(recording, rx);
  aPattern.store(curPattern, rx);
  aQueued.store(queued, rx);
  aSongSlot.store(songSlot, rx);
  for (int t = 0; t < kNumTracks; ++t) {
    aStep[t].store(tr[t].pos, rx);
    aTrig[t].store(tr[t].trig, rx);
  }
  aDuck.store(duck.gain(), rx);
  aRevision.store(revision, rx);
  // Peak hold: the UI resets these to 0 in status(); a reset that races with
  // this max is simply re-raised on the next quantum.
  if (peakL > aPeakL.load(rx)) aPeakL.store(peakL, rx);
  if (peakR > aPeakR.load(rx)) aPeakR.store(peakR, rx);
  peakL = peakR = 0.0f;
}

void State::quantum(int16_t* out) {
  if (seedReq.load(std::memory_order_relaxed) != 0) {
    const uint32_t s = seedReq.exchange(0, std::memory_order_relaxed);
    if (s) rng = s;
  }
  for (int t = 0; t < kNumTracks; ++t) tr[t].paramsSent = 0;
  drainCommands();
  refreshGlobals();
  if (playing) sequencerTick();
  for (int t = 0; t < kNumTracks; ++t) ensureParams(t);
  render(out);
  publishStatus();
}

}  // namespace engine_detail

// ================================================================ Engine ====

// DECISION: engine.h says the Impl is "statically allocated inside engine.cpp,
// no heap" and Engine has no destructor, so there is exactly ONE state block:
// every Engine object is a handle to the same engine. The firmware has one.
struct Engine::Impl : engine_detail::State {};

Engine::Engine() {
  static Impl storage;     // zero-initialised, lives in .bss (~100 KB)
  impl_ = &storage;
  engine_detail::g_state = &storage;
  engine_detail::g_stateBytes = sizeof(Impl);
}

void Engine::init(float sampleRate, Project* project) { impl_->init(sampleRate, project); }

// DECISION: the engine never calls Voice::init (voice.h: "called once, not on
// the audio thread"): whoever constructs the voices initialises them with the
// same sample rate. setVoice may be called before or after init(), but not
// while process() is running.
void Engine::setVoice(int track, Voice* voice) {
  if (track < 0 || track >= kNumTracks) return;
  engine_detail::State& s = *impl_;
  s.voice[track] = voice;
  s.cacheVoiceRanges(track);
  if (voice && s.gValid) voice->setTempo(float(s.bpm));
  if (s.inited && s.proj) s.sanitizeTrackSound(track);
  s.tr[track].paramsSent = 0;
}

void Engine::initProject(Project& p) const {
  std::memset(&p, 0, sizeof(Project));
  p.magic[0] = 'P'; p.magic[1] = 'G'; p.magic[2] = 'R'; p.magic[3] = 'V';
  p.version = kProjectVersion;
  p.songMode = 0;
  const char name[] = "INIT";
  std::memcpy(p.name, name, sizeof(name));
  for (int i = 0; i < kGNumUsed; ++i) p.globals[i] = globalParamInfo(i)->def;
  for (int t = 0; t < kNumTracks; ++t) {
    for (int i = 0; i < kParamsPerTrack; ++i) {
      const ParamInfo* info = trackParamInfo(t, i);
      p.tracks[t].params[i] = info ? info->def : 0;
    }
  }
  for (int pt = 0; pt < kNumPatterns; ++pt) {
    Pattern& pat = p.patterns[pt];
    pat.length = 16;
    for (int t = 0; t < kNumTracks; ++t) {
      pat.tracks[t].length = 16;
      for (int s = 0; s < kMaxSteps; ++s) pat.tracks[t].steps[s] = kDefaultStep;
    }
  }
}

const ParamInfo* Engine::trackParamInfo(int track, int index) const {
  if (track < 0 || track >= kNumTracks || index < 0 || index >= kParamsPerTrack) return nullptr;
  if (index < kSoundParams) return impl_->voice[track] ? impl_->voice[track]->paramInfo(index) : nullptr;
  return engine_detail::mixParamInfo(track, index);
}

void Engine::process(int16_t* out, int frames) {
  engine_detail::State& s = *impl_;
  // Dekker-style handshake with suspend(): we raise inProcess, THEN look at
  // suspendReq; suspend() raises suspendReq, THEN waits for inProcess to drop.
  // With sequentially consistent atomics one side always sees the other.
  s.inProcess.store(true);
  const int quanta = frames / kQuantum;
  for (int q = 0; q < quanta; ++q, out += 2 * kQuantum) {
    if (!s.inited) {
      std::memset(out, 0, sizeof(int16_t) * 2 * kQuantum);
      continue;
    }
    if (s.suspendReq.load()) {
      // Suspended: the Project is off limits. Fade the last sound out over a
      // few ms from cached state only (no commands, no sequencer, no project
      // reads), then output silence.
      if (s.suspendFade > 0) {
        s.render(out);
        const int n = engine_detail::kSuspendFadeQuanta * kQuantum;
        int pos = (engine_detail::kSuspendFadeQuanta - s.suspendFade) * kQuantum;
        for (int i = 0; i < kQuantum; ++i, ++pos) {
          const int32_t g = ((n - 1 - pos) << 15) / n;
          out[2 * i] = int16_t((int32_t(out[2 * i]) * g) >> 15);
          out[2 * i + 1] = int16_t((int32_t(out[2 * i + 1]) * g) >> 15);
        }
        --s.suspendFade;
        s.publishStatus();
      } else {
        std::memset(out, 0, sizeof(int16_t) * 2 * kQuantum);
      }
      continue;
    }
    const uint32_t epoch = s.resumeEpoch.load(std::memory_order_relaxed);
    if (epoch != s.seenEpoch) {
      // First quantum after resume(): the Project memory may hold a different
      // project now. Start clean: transport stopped at the top, voices choked,
      // FX cleared, everything re-derived and re-sanitised.
      s.seenEpoch = epoch;
      for (int t = 0; t < kNumTracks; ++t) s.vChoke(t);
      s.resetRuntime();
      s.resetFx();
      s.sanitizeProject();
      s.gValid = false;
      s.bump();
    }
    s.suspendFade = engine_detail::kSuspendFadeQuanta;
    s.quantum(out);
  }
  const int rest = frames - quanta * kQuantum;
  if (rest > 0) std::memset(out, 0, sizeof(int16_t) * 2 * size_t(rest));
  s.inProcess.store(false);
}

bool Engine::post(const Command& c) {
  engine_detail::State& s = *impl_;
  const uint32_t t = s.qTail.load(std::memory_order_relaxed);
  const uint32_t h = s.qHead.load(std::memory_order_acquire);
  if (t - h >= uint32_t(engine_detail::kQueueSize)) return false;
  s.queue[t & (engine_detail::kQueueSize - 1)] = c;
  s.qTail.store(t + 1, std::memory_order_release);
  return true;
}

EngineStatus Engine::status() {
  engine_detail::State& s = *impl_;
  const std::memory_order rx = std::memory_order_relaxed;
  EngineStatus st;
  st.playing = s.aPlaying.load(rx);
  st.recording = s.aRecording.load(rx);
  st.pattern = s.aPattern.load(rx);
  st.queuedPattern = s.aQueued.load(rx);
  st.songSlot = s.aSongSlot.load(rx);
  for (int t = 0; t < kNumTracks; ++t) {
    st.step[t] = s.aStep[t].load(rx);
    st.trig[t] = s.aTrig[t].load(rx);
  }
  st.peakL = s.aPeakL.exchange(0.0f, rx);
  st.peakR = s.aPeakR.exchange(0.0f, rx);
  st.duckGain = s.aDuck.load(rx);
  st.revision = s.aRevision.load(rx);
  return st;
}

void Engine::suspend() {
  engine_detail::State& s = *impl_;
  s.suspendReq.store(true);
  // Wait for a process() call that may have started before the request to
  // finish. Returns immediately when the audio thread is not inside process()
  // (never started, stopped, or between callbacks): from then on every
  // process() call sees the request before it touches the Project.
  while (s.inProcess.load()) {
  }
}

void Engine::resume() {
  engine_detail::State& s = *impl_;
  s.resumeEpoch.fetch_add(1);
  s.suspendReq.store(false);
}

// ================================================================= hooks ====

namespace engine_detail {

void setRngSeed(uint32_t seed) {
  if (g_state) g_state->seedReq.store(seed ? seed : kDefaultSeed);
}
size_t staticRamBytes() { return g_stateBytes; }
size_t delayRamBytes() { return sizeof(Delay); }
size_t reverbRamBytes() { return sizeof(Reverb); }
int delayTimeSamples() { return g_state ? 2 * g_state->delayHalfSamples : 0; }
bool fxIdle() { return g_state ? !g_state->fxRunning : true; }
uint32_t faultCount() { return g_state ? g_state->faults + g_state->master.faults() : 0; }

}  // namespace engine_detail
}  // namespace pg
