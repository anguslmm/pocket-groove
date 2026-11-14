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

#include "../macro/macro.h"
#include "delay.h"
#include "dsp_util.h"
#include "ducker.h"
#include "engine_hooks.h"
#include "master.h"
#include "reverb.h"
#include "stem_tap.h"
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
constexpr int kMacroSlots = 2 * kTrackMacros;   // a base and a tilt offset per macro
constexpr int kOffSlot = kTrackMacros;          // slot of macro m's offset: kOffSlot + m
constexpr int kMaxAltVoices = 2;                // tracks x kinds hosted besides the native voices (PERC's and LEAD's samplers)

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
  uint8_t lockParam, lockValue;       // CLASSIC: active p-lock (lockParam = index + 1, 0 = none)
  uint8_t supValid, supPattern, supStep;   // one-shot "don't fire this step" after a recorded note
  uint8_t ratLeft, ratSlide;
  int32_t ratCd, ratInterval, ratGate;
  NoteEvent ratEv;
  PendNote pend[kMaxPend];
  uint8_t trig;
  // ---- roll (CmdType::Roll) ----
  uint8_t rollC;                      // 0 = off, else hits per step x2 (1..4)
  int64_t rollCd;                     // Q32 samples until the next roll hit
  int64_t rollInt;                    // Q32 samples between hits
  NoteEvent rollEv;
  // ---- live recording: the last recorded step, while a key that landed on it is held ----
  uint8_t recHeld, recPattern, recStep;
  uint32_t recStartQ;                 // quantum counter at its (first) note-on
  uint32_t liveHeld[4];               // live keys down (NoteOn without NoteOff yet), bit per MIDI note
  // ---- instrument (engine.h "INSTRUMENTS") ----
  const macro::MacroSet* inst;        // the bound set, nullptr = CLASSIC or unknown id
  uint16_t boundId;                   // the Project id it was bound from
  uint8_t bindValid;                  // 0: (re)bind at the next quantum
  uint8_t evPos[kTrackMacros];        // the positions the derived values came from
  uint8_t iMix[macro::kMixRows];      // the instrument's evaluated mixer rows (level .. duck)
  uint8_t lkSound[kSoundParams];      // with the macros standing away from the stored bytes (motion): the sound
  uint8_t lkMix[macro::kMixRows];     // params and mixer rows the 7-bit path plays there (at stand[])
  uint8_t stand[kTrackMacros];        // where the macros stand, rounded, when lkSound / lkMix are valid
  uint8_t standOn;                    // lkSound / lkMix are in use (stand[] differs from the stored bytes)
  uint8_t dMix[macro::kMixRows];      // mixer defaults, the reference of the mixer rule
  // ---- macro motion (engine.h "MACRO MOTION") ----
  // Trajectory slots: 0..3 = macro m's base (or motion), 4..7 = macro m's tilt offset
  // (engine.h "TILT OFFSET"; signed, 0 when unused). What is heard: macroPos().
  float mFrom[kMacroSlots], mTo[kMacroSlots];   // ramp in position units; mTo = where it is / goes
  uint32_t mStart[kMacroSlots];       // qCount when the ramp began
  uint32_t mDur[kMacroSlots];         // ramp length, quanta (a motion glide can be 128 steps at 60 BPM: 32 s)
  uint8_t moving;                     // bit s: slot s is on a ramp (bits 0..3 a base, 4..7 an offset)
  uint8_t fine;                       // the voice plays fractional values (fineTick), not eff[0..23]
  uint8_t anchor;                     // start an interval at the next quantum: 1 from the values now, 2 jump, 3 entering
  uint8_t settling;                   // the motion ended: the interval runs into the stored bytes
  // ---- motion (engine.h "MOTION"): the notes' own macro values ----
  uint8_t kfMask;                     // bit m: macro m has keyframes in the playing pattern (motion owns it)
  uint8_t live;                       // bit m: motion recording: the player's hand moves macro m (heard, written)
  uint8_t tiltLive;                   // bit m: live because the tilt gesture holds it (ends at its release)
  uint32_t liveUntil[kTrackMacros];   // qCount after which a still hand stops being live (one bar)
  // The values the voice hears move linearly from fA (at iStart) to fB (at iStart + iLen):
  // fB is the instrument evaluated where the macros will be at the interval's end, so
  // the set is evaluated once per interval (kFineEvalQuanta), the voice fed every tick.
  // Both in 1/256 units (8.8 fixed point: 0..127 with 0.4 cent of a cutoff step).
  uint16_t fA[kSoundParams + 3], fB[kSoundParams + 3];   // sound params, then level, delay, reverb send
  uint32_t iStart;
  uint16_t iLen;
  uint32_t lerp;                      // bit i: fA[i] != fB[i] (the values that move in this interval)
  float fMix[3];                      // fractional level, delay and reverb send now, for render()
  uint32_t fEnum, fStep;              // bit p: parameter p is an enum / only jumps (enum, semitones)
  uint8_t dDef[kParamsPerTrack];      // the values under a set (voice + mixer defaults)
  // ---- params ----
  uint8_t eff[kParamsPerTrack];       // effective params this quantum (p-lock applied)
  uint8_t mute;                       // cached Project mute
  uint8_t paramsSent;
  uint8_t pMin[kSoundParams], pMax[kSoundParams];   // voice param ranges (for sanitising)
  uint32_t pHas;                      // bit i: the voice has sound param i (all set when no voice)
  // ---- mixer ----
  float cLevel;                       // level the targets were computed from (-1 = none)
  int cPan;                           // pan the targets were computed from (-1 = none)
  float panC, panS;                   // cos / sin of the pan angle of cPan
  float tgtL, tgtR;                   // level * pan
  float gL, gR;                       // smoothed level * pan * mute
  float outL, outR;                   // gains at the end of the previous quantum (incl. duck)
  float sendD, sendR;                 // send gains at the end of the previous quantum
  uint64_t mixKey;                    // raw mixer bytes 24..28 + mute the gains above were computed from
  uint8_t settled;                    // gains reached their targets: nothing to recompute until mixKey or the ducker moves
  uint8_t hasSends;                   // a send gain is non-zero (valid while settled)
  // ---- live recording, the held step (cold: last, so the hot fields keep their offsets) ----
  int32_t recOffT;                    // the step's own (ideal) time relative to recStartQ's quantum, ticks
  uint32_t recKeys[4];                // the keys (MIDI notes) down that landed on it
};

}  // namespace

struct State {
  // ---- setup ----
  Project* proj;
  Voice* voice[kNumTracks];           // what renders each track now: its native voice or a hosted kind (VOICE KINDS)
  Voice* nativeV[kNumTracks];         // setVoice: each track's own voice
  struct AltVoice { Voice* v; uint8_t track, kind; };
  AltVoice alt[kMaxAltVoices];        // setAltVoice: other kinds a track hosts
  Voice* fadeV;                       // the voice a switch left, rendered on fadeTrack until its choke has faded
  uint8_t fadeTrack;
  float sr;
  bool inited;

  // ---- UI -> audio command queue (lock-free SPSC ring) ----
  Command queue[kQueueSize];
  std::atomic<uint32_t> qHead;        // consumer (audio thread)
  std::atomic<uint32_t> qTail;        // producer (UI thread)
  uint32_t qPeak;                     // UI thread: most commands waiting at once (Engine::queuePeak)

  // ---- audio -> UI status ----
  std::atomic<bool> aPlaying, aRecording;
  std::atomic<uint8_t> aPattern, aQueued, aSongSlot, aSongRep, aMasterStep, aWritesDone, aInstRefreshes;
  std::atomic<uint8_t> aStep[kNumTracks], aTrig[kNumTracks];
  std::atomic<float> aPeakL, aPeakR, aDuck;
  std::atomic<uint32_t> aRevision;
  std::atomic<uint32_t> aMacPos[kNumTracks];          // heard macro positions, rounded, byte m = macro m
  std::atomic<uint8_t> aMotion[kNumTracks], aLive[kNumTracks];

  // ---- suspend / resume ----
  std::atomic<bool> suspendReq, inProcess;
  std::atomic<uint32_t> resumeEpoch;
  std::atomic<uint32_t> resumeTail;   // queue tail when resume() was called: older commands are dropped
  uint32_t seenEpoch;
  bool suspendAck;                    // the audio thread has seen the current suspend request
  int suspendFade;                    // quanta of fade-out left
  std::atomic<uint32_t> seedReq;

  // ---- sequencer ----
  bool playing, recording;
  uint8_t curPattern, queued;
  uint8_t songSlot, songRep;
  bool songFresh;                     // song mode was just switched on: enter songStart at the next boundary
  uint8_t songStart;                  // slot the song starts from (SetSongMode a)
  uint8_t writesDone;                 // WriteTrack / WritePattern commands consumed
  uint8_t instRefreshes;              // RefreshInstruments commands consumed
  uint8_t fineTracks;                 // bit t: tr[t].fine (one compare per quantum when nothing moves)
  uint8_t motDirty;                   // bit t: re-aim track t's motion at the next quantum (edits, tempo, queue)
  bool motStart;                      // Play from the top: the first step sets the motions' start values
  bool boundaryNow;                   // fireMasterStep: this step began a pattern
  uint8_t pubMask;                    // bit t: publish track t's macro positions at the end of the quantum
  uint16_t rampQ;                     // kMacroRampMs in quanta
  uint16_t settleQ;                   // kMacroSettleMs in quanta
  float qPerMs;                       // quanta per millisecond (SetMacroOffset ramps)
  uint32_t qCount;                    // quanta processed (record timing)
  uint32_t absStep;                   // master steps fired since Play (roll phase lock)
  uint32_t barCount;                  // bar lines fired (master step % 16 == 0), never reset: the output tap
  std::atomic<OutputTap*> outTap;     // engine.h OUTPUT TAP; nullptr = none
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
  bool monoOut;                       // device mode: one bus, mono FX, L == R
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

  // ---- scope tap (engine.h "SCOPE TAP"): UI -> audio requests, audio -> UI ring ----
  std::atomic<uint16_t> scopeReq;     // UI: (track + 1) | decimation << 8; 0 = off
  std::atomic<uint16_t> scopeCfgA;    // audio: the request in effect (published after scopeSince)
  std::atomic<uint32_t> scopeEnd, scopeSince, scopeMark, scopeHits;
  std::atomic<uint8_t> lastNote[kNumTracks];   // each track's last note-on (0 = none yet), for the scope's window
  uint16_t scopeCfg;                  // audio thread: the request in effect
  uint8_t scopeTrack, scopeDecim;     // decoded: tapped track (0xFF none), keep 1 sample in scopeDecim
  int scopePhase;                     // samples summed into scopeAcc so far
  float scopeAcc, scopeInvD;          // the running sum of the current ring sample, 1 / scopeDecim
  uint32_t scopeWr, scopeHitCount;    // samples written / note-ons, the audio thread's copies
  float scopeRing[kScopeRing];

#if PG_STEM_TAP
  // ---- host-only offline rendering hooks (stem_tap.h) ----
  StemTap* tap;
  HalfbandUp tapUpL, tapUpR;          // upsample the delay-only return for the tap
  int endAfter;                       // pattern plays per Play before the sequencer holds (-1 = off)
  int playsLeft;                      // boundaries still allowed in this run
  bool seqHeld;
  long endedAt;
#endif

  // ================= methods =================
  void init(float sampleRate, Project* p);
  void resetRuntime();
  void resetFx();
  void sanitizeProject();
  void sanitizeTrackSound(int t);
  bool sanitizeStep(int track, bool inst, Step& s) const;
  bool instTrack(int t) const { return instrumentId(proj->tracks[t]) != 0; }
  void clearTrackExt(int t);
  bool sanitizeAll(Project& p) const;
  bool sanitizeTrack(Project& p, int t) const;
  void paramRange(int track, int idx, int& lo, int& hi) const;
  void cacheVoiceRanges(int t);
  void cacheSoundRanges(int t);
  // VOICE KINDS (voice.h): the voice of `kind` on track t (nullptr: the track hosts none).
  Voice* kindVoice(int t, int kind) const {
    if (kind == kVoiceNative) return nativeV[t];
    for (const AltVoice& a : alt)
      if (a.v && a.track == t && a.kind == kind) return a.v;
    return nullptr;
  }
  // Whether parameter idx of voice v (on track t) exists, and its range (no voice: every
  // sound slot exists with 0..255, as cacheVoiceRanges has it).
  bool voiceRange(const Voice* v, int t, int idx, int& lo, int& hi) const;
  void switchVoice(int t, Voice* want);

  void quantum(int16_t* out);
  void render(int16_t* out);
  void renderFading(Voice* v, float* L, float* R);
  void publishStatus();
  void drainCommands();
  void discardCommands(uint32_t upTo);
  void quiesceForSuspend();
  void apply(const Command& c);
  void refreshGlobals();
  void bump() { ++revision; }

  // sequencer
  void sequencerTick();
  void fireMasterStep();
  void fireTrackStep(int t, const Step& st, int stepIndex, int32_t frac, int32_t dur);
  void hit(int t, NoteEvent ev, int32_t offCd, bool armSlide);
  void primeNote(int t, const Step& st);
  const Step* primeSt = nullptr;      // the step whose note hit() is about to start (primeNote)
  void boundary();
  uint8_t decideNext(bool commit);
  int64_t swingOffFP(int idx) const { return (idx & 1) ? int64_t(stepLenFP * uint64_t(swing) / 100u) : 0; }
  int curPatternLen() const { return sanitizeLength(proj->patterns[curPattern].length); }
  void play(bool cont);
  void stop();
  void releaseNotes(int t);
  void liveNoteOn(const Command& c);
  void liveNoteOff(const Command& c);
  void recordNote(int t, const NoteEvent& ev);
  int recordGateCap(int t, int p) const;
  void setRecordedGate(int t, int64_t quarterSteps);
  void startRoll(int t, const Command& c);
  void rollTick();
  void writeTrack(int pattern, int track, const TrackPattern* src);
  void afterNoteOn(int t);
  void buildEff(int t);
  void ensureParams(int t);
  // instruments
  void trackInfo(const Voice* v, int t, const ParamInfo** info, uint8_t* defaults) const;
  void deriveInto(const macro::MacroSet& set, const Voice* v, int t, const uint8_t* positions, uint8_t* out) const;
  void evalInstrument(int t, int lockMacro, uint8_t lockPos, uint8_t* out);
  void refreshInstrument(int t, bool rebind);
  void updateStand(int t);
  // macro motion
  float macroPos(const TrackRt& r, int m, uint32_t q) const;
  // Where trajectory slot s is at quantum q: macro m's base (s = m) or its tilt
  // offset (s = kOffSlot + m). Inline everywhere: no function of its own on the audio thread.
  __attribute__((always_inline)) float slotPos(const TrackRt& r, int s, uint32_t q) const {
    if (!((r.moving >> s) & 1u)) return r.mTo[s];
    const uint32_t e = q - r.mStart[s];
    if (e >= r.mDur[s]) return r.mTo[s];
    return r.mFrom[s] + (r.mTo[s] - r.mFrom[s]) * (float(e) / float(r.mDur[s]));
  }
  void startRamp(int t, int m, uint8_t target, uint16_t quanta);
  void rampTo(int t, int m, float from, float to, uint32_t quanta);
  // motion
  uint8_t keyframeMask(int t, int pattern) const;
  void motionStep(int t, bool fresh);
  void aimMotion(int t, uint8_t which, uint32_t minQuanta);
  void refreshMotion();
  float loopStartValue(int t, int m) const;
  void liveStep(int t);
  void publishMotion(int t) {
    aMotion[t].store(tr[t].kfMask, std::memory_order_relaxed);
    aLive[t].store(tr[t].live, std::memory_order_relaxed);
  }
  void jumpMacro(int t, int m, float to);
  void resetMotion(int t);
  void fineValues(int t, uint32_t q, float* v);
  void bytesNow(int t, float* v);
  void fineEval(int t);
  bool fineInterval(int t, float* cur);
  void exitFine(int t);
  void fineTick();
  uint32_t rand32() {
    uint32_t x = rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    rng = x;
    return x;
  }
  void vOn(int t, const NoteEvent& e) {
    if (voice[t]) {
      // VOICE KINDS: a hosted kind (the sampler) learns the track's pool slot at each note.
      if (voice[t] != nativeV[t]) voice[t]->setSampleSlot(proj->tracks[t].sampleSlot);
      voice[t]->noteOn(e);
    }
    lastNote[t].store(e.note, std::memory_order_relaxed);   // the scope's time window for synths
    if (t == scopeTrack) {   // the scope's trigger: drums draw from here, synths from the attack
      scopeMark.store(scopeWr, std::memory_order_relaxed);
      scopeHits.store(++scopeHitCount, std::memory_order_release);
    }
  }
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
  suspendAck = false;
  resumeTail.store(qTail.load());
  seedReq.store(0);
  rng = kDefaultSeed;
  revision = 0;
  faults = 0;
  monoOut = false;
  writesDone = 0;
  instRefreshes = 0;
  qCount = 0;
  scopeCfg = 0;            // nothing tapped until the first quantum reads the request
  scopeCfgA.store(0);
  scopeTrack = 0xFF;
  scopeDecim = 1;
  for (int t = 0; t < kNumTracks; ++t) voice[t] = nativeV[t];   // a new start plays the native voices until bound
  fadeV = nullptr;
  fadeTrack = 0xFF;
#if PG_STEM_TAP
  seqHeld = false;
  endedAt = -1;
  playsLeft = -1;
#endif

  const float quantaPerSec = sr / float(kQuantum);
  gainK = 1.0f - expf(-1.0f / (0.004f * quantaPerSec));     // 4 ms; init is not real-time
  rampQ = uint16_t(float(kMacroRampMs) * 0.001f * quantaPerSec + 0.5f);
  settleQ = uint16_t(float(kMacroSettleMs) * 0.001f * quantaPerSec + 0.5f);
  qPerMs = quantaPerSec * 0.001f;
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
  songStart = 0;
  absStep = 0;
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
    r.rollC = 0;
    r.rollCd = r.rollInt = 0;
    r.recHeld = 0;
    std::memset(r.liveHeld, 0, sizeof(r.liveHeld));
    r.paramsSent = 0;
    r.mute = 0;
    r.cLevel = -1.0f;
    r.cPan = -1;
    r.panC = r.panS = 0.0f;
    r.tgtL = r.tgtR = 0.0f;
    r.gL = r.gR = 0.0f;
    r.outL = r.outR = 0.0f;
    r.sendD = r.sendR = 0.0f;
    r.mixKey = 0;
    r.settled = 0;
    r.hasSends = 0;
    std::memset(r.eff, 0, sizeof(r.eff));
    r.bindValid = 0;                  // a new project may be in place: bind at the next quantum
    r.inst = nullptr;
    r.standOn = 0;
    r.kfMask = r.live = r.tiltLive = 0;
    r.moving = 0;
    r.fine = r.anchor = r.settling = 0;
    for (int i = 0; i < kSoundParams + 3; ++i) r.fA[i] = r.fB[i] = 0;
    r.iStart = 0;
    r.iLen = 1;
    r.lerp = 0;
    r.fMix[0] = r.fMix[1] = r.fMix[2] = 0.0f;
    for (int m = 0; m < kMacroSlots; ++m) r.mFrom[m] = r.mTo[m] = 0.0f;
  }
  fineTracks = 0;
  fadeV = nullptr;
  motDirty = 0;
  motStart = false;
  boundaryNow = false;
  pubMask = 0xFF;
  for (int t = 0; t < kNumTracks; ++t) publishMotion(t);
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
#if PG_STEM_TAP
  tapUpL.reset(); tapUpR.reset();
#endif
  dSilent = rSilent = 1000;
  fxRunning = false;
}

// The current voice's (voice[t]) parameter ranges, masks and defaults.
void State::cacheSoundRanges(int t) {
  TrackRt& r = tr[t];
  r.pHas = voice[t] ? 0u : 0xFFFFFFFFu;
  r.fEnum = r.fStep = 0;
  for (int i = 0; i < kParamsPerTrack; ++i) {
    const ParamInfo* info = i < kSoundParams ? (voice[t] ? voice[t]->paramInfo(i) : nullptr) : mixParamInfo(t, i);
    if (i < kSoundParams) {
      r.pMin[i] = info ? info->min : 0;
      r.pMax[i] = info ? info->max : 255;
      if (info) r.pHas |= 1u << i;
    }
    r.dDef[i] = info ? info->def : 0;
    if (info && info->fmt == ParamFormat::Enum) r.fEnum |= 1u << i;
    if (info && (info->fmt == ParamFormat::Enum || info->fmt == ParamFormat::Semitones)) r.fStep |= 1u << i;
  }
}

bool State::voiceRange(const Voice* v, int t, int idx, int& lo, int& hi) const {
  lo = 0; hi = 255;
  const ParamInfo* info = idx < kSoundParams ? (v ? v->paramInfo(idx) : nullptr) : mixParamInfo(t, idx);
  if (info) { lo = info->min; hi = info->max; return true; }
  return idx < kSoundParams && !v;   // no voice: every sound slot, 0..255
}

void State::cacheVoiceRanges(int t) {
  TrackRt& r = tr[t];
  cacheSoundRanges(t);
  for (int i = 0; i < macro::kMixRows; ++i) {
    const ParamInfo* info = mixParamInfo(t, kMixLevel + i);
    r.dMix[i] = info ? info->def : 0;
    r.iMix[i] = r.lkMix[i] = r.dMix[i];
  }
  r.bindValid = 0;
}

void State::paramRange(int track, int idx, int& lo, int& hi) const {
  lo = 0; hi = 255;
  if (idx < 0) return;
  if (idx < kSoundParams) { lo = tr[track].pMin[idx]; hi = tr[track].pMax[idx]; return; }
  const ParamInfo* info = mixParamInfo(track, idx);
  if (info) { lo = info->min; hi = info->max; }
}

// Clamp one step to the documented field ranges. Returns true if it changed.
// DECISION: unknown flag bits are cleared, gate 0 becomes 1, a chord on a
// track other than the lead is dropped. CLASSIC track: a lock naming a param
// that does not exist (unused voice slot, reserved mixer slot) is removed, its
// value clamped, ext[2] zero. Instrument track (`inst`): the step's last three
// bytes are four macro codes, every value legal.
bool State::sanitizeStep(int track, bool inst, Step& s) const {
  Step o = s;
  if (s.note > 127) s.note = 127;
  if (s.vel < 1) s.vel = 1;
  if (s.vel > 127) s.vel = 127;
  if (s.gate < 1) s.gate = 1;
  if (s.prob() > 100) s.setProb(100);
  if (s.chord() >= kChordNum || track != kTrackLead) s.setChord(0);
  if (!inst) {
    const int lp = s.lockParam();
    if (lp == 0 || lp > kParamsPerTrack) {
      s.clearExt();
    } else {
      // (a CLASSIC track plays its native voice: the lock is that voice's parameter)
      const int idx = lp - 1;
      int lo, hi;
      if (!voiceRange(nativeV[track], track, idx, lo, hi)) {
        s.clearExt();
      } else {
        s.setLock(lp, clampi(s.lockValue(), lo, hi));
      }
    }
  }
  return std::memcmp(&o, &s, sizeof(Step)) != 0;
}

// One track's settings and its steps in every pattern.
bool State::sanitizeTrack(Project& p, int t) const {
  bool changed = false;
  TrackSettings& ts = p.tracks[t];
  // VOICE KINDS: the parameters are those of the voice the track's instrument plays
  // (an instrument of a kind the track does not host is as unknown: the native voice).
  const macro::MacroSet* set = macro::findInstrument(instrumentId(ts));
  if (set && (set->track != t || !kindVoice(t, set->voice))) set = nullptr;
  const Voice* v = set ? kindVoice(t, set->voice) : nativeV[t];
  uint32_t has = 0;
  for (int i = 0; i < kParamsPerTrack; ++i) {
    int lo, hi;
    const bool exists = voiceRange(v, t, i, lo, hi);
    if (exists && i < kSoundParams) has |= 1u << i;
    const uint8_t val = exists ? uint8_t(clampi(ts.params[i], lo, hi)) : 0;
    if (val != ts.params[i]) { ts.params[i] = val; changed = true; }
  }
  if (ts.mute > 1) { ts.mute = 1; changed = true; }
  if (ts.sampleSlot >= kSampleSlots) { ts.sampleSlot = 0; changed = true; }
  const bool inst = instrumentId(ts) != 0;
  // DECISION: an instrument track's sound parameters are derived values, so the
  // sanitiser derives them (a project saved with an older version of a set comes
  // back with what the set gives now; the engine then finds nothing to change).
  if (set) {
    uint8_t out[kParamsPerTrack];
    deriveInto(*set, v, t, ts.macro, out);
    for (int i = 0; i < kSoundParams; ++i)
      if (((has >> i) & 1u) && ts.params[i] != out[i]) { ts.params[i] = out[i]; changed = true; }
  }
  for (int pt = 0; pt < kNumPatterns; ++pt) {
    TrackPattern& tp = p.patterns[pt].tracks[t];
    const uint8_t len = uint8_t(sanitizeLength(tp.length));
    if (len != tp.length) { tp.length = len; changed = true; }
    for (int i = 0; i < kMaxSteps; ++i) changed |= sanitizeStep(t, inst, tp.steps[i]);
  }
  return changed;
}

// Everything. Shared by the public Engine::sanitizeProject and the engine's
// own checks at init()/resume().
bool State::sanitizeAll(Project& p) const {
  bool changed = false;
  const uint8_t magic[4] = {'P', 'G', 'R', 'V'};
  if (std::memcmp(p.magic, magic, 4) != 0) { std::memcpy(p.magic, magic, 4); changed = true; }
  if (p.version != kProjectVersion) {
    // DECISION: version 1 had no instruments (the bytes were reserved, zero): it loads
    // with none on any track (CLASSIC), whatever those bytes hold, so it sounds exactly
    // as it did. Any other foreign version is treated the same way. Versions 1 and 2
    // store steps in the old layout (StepV2): repacked here, losslessly (project.h).
    const bool v2 = p.version == 2;
    for (int t = 0; t < kNumTracks; ++t) {
      TrackSettings& ts = p.tracks[t];
      if (!v2) {
        std::memset(ts.instrument, 0, sizeof(ts.instrument));
        std::memset(ts.macro, 0, sizeof(ts.macro));
        ts.sampleSlot = 0;
      }
      const bool inst = instrumentId(ts) != 0;
      for (int pt = 0; pt < kNumPatterns; ++pt) {
        Step* steps = p.patterns[pt].tracks[t].steps;
        for (int i = 0; i < kMaxSteps; ++i) {
          StepV2 o;
          std::memcpy(&o, &steps[i], sizeof(o));
          steps[i] = repackStepV2(o, inst);
        }
      }
    }
    p.version = kProjectVersion;
    changed = true;
  }
  // Name: printable ASCII up to the first NUL, NUL padded after it, and
  // always NUL terminated.
  {
    bool end = false;
    for (int i = 0; i < int(sizeof(p.name)); ++i) {
      char c = p.name[i];
      if (i == int(sizeof(p.name)) - 1 || c == 0) end = true;
      const char want = end ? char(0) : ((c < 0x20 || c > 0x7E) ? '?' : c);
      if (want != c) { p.name[i] = want; changed = true; }
    }
  }
  if (p.songMode > 1) { p.songMode = 1; changed = true; }
  for (int i = 0; i < kNumGlobals; ++i) {
    const ParamInfo* info = Engine::globalParamInfo(i);
    const uint8_t v = info ? uint8_t(clampi(p.globals[i], info->min, info->max)) : 0;
    if (v != p.globals[i]) { p.globals[i] = v; changed = true; }
  }
  for (int i = 0; i < kSongSlots; ++i) {
    if (p.song[i].pattern >= kNumPatterns) { p.song[i].pattern = kNumPatterns - 1; changed = true; }
  }
  for (int pt = 0; pt < kNumPatterns; ++pt) {
    const uint8_t len = uint8_t(sanitizeLength(p.patterns[pt].length));
    if (len != p.patterns[pt].length) { p.patterns[pt].length = len; changed = true; }
  }
  for (int t = 0; t < kNumTracks; ++t) changed |= sanitizeTrack(p, t);
  return changed;
}

void State::sanitizeTrackSound(int t) {
  if (sanitizeTrack(*proj, t)) bump();
}

// The engine trusts the Project after this: every field it indexes with or
// maps to a coefficient is inside its documented range. Runs at init(), after
// resume() (a new project may have been loaded) and per command afterwards.
void State::sanitizeProject() {
  if (sanitizeAll(*proj)) bump();
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
    motDirty = 0xFF;
    const uint64_t srQ8 = uint64_t(sr * 256.0f + 0.5f);
    stepLenFP = ((srQ8 * 15u) << 24) / uint64_t(bpm);
    stepTicks = int32_t(stepLenFP >> 24);
    for (int t = 0; t < kNumTracks; ++t) {
      if (nativeV[t]) nativeV[t]->setTempo(float(bpm));
      if (tr[t].rollC) tr[t].rollInt = int64_t((stepLenFP * 2u) / tr[t].rollC);
    }
    for (const AltVoice& a : alt)
      if (a.v) a.v->setTempo(float(bpm));
  }
  if (gValid && v[kGSwing] != swing) motDirty = 0xFF;   // the notes' times moved: motion re-aims
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
    // The delay filters every echo once more than the previous one (see
    // delay.h); the original line filtered echoes in pairs. Per-pass corners
    // chosen so TWO passes have their -3 dB point where one pass used to
    // (one-pole x2: LP corner * 1/sqrt(sqrt(2) - 1), HP corner * sqrt(sqrt(2) - 1)):
    // the tail darkens at the same rate as before.
    delay.setParams(0.95f * float(v[kGDelayFeedback]) * (1.0f / 127.0f), onePoleCoef(1.5538f * lpHz, half),
                    onePoleCoef(0.6436f * hpHz, half), float(v[kGDelayWidth]) * (1.0f / 127.0f));
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
      case CmdType::ClearPattern: case CmdType::CopyPattern: case CmdType::WritePattern: budget -= 64; break;
      case CmdType::ClearTrack: case CmdType::CopyTrack: case CmdType::WriteTrack: budget -= 8; break;
      default: budget -= 1; break;
    }
    apply(c);
  }
  qHead.store(h, std::memory_order_release);
}

// Suspend/resume: drop queued commands without applying them (they were
// meant for the project that is being replaced). A dropped WriteTrack /
// WritePattern still counts in writesDone: the engine will never read its
// buffer, which is all the UI waits for. Audio thread only.
void State::discardCommands(uint32_t upTo) {
  uint32_t h = qHead.load(std::memory_order_relaxed);
  if (int32_t(upTo - h) <= 0) return;
  bool wrote = false;
  for (; h != upTo; ++h) {
    const CmdType ty = queue[h & (kQueueSize - 1)].type;
    if (ty == CmdType::WriteTrack || ty == CmdType::WritePattern) { ++writesDone; wrote = true; }
    if (ty == CmdType::RefreshInstruments) ++instRefreshes;
  }
  qHead.store(h, std::memory_order_release);
  if (wrote) aWritesDone.store(writesDone, std::memory_order_relaxed);
  aInstRefreshes.store(instRefreshes, std::memory_order_relaxed);
}

// First quantum that sees a suspend request: stop rolls and release held
// live / sequenced notes, so the fade-out and the resumed engine start clean.
// Touches no Project memory.
void State::quiesceForSuspend() {
  for (int t = 0; t < kNumTracks; ++t) {
    TrackRt& r = tr[t];
    r.rollC = 0;
    r.ratLeft = 0;
    r.recHeld = 0;
    for (int i = 0; i < kMaxPend; ++i)
      if (r.pend[i].active) { vOff(t, r.pend[i].note); r.pend[i].active = 0; }
    // Live keys still down: their NoteOff may be among the dropped commands.
    for (int w = 0; w < 4; ++w) {
      for (uint32_t m = r.liveHeld[w]; m; m &= m - 1) vOff(t, uint8_t(32 * w + __builtin_ctz(m)));
      r.liveHeld[w] = 0;
    }
  }
}

void State::apply(const Command& c) {
  switch (c.type) {
    case CmdType::Play: play(c.a == 1); break;
    case CmdType::Stop: stop(); break;
    case CmdType::SetStep: {
      if (c.a >= kNumPatterns || c.b >= kNumTracks || c.c >= kMaxSteps) break;
      Step s = c.step;
      sanitizeStep(c.b, instTrack(c.b), s);
      proj->patterns[c.a].tracks[c.b].steps[c.c] = s;
      motDirty = uint8_t(motDirty | (1u << c.b));
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
      motDirty = uint8_t(motDirty | (1u << c.b));
      bump();
      break;
    case CmdType::SetPatternLength:
      if (c.a >= kNumPatterns) break;
      proj->patterns[c.a].length = uint8_t(clampi(c.c, 1, kMaxSteps));
      motDirty = 0xFF;
      bump();
      break;
    case CmdType::SetMute:
      if (c.a >= kNumTracks) break;
      proj->tracks[c.a].mute = c.c ? 1 : 0;
      bump();
      break;
    case CmdType::SetSampleSlot:
      // takes effect at the track's next note (the sampler latches its slot there)
      if (c.a >= kNumTracks || c.c >= kSampleSlots) break;
      proj->tracks[c.a].sampleSlot = c.c;
      bump();
      break;
    case CmdType::NoteOn:
      if (c.a < kNumTracks) liveNoteOn(c);
      break;
    case CmdType::NoteOff:
      if (c.a < kNumTracks) liveNoteOff(c);
      break;
    case CmdType::QueuePattern:
      if (c.a >= kNumPatterns) break;
      if (playing) {
        queued = c.a;
        motDirty = 0xFF;   // what plays next changed: glides into it re-aim from where they are
      } else {
        // Stopped: switch immediately and park at the top of the pattern.
        curPattern = c.a;
        queued = kNoPattern;
        masterPos = masterNext = 0;
        for (int t = 0; t < kNumTracks; ++t) { tr[t].pos = tr[t].next = 0; tr[t].supValid = 0; }
      }
      break;
    case CmdType::SetSongMode:
      // DECISION: an out-of-range start slot means slot 0. The start slot is
      // remembered: Play (from the top) in song mode restarts from it too.
      proj->songMode = c.c ? 1 : 0;
      motDirty = 0xFF;
      if (proj->songMode) {
        songStart = c.a < kSongSlots ? c.a : 0;
        if (playing) {
          songFresh = true;         // enter songStart at the next pattern boundary
        } else {
          songSlot = songStart; songRep = 0; songFresh = false;
          if (proj->song[songStart].repeats) {
            curPattern = proj->song[songStart].pattern;
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
      motDirty = 0xFF;
      bump();
      break;
    case CmdType::SetRecord:
      recording = c.c != 0;
      if (!recording)
        for (int t = 0; t < kNumTracks; ++t) {
          tr[t].recHeld = 0;
          tr[t].tiltLive = 0;
          if (tr[t].live) {   // the hands let go: the recorded motion plays
            tr[t].live = 0;
            motDirty = uint8_t(motDirty | (1u << t));
            publishMotion(t);
          }
        }
      break;
    case CmdType::ClearTrack: {
      // DECISION: clears the steps, keeps the track's length.
      if (c.a >= kNumPatterns || c.b >= kNumTracks) break;
      TrackPattern& tp = proj->patterns[c.a].tracks[c.b];
      for (int s = 0; s < kMaxSteps; ++s) tp.steps[s] = kDefaultStep;
      motDirty = uint8_t(motDirty | (1u << c.b));
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
      motDirty = 0xFF;
      bump();
      break;
    }
    case CmdType::CopyPattern:
      if (c.a >= kNumPatterns || c.b >= kNumPatterns || c.a == c.b) break;
      std::memcpy(&proj->patterns[c.b], &proj->patterns[c.a], sizeof(Pattern));
      motDirty = 0xFF;
      bump();
      break;
    case CmdType::CopyTrack: {
      // DECISION ("same type only"): drum -> drum and synth -> synth are allowed,
      // drum <-> synth is ignored. Bass <- lead drops the chord field; CLASSIC
      // p-locks on sound params are dropped when the voices differ (different
      // meaning), and so are a note's own macro values (another instrument's
      // macros) and anything that does not fit the destination's track kind.
      if (c.a >= kNumPatterns || c.b >= kNumTracks || c.c >= kNumTracks || c.b == c.c) break;
      if (isSynthTrack(c.b) != isSynthTrack(c.c)) break;
      TrackPattern& dst = proj->patterns[c.a].tracks[c.c];
      std::memcpy(&dst, &proj->patterns[c.a].tracks[c.b], sizeof(TrackPattern));
      const bool srcInst = instTrack(c.b), dstInst = instTrack(c.c);
      for (int s = 0; s < kMaxSteps; ++s) {
        Step& st = dst.steps[s];
        if (c.c != kTrackLead) st.setChord(0);
        if (srcInst || dstInst || (st.lockParam() && st.lockParam() <= kSoundParams)) st.clearExt();
        sanitizeStep(c.c, dstInst, st);
      }
      motDirty = uint8_t(motDirty | (1u << c.c));
      bump();
      break;
    }
    case CmdType::Roll:
      if (c.a < kNumTracks && c.c <= 4) startRoll(c.a, c);
      break;
    case CmdType::SetInstrument: {
      if (c.a >= kNumTracks) break;
      TrackSettings& ts = proj->tracks[c.a];
      const uint16_t id = uint16_t(c.step.ext[0] | (c.step.ext[1] << 8));
      // The steps' last bytes are a lock on a CLASSIC track and macro values on an
      // instrument track: a change of kind clears them (project.h Step).
      if ((instrumentId(ts) != 0) != (id != 0)) clearTrackExt(c.a);
      setInstrumentId(ts, id);
      ts.macro[0] = c.step.note;
      ts.macro[1] = c.step.flagsRaw;
      ts.macro[2] = c.step.vel;
      ts.macro[3] = c.step.gate;
      bump();
      refreshInstrument(c.a, true);   // now, so later commands of this drain see it
      tr[c.a].kfMask = 0;             // (refreshInstrument jumped every macro to its byte)
      tr[c.a].live = 0;
      motDirty = uint8_t(motDirty | (1u << c.a));
      break;
    }
    case CmdType::SetMacro:
      if (c.a >= kNumTracks || c.b >= kTrackMacros) break;
      if (proj->tracks[c.a].macro[c.b] != c.c) {
        proj->tracks[c.a].macro[c.b] = c.c;
        bump();
      }
      {
        // MOTION: with record on and the transport running the player's hand is live
        // (heard, and written into the notes it passes); otherwise a macro whose motion
        // the playing pattern owns only gets a new base (heard once the motion ends;
        // a glide to the base re-aims).
        TrackRt& r = tr[c.a];
        const uint8_t bit = uint8_t(1u << c.b);
        if (playing && recording && r.inst && c.b < r.inst->numMacros) {
          r.live = uint8_t(r.live | bit);
          r.liveUntil[c.b] = qCount + uint32_t((uint64_t(stepLenFP) * 16u) / uint64_t(kQuantumFP));   // a bar
          publishMotion(c.a);
        }
        if ((r.kfMask & bit) && !(r.live & bit)) {
          motDirty = uint8_t(motDirty | (1u << c.a));
          break;
        }
      }
      // The stored parameters are derived after the drain (quantum()), once for any
      // number of moves; the voice follows the macro's trajectory (MACRO MOTION).
      startRamp(c.a, c.b, c.c, uint16_t(c.step.ext[0] | (c.step.ext[1] << 8)));
      break;
    case CmdType::SetMacroOffset: {
      // engine.h "TILT OFFSET". Never touches the Project.
      const int m = c.b & 3;
      if (c.a >= kNumTracks) break;
      TrackRt& r = tr[c.a];
      const uint8_t bit = uint8_t(1u << m);
      const bool hold = (c.b & kOffsetHold) != 0;
      if (hold && playing && recording && r.inst && m < r.inst->numMacros) {
        if (!(r.live & bit)) {
          // The hand takes the macro: it stays where it is now (a motion glide stops
          // there), the notes it passes take what is heard. No key bar is pending.
          const float here = slotPos(r, m, qCount);
          rampTo(c.a, m, here, here, 0);
          r.live = uint8_t(r.live | bit);
          r.liveUntil[m] = qCount;
          publishMotion(c.a);
        }
        r.tiltLive = uint8_t(r.tiltLive | bit);
      } else if (!hold && (r.tiltLive & bit)) {
        // Released: live ends here, unless a key moved the macro within the last bar.
        r.tiltLive = uint8_t(r.tiltLive & ~bit);
        if (int32_t(qCount - r.liveUntil[m]) >= 0) {
          r.live = uint8_t(r.live & ~bit);
          motDirty = uint8_t(motDirty | (1u << c.a));
          publishMotion(c.a);
        }
      }
      const int16_t raw = int16_t(uint16_t(c.step.note | (c.step.vel << 8)));
      // CLASSIC, or a macro the set lacks: nothing to offset (it stays 0)
      const bool has = r.inst && m < r.inst->numMacros;
      const float to = has ? clampf(float(raw) * (1.0f / float(kOffsetScale)), -255.0f, 255.0f) : 0.0f;
      const uint32_t ms = uint32_t(c.step.ext[0] | (c.step.ext[1] << 8));
      const uint32_t q = uint32_t(float(ms) * qPerMs + 0.5f);
      rampTo(c.a, kOffSlot + m, slotPos(r, kOffSlot + m, qCount), to, q);
      break;
    }
    case CmdType::RefreshInstruments:
      for (int t = 0; t < kNumTracks; ++t)
        if (c.a == 0xFF || c.a == t) {
          refreshInstrument(t, true);
          tr[t].kfMask = 0;
          motDirty = uint8_t(motDirty | (1u << t));
        }
      ++instRefreshes;
      aInstRefreshes.store(instRefreshes, std::memory_order_relaxed);
      break;
    case CmdType::WriteTrack:
      // DECISION: writesDone counts every Write command the engine has
      // CONSUMED (applied, or rejected for a bad address): either way the
      // engine will never read the buffer again, which is what the UI waits for.
      if (c.ptr && c.a < kNumPatterns && c.b < kNumTracks) {
        writeTrack(c.a, c.b, static_cast<const TrackPattern*>(c.ptr));
        motDirty = uint8_t(motDirty | (1u << c.b));
        bump();
      }
      ++writesDone;
      break;
    case CmdType::WritePattern:
      if (c.ptr && c.a < kNumPatterns) {
        const Pattern* src = static_cast<const Pattern*>(c.ptr);
        Pattern& dst = proj->patterns[c.a];
        dst.length = uint8_t(sanitizeLength(src->length));
        std::memcpy(dst.reserved, src->reserved, sizeof(dst.reserved));
        for (int t = 0; t < kNumTracks; ++t) writeTrack(c.a, t, &src->tracks[t]);
        motDirty = 0xFF;
        bump();
      }
      ++writesDone;
      break;
  }
}

void State::writeTrack(int pattern, int track, const TrackPattern* src) {
  TrackPattern& dst = proj->patterns[pattern].tracks[track];
  std::memcpy(&dst, src, sizeof(TrackPattern));
  dst.length = uint8_t(sanitizeLength(dst.length));
  const bool inst = instTrack(track);
  for (int s = 0; s < kMaxSteps; ++s) sanitizeStep(track, inst, dst.steps[s]);
}

// Every step of track t in every pattern loses its lock / macro values.
void State::clearTrackExt(int t) {
  for (int pt = 0; pt < kNumPatterns; ++pt)
    for (int s = 0; s < kMaxSteps; ++s) proj->patterns[pt].tracks[t].steps[s].clearExt();
}

// ---------------------------------------------------------------- roll ----

// DECISION (Roll): the rolling track's own sequenced steps are silenced while
// the roll is held (note repeat replaces the pattern; the positions and the
// song keep running), a muted track rolls silently, and the roll survives
// Play/Stop: it re-locks to the grid when the transport starts and runs free
// when it stops. c > 4 is out of range and ignored. Hits use the straight
// (unswung) grid. Synth notes are gated for half the hit interval.
void State::startRoll(int t, const Command& c) {
  TrackRt& r = tr[t];
  if (c.c == 0) {
    if (r.rollC) {
      r.rollC = 0;
      if (!playing) releaseNotes(t);
    }
    return;
  }
  const bool wasRolling = r.rollC != 0;
  r.rollC = c.c;
  r.rollInt = int64_t((stepLenFP * 2u) / c.c);
  NoteEvent ev;
  ev.note = c.step.note > 127 ? 127 : c.step.note;
  ev.vel = uint8_t(clampi(c.step.vel, 1, 127));
  ev.accent = (c.step.flags() & kStepAccent) != 0;
  ev.slide = false;
  ev.chord = (t == kTrackLead && c.step.chord() < kChordNum) ? c.step.chord() : 0;
  r.rollEv = ev;
  if (wasRolling && !playing) return;     // rate change while free-running: keep the phase
  if (!playing) {
    r.rollCd = 0;                         // free-running: first hit now
    return;
  }
  // Locked to the transport: the next point of the roll grid. The grid
  // repeats every step (even c) or every two steps (odd c, starting on an
  // even step since Play).
  int64_t ref = gridCd;                   // time of the next straight grid line (may be just past)
  if ((c.c & 1) && (absStep & 1)) ref += int64_t(stepLenFP);
  int64_t ph = ref % r.rollInt;
  if (ph < -kHalfQuantumFP) ph += r.rollInt;
  r.rollCd = ph;
}

// Once per quantum: fires due roll hits and advances the roll clocks. While
// playing it runs inside sequencerTick (before pending note-offs are advanced);
// while stopped it is called on its own and also runs the rolling tracks'
// note-offs, which the sequencer normally does.
void State::rollTick() {
  for (int t = 0; t < kNumTracks; ++t) {
    TrackRt& r = tr[t];
    if (!playing) {
      for (int i = 0; i < kMaxPend; ++i) {
        PendNote& p = r.pend[i];
        if (p.active && p.cd < kDueTicks) { vOff(t, p.note); p.active = 0; }
      }
    }
    if (r.rollC) {
      int guard = 0;
      while (r.rollCd < kHalfQuantumFP && guard++ < 8) {
        if (!proj->tracks[t].mute) {
          const int32_t frac = int32_t(r.rollCd >> 24);
          int32_t gate = int32_t(r.rollInt >> 25);
          if (gate < kQuantumTicks) gate = kQuantumTicks;
          hit(t, r.rollEv, frac + gate, false);
        }
        r.rollCd += r.rollInt;
      }
      r.rollCd -= kQuantumFP;
    }
    if (!playing) {
      for (int i = 0; i < kMaxPend; ++i)
        if (r.pend[i].active && r.pend[i].cd > -(1 << 30)) r.pend[i].cd -= kQuantumTicks;
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
    if (!playing) {
      playing = true;
      gridCd = 0;
      motDirty = 0xFF;   // motion picks up from where the macros are
      for (int t = 0; t < kNumTracks; ++t)
        if (tr[t].rollC) tr[t].rollCd = (tr[t].rollC & 1) && (absStep & 1) ? int64_t(stepLenFP) : 0;
    }
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
    songSlot = songStart; songRep = 0; songFresh = false;
    if (proj->song[songStart].repeats) curPattern = proj->song[songStart].pattern;
  }
  masterPos = masterNext = 0;
  gridCd = 0;
  sinceFire = 0;
  absStep = 0;
  playing = true;
  motStart = true;     // the first step sets every motion's start value (motionStep)
  motDirty = 0;
#if PG_STEM_TAP
  playsLeft = endAfter > 0 ? endAfter - 1 : -1;
  seqHeld = false;
  endedAt = -1;
#endif
  for (int t = 0; t < kNumTracks; ++t)
    if (tr[t].rollC) tr[t].rollCd = 0;   // a held roll locks to the downbeat
}

// DECISION: Stop sends noteOff for every sequenced note that is still gated and
// then choke()s every voice (voice.h names transport stop as a choke case), so
// stop is tight; the send FX are untouched and ring out. P-locks are dropped so
// the voices return to the params the UI shows.
// DECISION (motion at Stop): the macros rest where the motion had them (a tail
// or a live note sounds as the pattern left it); the motion lets go of them, so
// the next move of a macro glides from there to its new base.
void State::stop() {
  playing = false;
  motStart = false;
  motDirty = 0;
  for (int t = 0; t < kNumTracks; ++t) {
    TrackRt& r = tr[t];
    for (uint8_t b = r.kfMask; b; b = uint8_t(b & (b - 1))) {
      const int m = __builtin_ctz(b);
      jumpMacro(t, m, slotPos(r, m, qCount));
    }
    r.kfMask = 0;
    r.live = r.tiltLive = 0;
    publishMotion(t);
    pubMask = uint8_t(pubMask | (1u << t));
    tr[t].recHeld = 0;
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
  if (!r.inst) {
    if (r.lockParam && r.lockParam <= kParamsPerTrack) r.eff[r.lockParam - 1] = r.lockValue;   // CLASSIC
  } else {
    // An instrument: params[0..23] are derived (or, with the macros standing away from
    // the stored bytes, the evaluation where they stand); no locks; the player's mixer
    // values combine with the instrument's level and sends.
    if (r.standOn) std::memcpy(r.eff, r.lkSound, kSoundParams);
    macro::combineMixerRows(r.eff, r.standOn ? r.lkMix : r.iMix, r.dMix, r.eff);
  }
  r.mute = ts.mute;
}

// ---------------------------------------------------------- instruments ----

// The track's parameter metadata and its defaults (the values under a set).
void State::trackInfo(const Voice* v, int t, const ParamInfo** info, uint8_t* defaults) const {
  for (int i = 0; i < kParamsPerTrack; ++i) {
    info[i] = i < kSoundParams ? (v ? v->paramInfo(i) : nullptr) : mixParamInfo(t, i);
    defaults[i] = info[i] ? info[i]->def : 0;
  }
}

// DECISION (evaluation site): the audio thread evaluates, at a quantum edge, only
// when the bound id or a position changed (commands, or a Project changed under
// the engine) and at a step whose macro lock differs from the one in scope. The
// values under the set are the voice's defaults, so an instrument sounds the
// same whatever the track held before. Not on the per-sample or per-quantum path.
void State::deriveInto(const macro::MacroSet& set, const Voice* v, int t, const uint8_t* positions,
                       uint8_t* out) const {
  const ParamInfo* info[kParamsPerTrack];
  uint8_t under[kParamsPerTrack];
  trackInfo(v, t, info, under);
  float pos[kTrackMacros];
  for (int m = 0; m < kTrackMacros; ++m) pos[m] = macro::posFromByte(positions[m]);
  macro::evaluate(set, info, pos, under, out);
}

void State::evalInstrument(int t, int lockMacro, uint8_t lockPos, uint8_t* out) {
  uint8_t pos[kTrackMacros];
  std::memcpy(pos, proj->tracks[t].macro, kTrackMacros);
  if (lockMacro >= 0 && lockMacro < kTrackMacros) pos[lockMacro] = lockPos;
  deriveInto(*tr[t].inst, voice[t], t, pos, out);
}

// DECISION (the switch): at the quantum edge where the instrument changes kind, the
// notes the old voice holds are released, the old voice is choked (its ~2 ms fade)
// and keeps rendering on the track until it has faded (fadeV), and the new voice
// renders from this quantum on with the parameters derived for it. The parameter
// caches follow the voice that renders.
void State::switchVoice(int t, Voice* want) {
  releaseNotes(t);                                // note-offs to the voice that held them
  Voice* old = voice[t];
  if (old && old != want && old->active()) {
    old->choke();
    fadeV = old;
    fadeTrack = uint8_t(t);
  }
  if (fadeV == want) fadeV = nullptr;             // switched straight back: it plays on as the track's voice
  voice[t] = want;
  if (want && gValid) want->setTempo(float(bpm));
  cacheSoundRanges(t);
  tr[t].paramsSent = 0;
}

void State::refreshInstrument(int t, bool rebind) {
  TrackRt& r = tr[t];
  TrackSettings& ts = proj->tracks[t];
  const uint16_t id = instrumentId(ts);
  if (rebind || !r.bindValid || id != r.boundId) {
    const macro::MacroSet* s = macro::findInstrument(id);
    r.inst = (s && s->track == t) ? s : nullptr;   // a set for another voice is as unknown
    if (r.inst && !kindVoice(t, r.inst->voice)) r.inst = nullptr;   // so is a kind this track does not host
    r.boundId = id;
    r.bindValid = 1;
    resetMotion(t);                                // a new instrument jumps: nothing moves
    // VOICE KINDS: the instrument's kind renders the track from this quantum on.
    Voice* want = kindVoice(t, r.inst ? int(r.inst->voice) : int(kVoiceNative));
    if (want != voice[t]) {
      const bool fromOther = voice[t] != nativeV[t];
      switchVoice(t, want);
      // Back on the native voice without an instrument (CLASSIC, unknown): the stored
      // sound parameters were the other kind's; the native voice starts from its defaults.
      if (fromOther && !r.inst) {
        bool changed = false;
        for (int i = 0; i < kSoundParams; ++i) {
          const uint8_t d = ((r.pHas >> i) & 1u) ? r.dDef[i] : 0;
          if (ts.params[i] != d) { ts.params[i] = d; changed = true; }
        }
        if (changed) bump();
      }
    }
  } else {
    // Positions changed under the engine by another route than SetMacro: they jump
    // (not a macro the playing pattern's motion owns: that one only has a new base).
    for (int m = 0; m < kTrackMacros; ++m)
      if (!((r.kfMask >> m) & 1u) && float(ts.macro[m]) != r.mTo[m]) jumpMacro(t, m, float(ts.macro[m]));
  }
  std::memcpy(r.evPos, ts.macro, kTrackMacros);
  r.standOn = 0;
  r.paramsSent = 0;
  if (!r.inst) return;   // CLASSIC or unknown: the stored parameters play
  uint8_t out[kParamsPerTrack];
  evalInstrument(t, -1, 0, out);
  bool changed = false;
  for (int i = 0; i < kSoundParams; ++i) {
    // unused voice slots are 0 (sanitised; after a switch of voice kind they may hold the other kind's)
    const uint8_t v = ((r.pHas >> i) & 1u) ? out[i] : 0;
    if (ts.params[i] != v) { ts.params[i] = v; changed = true; }
  }
  for (int i = 0; i < macro::kMixRows; ++i) r.iMix[i] = out[kMixLevel + i];
  if (changed) bump();
  updateStand(t);
}

// The 7-bit path where the macros stand (no macro on a ramp): the stored values
// when they stand on the stored bytes; otherwise (a motion holds them elsewhere, or
// they rest there after Stop) the instrument evaluated there, once per change.
void State::updateStand(int t) {
  TrackRt& r = tr[t];
  if (r.moving) return;   // the fractional path plays; asked again when the last ramp ends
  const uint8_t* base = proj->tracks[t].macro;
  uint8_t want[kTrackMacros];
  bool diff = false;
  for (int m = 0; m < kTrackMacros; ++m) {   // where they stand, with a tilt offset held still
    want[m] = uint8_t(clampi(int(clampf(r.mTo[m] + r.mTo[kOffSlot + m], 0.0f, 255.0f) + 0.5f), 0, 255));
    diff |= want[m] != base[m];
  }
  if (!r.inst || !diff) {
    if (r.standOn) { r.standOn = 0; r.paramsSent = 0; }
    return;
  }
  if (r.standOn && std::memcmp(want, r.stand, kTrackMacros) == 0) return;
  std::memcpy(r.stand, want, kTrackMacros);
  uint8_t out[kParamsPerTrack];
  deriveInto(*r.inst, voice[t], t, want, out);
  for (int i = 0; i < kSoundParams; ++i) r.lkSound[i] = ((r.pHas >> i) & 1u) ? out[i] : 0;
  for (int i = 0; i < macro::kMixRows; ++i) r.lkMix[i] = out[kMixLevel + i];
  r.standOn = 1;
  r.paramsSent = 0;
}

void State::ensureParams(int t) {
  TrackRt& r = tr[t];
  if (r.paramsSent) return;
  buildEff(t);
  // While a macro moves the voice is fed fractional values by fineEval instead.
  if (voice[t] && !r.fine) voice[t]->setParams(r.eff);
  r.paramsSent = 1;
}

// ---------------------------------------------------------- macro motion ----
// engine.h "MACRO MOTION". Positions are in byte units (0..255) as floats.
//
// DECISION (control rate): while a track is in the fractional path its voice is
// fed every kFineQuanta quanta (1.45 ms at 44.1 kHz; the bass's own 1.2 ms
// cutoff lag and the lead's 5 ms smoothing join those updates into a curve) with
// values that move in a straight line across intervals of kFineEvalQuanta
// (11.6 ms): at the start of each interval the set is evaluated once, where the
// macros will be at its end (the ramps are known, so this is no lag), and the
// values run there from where they are. One evaluation per 11.6 ms per moving
// track; a new target joins at the next interval (it only changes the speed).

// Where macro m of the track is HEARD at quantum q: its base or motion (on its ramp,
// or where it stands) plus its tilt offset, within 0..255. Without an offset exactly
// the base (one more compare).
float State::macroPos(const TrackRt& r, int m, uint32_t q) const {
  const float base = slotPos(r, m, q);
  const float off = slotPos(r, kOffSlot + m, q);
  if (off == 0.0f) return base;
  return clampf(base + off, 0.0f, 255.0f);
}

// Slot m (a macro's base 0..3, or kOffSlot + the macro for its offset) jumps to `to`.
void State::jumpMacro(int t, int m, float to) {
  TrackRt& r = tr[t];
  r.mFrom[m] = r.mTo[m] = to;
  r.moving = uint8_t(r.moving & ~(1u << m));
  pubMask = uint8_t(pubMask | (1u << t));
  if (r.fine) r.anchor = 2;   // the values jump there at the next quantum
  else updateStand(t);
}

// Every macro stands at its stored position and the track plays the 7-bit path.
void State::resetMotion(int t) {
  TrackRt& r = tr[t];
  for (int m = 0; m < kTrackMacros; ++m) r.mFrom[m] = r.mTo[m] = float(proj->tracks[t].macro[m]);
  for (int m = kOffSlot; m < kMacroSlots; ++m) r.mFrom[m] = r.mTo[m] = 0.0f;   // a tilt offset ends too
  r.moving = 0;
  pubMask = uint8_t(pubMask | (1u << t));
  if (r.fine) exitFine(t);
}

// DECISION (ramp): a SetMacro moves the macro from wherever it is now (on a ramp
// or standing) to the target in a straight line over `quanta` (0 = kMacroRampMs).
// The default is longer than the longest gap between two key repeats on the
// device (repeats every 40 ms, run in 33 ms UI frames: 33 or 66 ms apart), so a
// held key never lets the macro stop between repeats. A ramp of 1, a track
// without a bound instrument, or a macro the set does not have: jump.
void State::startRamp(int t, int m, uint8_t target, uint16_t quanta) {
  TrackRt& r = tr[t];
  rampTo(t, m, slotPos(r, m, qCount), float(target), quanta == 0 ? rampQ : quanta);
}

// A straight line from `from` (where the macro is now, or a note's exact value) to
// `to` in `quanta`; 1 or less: a jump. m is a trajectory slot (a base, or kOffSlot +
// the macro for its tilt offset).
void State::rampTo(int t, int m, float from, float to, uint32_t quanta) {
  TrackRt& r = tr[t];
  if (!r.inst || (m & (kTrackMacros - 1)) >= r.inst->numMacros || quanta <= kMacroJump) {
    jumpMacro(t, m, to);
    return;
  }
  pubMask = uint8_t(pubMask | (1u << t));
  if (from == to) {   // already there: it stops
    r.mFrom[m] = r.mTo[m] = to;
    r.moving = uint8_t(r.moving & ~(1u << m));
    if (!r.fine) updateStand(t);
    return;
  }
  r.mFrom[m] = from;
  r.mTo[m] = to;
  r.mStart[m] = qCount;
  r.mDur[m] = quanta;
  r.moving = uint8_t(r.moving | (1u << m));
  if (!r.fine) {
    // Into the fractional path, starting from the bytes the voice was playing (r.eff,
    // as last built): the first interval runs from them, so nothing jumps.
    for (int i = 0; i < kSoundParams; ++i) r.fA[i] = uint16_t(((r.pHas >> i) & 1u) ? r.eff[i] << 8 : 0);
    r.fA[kSoundParams + 0] = uint16_t(r.eff[kMixLevel] << 8);
    r.fA[kSoundParams + 1] = uint16_t(r.eff[kMixDelaySend] << 8);
    r.fA[kSoundParams + 2] = uint16_t(r.eff[kMixReverbSend] << 8);
    std::memcpy(r.fB, r.fA, sizeof(r.fA));
    r.lerp = 0;
    r.iStart = qCount;
    r.iLen = 1;
    r.lerp = 0;
    r.fMix[0] = r.fMix[1] = r.fMix[2] = -1.0f;   // render() takes the first values
    r.fine = 1;
    r.settling = 0;
    r.anchor = 3;
    fineTracks = uint8_t(fineTracks | (1u << t));
  } else if (r.settling) {
    r.settling = 0;   // back into motion: a new interval from where the values are
    if (!r.anchor) r.anchor = 1;
  }
  // Otherwise (a new target while moving) the interval under way runs on; the next
  // one follows the new ramp.
}

// The instrument at the macros' positions at quantum q (a macro lock in scope
// wins), unrounded: v[0..23] the sound params, v[24..26] the combined level,
// delay and reverb send.
void State::fineValues(int t, uint32_t q, float* v) {
  TrackRt& r = tr[t];
  float pos[kTrackMacros];
  for (int m = 0; m < kTrackMacros; ++m) pos[m] = macroPos(r, m, q) * (1.0f / float(macro::kPosMax));
  macro::FineRanges fr;
  fr.enumMask = r.fEnum;
  fr.stepMask = r.fStep;
  fr.lo = r.pMin;
  fr.hi = r.pMax;
  float all[kParamsPerTrack];
  macro::evaluateFine(*r.inst, fr, pos, r.dDef, all);
  for (int i = 0; i < kSoundParams; ++i) v[i] = ((r.pHas >> i) & 1u) ? all[i] : 0.0f;
  // The player's mixer rows (instrument tracks have no locks).
  macro::combineMixerFine(proj->tracks[t].params + kMixLevel, all + kMixLevel, r.dMix, v + kSoundParams);
}

// What the 7-bit path would play now, as floats in fineValues' layout.
void State::bytesNow(int t, float* v) {
  TrackRt& r = tr[t];
  buildEff(t);
  for (int i = 0; i < kSoundParams; ++i) v[i] = ((r.pHas >> i) & 1u) ? float(r.eff[i]) : 0.0f;
  v[kSoundParams + 0] = float(r.eff[kMixLevel]);
  v[kSoundParams + 1] = float(r.eff[kMixDelaySend]);
  v[kSoundParams + 2] = float(r.eff[kMixReverbSend]);
}

void State::exitFine(int t) {
  TrackRt& r = tr[t];
  r.fine = r.anchor = r.settling = 0;
  r.moving = 0;
  r.paramsSent = 0;      // the 7-bit values go out at this quantum's ensureParams
  r.settled = 0;         // and the mixer recomputes its gains from them
  fineTracks = uint8_t(fineTracks & ~(1u << t));
}

// One control tick of a track in the fractional path: the values now; when the
// interval has run out (or after a start, a jump, a lock) a new one from them, to
// the instrument where the macros will be at its end or, once no macro moves, to
// the stored bytes over kMacroSettleMs (after which the track leaves the
// fractional path); then the values to the voice and the mixer.
// DECISION (enums, semitones): they do not glide between two whole values; they
// switch to the next interval's value when it begins (so at most one interval,
// 11.6 ms, from the position where the set switches them).
// DECISION (drums): a drum voice is fed every kFineEvalQuanta (11.6 ms), not every
// tick: its continuous parameters move in 11.6 ms steps of a small fraction of a
// 7-bit value, and its fractional mapping runs from flash at 86 Hz instead of
// taking IRAM to run at 690 Hz.
void State::fineEval(int t) {
  TrackRt& r = tr[t];
  if (!r.inst) { exitFine(t); return; }
  constexpr int kN = kSoundParams + 3;
  float cur[kN];
  const uint32_t e = qCount - r.iStart;
  const bool done = e >= r.iLen;
  const float w = done ? 1.0f : float(e) / float(r.iLen);
  for (int i = 0; i < kN; ++i) {
    const float a = float(r.fA[i]);
    if (!((r.lerp >> i) & 1u)) {   // standing still in this interval (most parameters)
      cur[i] = a * (1.0f / 256.0f);
      continue;
    }
    const float b = float(r.fB[i]);
    const bool step = i < kSoundParams && ((r.fStep >> i) & 1u);
    cur[i] = (step ? (done ? b : a) : a + (b - a) * w) * (1.0f / 256.0f);
  }
  const bool fresh = r.anchor || done;
  if (fresh && !fineInterval(t, cur)) return;
  const bool feed = fresh || t >= kNumDrumTracks || ((qCount + uint32_t(t)) & uint32_t(kFineEvalQuanta - 1)) == 0;
  if (voice[t] && feed) voice[t]->setParamsFine(cur);
  const float* mix = cur + kSoundParams;
  if (mix[0] != r.fMix[0] || mix[1] != r.fMix[1] || mix[2] != r.fMix[2]) {
    r.fMix[0] = mix[0];
    r.fMix[1] = mix[1];
    r.fMix[2] = mix[2];
    r.settled = 0;
  }
}

// A new interval from the values now (cur; replaced by the set's values on a
// jump). Once per 11.6 ms per moving track: stays in flash (only fineEval's
// per-tick part is in IRAM). False when the track has left the fractional path.
__attribute__((noinline)) bool State::fineInterval(int t, float* cur) {
  TrackRt& r = tr[t];
  constexpr int kN = kSoundParams + 3;
  {
    if (r.settling && !r.anchor) { exitFine(t); return false; }   // arrived at the stored bytes
    if (r.anchor == 2) fineValues(t, qCount, cur);          // a jump: the values are what the set says now
    const bool first = r.anchor == 3;                       // the start, from the bytes
    r.anchor = 0;
    for (int m = 0; m < kMacroSlots; ++m)
      if (((r.moving >> m) & 1u) && qCount - r.mStart[m] >= r.mDur[m]) {
        r.mFrom[m] = r.mTo[m];
        r.moving = uint8_t(r.moving & ~(1u << m));
      }
    // The interval ends on a control tick of this track. Entering and leaving the
    // fractional path take kMacroSettleMs (from or to the bytes, at most half a
    // 7-bit value away); in motion an interval is kFineEvalQuanta.
    uint32_t end = qCount + uint32_t(r.moving && !first ? kFineEvalQuanta : settleQ);
    end += (uint32_t(kFineQuanta) - ((end + uint32_t(t)) & uint32_t(kFineQuanta - 1))) & uint32_t(kFineQuanta - 1);
    float to[kN];
    if (r.moving) {
      fineValues(t, end, to);
      r.settling = 0;
    } else {
      updateStand(t);   // the bytes to run into: the stored ones, or where a motion holds the macros
      bytesNow(t, to);
      r.settling = 1;
    }
    auto q8 = [](float x) { return uint16_t(int(clampf(x, 0.0f, 255.0f) * 256.0f + 0.5f)); };
    r.lerp = 0;
    for (int i = 0; i < kN; ++i) {
      r.fA[i] = q8(cur[i]);
      r.fB[i] = q8(to[i]);
      if (r.fA[i] != r.fB[i]) r.lerp |= 1u << i;
    }
    r.iStart = qCount;
    r.iLen = uint16_t(end - qCount);
  }
  return true;
}

// Once per quantum while any track is in the fractional path: each such track is
// fed every kFineQuanta quanta (tracks staggered by their index, so two moving
// tracks do not evaluate in one quantum), or at once after a start, jump or lock.
void State::fineTick() {
  for (int t = 0; t < kNumTracks; ++t) {
    if (!((fineTracks >> t) & 1u)) continue;
    if (tr[t].anchor || ((qCount + uint32_t(t)) & uint32_t(kFineQuanta - 1)) == 0) fineEval(t);
  }
}

// ---------------------------------------------------------------- motion ----
// engine.h "MOTION". A keyframe of macro m on track t is a note (a step that is on)
// with its own value of m, at its grid time with swing, within the master length
// of the pattern (a track wraps on its own length). Nothing here runs per quantum:
// motionStep runs at a step of a track whose pattern has motion (a few compares
// unless the step is a keyframe), aimMotion (a scan of at most two patterns'
// worth of steps) at a keyframe, a pattern boundary, or once per drain that
// changed what the motion depends on (refreshMotion).

namespace {
// The quantum (counted from now, at least minQ) in which a step whose ideal time
// is dFP (Q32 samples from this quantum's edge) fires: the sequencer fires a step
// in the first quantum whose edge is less than half a quantum before it.
uint32_t fireQuanta(int64_t dFP, uint32_t minQ) {
  int64_t j = dFP < kHalfQuantumFP ? 0 : (dFP - kHalfQuantumFP) / kQuantumFP + 1;
  if (j < int64_t(minQ)) j = minQ;
  return uint32_t(j);
}
}  // namespace

// Macros (bits) with keyframes in `pattern` for track t (only macros its set has).
uint8_t State::keyframeMask(int t, int pattern) const {
  const TrackRt& r = tr[t];
  if (!r.inst || r.inst->numMacros <= 0) return 0;
  const Pattern& P = proj->patterns[pattern];
  const TrackPattern& tp = P.tracks[t];
  const int tlen = sanitizeLength(tp.length), plen = sanitizeLength(P.length);
  const int n = tlen < plen ? tlen : plen;
  uint32_t w = 0;
  for (int i = 0; i < n; ++i)
    if (tp.steps[i].on()) w |= tp.steps[i].extWord();
  uint8_t mask = 0;
  for (int m = 0; m < kTrackMacros; ++m)
    if ((w >> (6 * m)) & 63u) mask = uint8_t(mask | (1u << m));
  return uint8_t(mask & ((1u << r.inst->numMacros) - 1u));
}

// Aims macros `which` of track t at their next keyframe after the steps already
// played: through the rest of this pattern, then the pattern that plays next (the
// same one looping, a queued one, the next song slot); none there: the base,
// arriving at the boundary. From where the macro is now, arriving in the quantum
// that note fires (at least minQuanta from now).
void State::aimMotion(int t, uint8_t which, uint32_t minQuanta) {
  TrackRt& r = tr[t];
  which = uint8_t(which & ~r.live);
  if (!which || !r.inst) return;
  const int64_t L = int64_t(stepLenFP);
  uint8_t found = 0;
  uint8_t val[kTrackMacros] = {};
  int64_t when[kTrackMacros] = {};
  auto scan = [&](const Step& st, int64_t at) {
    if (!st.on()) return;
    for (uint8_t b = uint8_t(which & ~found); b; b = uint8_t(b & (b - 1))) {
      const int m = __builtin_ctz(b);
      if (st.hasMacro(m)) {
        found = uint8_t(found | (1u << m));
        val[m] = st.macroValue(m);
        when[m] = at;
      }
    }
  };
  const int plen = curPatternLen();
  int k = 0;
  {
    const TrackPattern& tp = proj->patterns[curPattern].tracks[t];
    const int tlen = sanitizeLength(tp.length);
    int s = r.next >= tlen ? 0 : r.next;
    for (int idx = masterNext; idx < plen && found != which; ++idx, ++k) {
      scan(tp.steps[s], gridCd + int64_t(k) * L + swingOffFP(idx));
      s = s + 1 >= tlen ? 0 : s + 1;
    }
  }
  if (found != which) {
    const int64_t t0 = gridCd + int64_t(k) * L;   // the boundary's grid line (k: the steps left)
    const uint8_t np = decideNext(false);
    const TrackPattern& tq = proj->patterns[np].tracks[t];
    const int tl2 = sanitizeLength(tq.length), pl2 = sanitizeLength(proj->patterns[np].length);
    for (int j = 0; j < pl2 && found != which; ++j) scan(tq.steps[j % tl2], t0 + int64_t(j) * L + swingOffFP(j));
    for (uint8_t b = uint8_t(which & ~found); b; b = uint8_t(b & (b - 1))) {
      const int m = __builtin_ctz(b);
      val[m] = proj->tracks[t].macro[m];
      when[m] = t0;
    }
  }
  for (uint8_t b = which; b; b = uint8_t(b & (b - 1))) {
    const int m = __builtin_ctz(b);
    rampTo(t, m, slotPos(r, m, qCount), float(val[m]), fireQuanta(when[m], minQuanta));
  }
}

// Where the looping pattern has macro m at its first step (Play from the top): a
// keyframe there, or the line from the pattern's last keyframe (one loop earlier)
// to its first, at that time. So the first pass sounds as every later one.
float State::loopStartValue(int t, int m) const {
  const Pattern& P = proj->patterns[curPattern];
  const TrackPattern& tp = P.tracks[t];
  const int tlen = sanitizeLength(tp.length), plen = sanitizeLength(P.length);
  int iF = -1, iL = -1;
  float vF = 0.0f, vL = 0.0f;
  for (int i = 0; i < plen; ++i) {
    const Step& st = tp.steps[i % tlen];
    if (!st.on() || !st.hasMacro(m)) continue;
    if (iF < 0) { iF = i; vF = float(st.macroValue(m)); }
    iL = i;
    vL = float(st.macroValue(m));
  }
  if (iF < 0) return float(proj->tracks[t].macro[m]);
  if (iF == 0 || vF == vL) return vF;
  const int64_t L = int64_t(stepLenFP);
  const int64_t tL = int64_t(iL) * L + swingOffFP(iL) - int64_t(plen) * L;   // < 0
  const int64_t tF = int64_t(iF) * L + swingOffFP(iF);                         // > 0
  const float w = float(-tL >> 16) / float((tF - tL) >> 16);
  return vL + (vF - vL) * w;
}

// After master step r.pos of track t fired (or would have: a muted, suppressed or
// unlucky note moves the motion all the same). `fresh`: the step began a pattern,
// or Play started: the playing pattern's motion is looked up again.
void State::motionStep(int t, bool fresh) {
  TrackRt& r = tr[t];
  if (fresh) {
    const uint8_t now = keyframeMask(t, curPattern);
    const uint8_t ended = uint8_t(r.kfMask & ~now & ~r.live);
    r.kfMask = now;
    publishMotion(t);
    // No motion in this pattern: the base (the glide into the pattern already went there;
    // at Play, from where an earlier motion left it resting).
    uint8_t toBase = ended;
    if (motStart)
      for (int m = 0; m < kTrackMacros; ++m)
        if (!((now >> m) & 1u) && !((r.live >> m) & 1u) && !((r.moving >> m) & 1u) && r.mTo[m] != float(proj->tracks[t].macro[m]))
          toBase = uint8_t(toBase | (1u << m));
    for (uint8_t b = toBase; b; b = uint8_t(b & (b - 1))) startRamp(t, __builtin_ctz(b), proj->tracks[t].macro[__builtin_ctz(b)], 0);
  }
  if (r.live) liveStep(t);
  const uint8_t mask = uint8_t(r.kfMask & ~r.live);
  if (!mask) return;
  const Step& st = proj->patterns[curPattern].tracks[t].steps[r.pos];
  uint8_t kf = 0;
  if (st.on())
    for (uint8_t b = mask; b; b = uint8_t(b & (b - 1))) {
      const int m = __builtin_ctz(b);
      if (st.hasMacro(m)) {
        kf = uint8_t(kf | (1u << m));
        // At its note the value is exactly the note's (the glide arrived here).
        r.mFrom[m] = r.mTo[m] = float(st.macroValue(m));
        r.moving = uint8_t(r.moving & ~(1u << m));
      }
    }
  uint8_t aim = kf;
  if (fresh) {
    const uint8_t other = uint8_t(mask & ~kf);
    if (motStart)
      for (uint8_t b = other; b; b = uint8_t(b & (b - 1))) jumpMacro(t, __builtin_ctz(b), loopStartValue(t, __builtin_ctz(b)));
    aim = mask;
  }
  if (aim) aimMotion(t, aim, 1);
  if (kf && !r.moving && !r.fine) updateStand(t);
}

// Motion recording (engine.h "MOTION"): a live macro's value goes into the note
// that starts here; a hand still for a bar is no longer live (its motion plays).
void State::liveStep(int t) {
  TrackRt& r = tr[t];
  for (uint8_t b = uint8_t(r.live & ~r.tiltLive); b; b = uint8_t(b & (b - 1))) {   // a held tilt stays live
    const int m = __builtin_ctz(b);
    if (int32_t(qCount - r.liveUntil[m]) >= 0) {
      r.live = uint8_t(r.live & ~(1u << m));
      motDirty = uint8_t(motDirty | (1u << t));
      publishMotion(t);
    }
  }
  if (!r.live || !recording) return;
  Step& st = proj->patterns[curPattern].tracks[t].steps[r.pos];
  if (!st.on()) return;
  const uint32_t before = st.extWord();
  for (uint8_t b = r.live; b; b = uint8_t(b & (b - 1))) {
    const int m = __builtin_ctz(b);
    st.setMacroValue(m, int(macroPos(r, m, qCount) + 0.5f));
  }
  if (st.extWord() != before) bump();
}

// What the motion depends on changed (a step, a length, the next pattern, the
// tempo): every dirty track's motion re-aims from where its macros are.
void State::refreshMotion() {
  for (int t = 0; t < kNumTracks; ++t) {
    if (!((motDirty >> t) & 1u)) continue;
    TrackRt& r = tr[t];
    const uint8_t now = keyframeMask(t, curPattern);
    const uint8_t ended = uint8_t(r.kfMask & ~now & ~r.live);
    if (now != r.kfMask) {
      r.kfMask = now;
      publishMotion(t);
    }
    for (uint8_t b = ended; b; b = uint8_t(b & (b - 1))) startRamp(t, __builtin_ctz(b), proj->tracks[t].macro[__builtin_ctz(b)], 0);
    aimMotion(t, now, 0);
  }
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

// A hosted voice's note with macro values of its own: the voice gets the instrument
// evaluated where the macros are heard now, with the note's values in place of theirs,
// so what it latches at the note-on (the sampler's slice) is the note's. The engine's
// own parameters follow at the next quantum (paramsSent = 0; while a macro moves, at
// the next fractional feed). One evaluation per such note.
void State::primeNote(int t, const Step& st) {
  TrackRt& r = tr[t];
  uint8_t pos[kTrackMacros];
  for (int m = 0; m < kTrackMacros; ++m)
    pos[m] = st.hasMacro(m) ? st.macroValue(m) : uint8_t(macroPos(r, m, qCount) + 0.5f);
  uint8_t out[kParamsPerTrack];
  deriveInto(*r.inst, voice[t], t, pos, out);
  voice[t]->setParams(out);
  r.paramsSent = 0;
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
  if (primeSt) primeNote(t, *primeSt);
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
  // A step recorded while its key is still down does not fire under the player's
  // hand (a note held longer than the loop): the live note wins until release.
  const bool suppressed = (r.supValid && r.supPattern == curPattern && r.supStep == stepIndex) ||
                          (r.recHeld && r.recPattern == curPattern && r.recStep == stepIndex);
  r.supValid = 0;
  r.ratLeft = 0;                                   // ratchets never spill into the next step

  bool fires = (st.flags() & kStepOn) && !proj->tracks[t].mute && !suppressed && !r.rollC;
  if (fires && st.prob() < 100) fires = (rand32() % 100u) < st.prob();

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
  // (CLASSIC only: an instrument track's notes carry macro values instead, motionStep.)
  uint8_t newLock = 0, newValue = 0;
  if (!instTrack(t) && st.lockParam() >= 1 && st.lockParam() <= kParamsPerTrack) {
    newLock = st.lockParam();
    newValue = st.lockValue();
  }
  if (newLock != r.lockParam || newValue != r.lockValue) r.paramsSent = 0;
  r.lockParam = newLock;
  r.lockValue = newValue;

  const int ratchets = stepRatchet(st);
  int32_t gate = int32_t((int64_t(st.gate) * stepTicks) / (kGateUnitsPerStep * ratchets));
  if (gate < kQuantumTicks) gate = kQuantumTicks;
  NoteEvent ev;
  ev.note = st.note > 127 ? 127 : st.note;
  ev.vel = st.vel < 1 ? 1 : (st.vel > 127 ? 127 : st.vel);
  ev.accent = (st.flags() & kStepAccent) != 0;
  ev.slide = false;
  ev.chord = (t == kTrackLead && st.chord() < kChordNum) ? st.chord() : 0;
  const bool slide = isSynthTrack(t) && (st.flags() & kStepSlide);

  // VOICE KINDS: a hosted voice (the sampler) latches parameters at its note-on, and
  // the note's own macro values only reach the motion after the note has started
  // (motionStep runs after the step fires). primeNote hands it the parameters at the
  // note's own values first. Native voices are never primed (bit-identical).
  primeSt = (voice[t] != nativeV[t] && r.inst && st.hasExt()) ? &st : nullptr;
  hit(t, ev, frac + gate, slide && ratchets == 1);
  primeSt = nullptr;
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
      slot = songStart; rep = 0;
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
  boundaryNow = true;
  curPattern = decideNext(true);
  queued = kNoPattern;
  masterNext = 0;
  for (int t = 0; t < kNumTracks; ++t) tr[t].next = 0;
}

void State::fireMasterStep() {
#if PG_STEM_TAP
  if (masterNext >= curPatternLen() && playsLeft >= 0) {
    if (playsLeft == 0) {
      // End of the offline render: fire nothing more. Notes held "until the
      // next step" would never see one, so let their own gates end them.
      seqHeld = true;
      endedAt = long(qCount) * kQuantum;
      for (int t = 0; t < kNumTracks; ++t)
        for (int i = 0; i < kMaxPend; ++i) tr[t].pend[i].holdToStep = 0;
      return;
    }
    --playsLeft;
  }
#endif
  if (masterNext >= curPatternLen()) boundary();
  const Pattern& pat = proj->patterns[curPattern];
  const int len = sanitizeLength(pat.length);
  const int idx = masterNext;
  masterPos = idx;
  masterNext = idx + 1;
  if ((idx & 15) == 0) ++barCount;   // a bar line (the output tap's kTapBar)

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

  // Rolls stay phase-locked: on every even step since Play (a point of every
  // roll grid) snap the roll clock to the step's exact time, keeping which
  // hit is next (only the accumulated rounding is removed).
  if ((absStep & 1) == 0) {
    const int64_t at = gridCd;            // straight grid line of this (even, unswung) step
    for (int t = 0; t < kNumTracks; ++t) {
      TrackRt& r = tr[t];
      if (!r.rollC || r.rollInt <= 0) continue;
      int64_t d = (r.rollCd - at) % r.rollInt;
      if (d > r.rollInt / 2) d -= r.rollInt;
      if (d < -r.rollInt / 2) d += r.rollInt;
      r.rollCd -= d;
    }
  }
  ++absStep;

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
  // MOTION: the notes' own macro values at their grid times (whether they fire or not).
  const bool fresh = boundaryNow || motStart;
  for (int t = 0; t < kNumTracks; ++t)
    if (tr[t].inst && (fresh || tr[t].kfMask || tr[t].live)) motionStep(t, fresh);
  boundaryNow = false;
  motStart = false;
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
#if PG_STEM_TAP
    if (!seqHeld)
#endif
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
  // 3b. Roll hits.
  rollTick();
  // 4. Slide-armed notes whose (long) gate ran out without anything to slide
  //    into; 5. advance time. One pass (4 never looks at other entries).
  gridCd -= kQuantumFP;
  if (sinceFire < (1 << 30)) sinceFire += kQuantumTicks;
  for (int t = 0; t < kNumTracks; ++t) {
    TrackRt& r = tr[t];
    for (int i = 0; i < kMaxPend; ++i) {
      PendNote& p = r.pend[i];
      if (!p.active) continue;
      if (p.slideArmed && !p.holdToStep && p.cd < kDueTicks) { vOff(t, p.note); p.active = 0; continue; }
      if (p.cd > -(1 << 30)) p.cd -= kQuantumTicks;
    }
    if (r.ratLeft) r.ratCd -= kQuantumTicks;
  }
}

// ------------------------------------------------------------ live play ----

void State::liveNoteOn(const Command& c) {
  const int t = c.a;
  NoteEvent ev;
  ev.note = uint8_t(c.b > 127 ? 127 : c.b);
  ev.vel = uint8_t(clampi(c.c, 1, 127));
  ev.accent = (c.step.flags() & kStepAccent) != 0;
  ev.slide = isSynthTrack(t) && (c.step.flags() & kStepSlide);
  ev.chord = (t == kTrackLead && c.step.chord() < kChordNum) ? c.step.chord() : 0;
  ensureParams(t);
  vOn(t, ev);
  tr[t].liveHeld[ev.note >> 5] |= 1u << (ev.note & 31);
  afterNoteOn(t);
  if (playing && recording) recordNote(t, ev);
}

// DECISION (record gate): only synth tracks track the held keys (drums are
// one-shots). The gate is the held time in quarter-steps of the current tempo,
// rounded, from the first note-on that landed on the step to the release of the
// LAST key down that landed on it (a hand-played chord: whatever the release
// order). A NoteOff for any other note (e.g. the earlier key of a legato pair,
// whose step already got its gate when the next note landed) changes nothing.
// Every recorded gate is capped at the loop the step lives in (recordGateCap):
// a note cannot be longer than the pattern that repeats it, so on playback it
// ends no later than its own next trigger. Recording stopping or the transport
// stopping forgets the held step.
void State::liveNoteOff(const Command& c) {
  const int t = c.a;
  const uint8_t note = uint8_t(c.b & 127);
  vOff(t, note);
  TrackRt& r = tr[t];
  r.liveHeld[note >> 5] &= ~(1u << (note & 31));
  if (!r.recHeld || !(r.recKeys[note >> 5] & (1u << (note & 31)))) return;
  r.recKeys[note >> 5] &= ~(1u << (note & 31));
  if (r.recKeys[0] | r.recKeys[1] | r.recKeys[2] | r.recKeys[3]) return;   // another key of the step is down
  r.recHeld = 0;
  if (!playing || !recording) return;
  int64_t held = int64_t(uint32_t(qCount - r.recStartQ)) * kQuantumTicks;   // ticks
  if (held > (int64_t(1) << 40)) held = int64_t(1) << 40;
  const int64_t unit = stepTicks > 0 ? stepTicks : 1;                       // ticks per step
  setRecordedGate(t, (held * kGateUnitsPerStep + unit / 2) / unit);
}

// The longest gate a step of track t in pattern p records: the loop it repeats
// in (the shorter of the track's and the pattern's length), at most the byte
// (63.75 steps).
int State::recordGateCap(int t, int p) const {
  const Pattern& pat = proj->patterns[p];
  int len = sanitizeLength(pat.tracks[t].length);
  const int plen = sanitizeLength(pat.length);
  if (plen < len) len = plen;
  const int cap = len * kGateUnitsPerStep;
  return cap > 255 ? 255 : cap;
}

// The held step's gate, in quarter-steps, capped (recordGateCap).
void State::setRecordedGate(int t, int64_t g) {
  TrackRt& r = tr[t];
  const int cap = recordGateCap(t, r.recPattern);
  if (g > cap) g = cap;
  if (g < 1) g = 1;
  Step& s = proj->patterns[r.recPattern].tracks[t].steps[r.recStep];
  if ((s.flags() & kStepOn) && s.gate != uint8_t(g)) {
    s.gate = uint8_t(g);
    bump();
  }
}

// DECISION (record quantise): the note goes to whichever step's fire time
// (swing included) is nearer: the step this track last played, or the one it
// is about to play. Ties go to the step already played. If it lands on the
// upcoming step, that step is suppressed once, so the player hears their own
// live note and not a second trigger a few ms later. At a pattern boundary
// the upcoming step is step 0 of whatever pattern plays next (queued pattern /
// song slot), because that is the downbeat the player was aiming at.
// Drums: step on + velocity (tuning, p-lock, prob, ratchet untouched).
// Synths: note, velocity, accent as played, default gate (replaced by the held
// time when the last key on the step is released), chord (lead), no slide.
// Several keys landing on one step (a hand-played chord): the last one pressed
// is the step's note; the gate runs until the last of them is released.
// Legato: if a key of the previously recorded step is still down when the new
// note lands on a different step, that previous step gets kStepSlide and the
// gate that reaches the new note (from its own grid time to the new note's,
// swing, pages and the loop point included, rounded up), so it plays back as
// one connected line. kStepSlide on a step means "this note runs into the
// next", so the new step gets no slide from being played legato.
void State::recordNote(int t, const NoteEvent& ev) {
  TrackRt& r = tr[t];
  const int len = curPatternLen();
  const int nextMaster = masterNext >= len ? 0 : masterNext;
  const int32_t toNext = int32_t((gridCd + swingOffFP(nextMaster)) >> 24);
  int pattern = curPattern, step = r.pos;
  int32_t offT = -sinceFire;                     // the chosen step's grid time relative to now, ticks
  if (sinceFire > toNext) {
    if (masterNext >= len) {
      pattern = decideNext(false);
      step = 0;
    } else {
      step = r.next;
      if (step >= sanitizeLength(proj->patterns[pattern].tracks[t].length)) step = 0;
    }
    offT = toNext;
    r.supValid = 1;
    r.supPattern = uint8_t(pattern);
    r.supStep = uint8_t(step);
  }
  const bool sameStep = r.recHeld && r.recPattern == pattern && r.recStep == step;
  if (isSynthTrack(t) && r.recHeld && !sameStep) {
    Step& prev = proj->patterns[r.recPattern].tracks[t].steps[r.recStep];
    if (prev.flags() & kStepOn) {
      prev.addFlags(kStepSlide);
      // Rounded up (past 1/8 of a quarter-step): a slide must still be sounding
      // when the new note fires, or it plays as a release and a fresh attack.
      const int64_t dist = int64_t(uint32_t(qCount - r.recStartQ)) * kQuantumTicks + offT - r.recOffT;   // ticks
      const int64_t unit = stepTicks > 0 ? stepTicks : 1;
      setRecordedGate(t, dist < (int64_t(1) << 40) ? (dist * kGateUnitsPerStep + unit * 7 / 8) / unit : 255);
    }
  }
  Step& s = proj->patterns[pattern].tracks[t].steps[step];
  s.vel = ev.vel;
  if (isSynthTrack(t)) {
    s.note = ev.note;
    s.setFlags(uint8_t((s.flags() & kStepRatchetMask) | kStepOn | (ev.accent ? kStepAccent : 0)));
    s.gate = kDefaultStep.gate;
    s.setChord(ev.chord);
    for (uint8_t b = r.live; b; b = uint8_t(b & (b - 1))) {   // a live hand: the note takes its values
      const int m = __builtin_ctz(b);
      s.setMacroValue(m, int(macroPos(r, m, qCount) + 0.5f));
    }
    if (!sameStep) {
      std::memset(r.recKeys, 0, sizeof(r.recKeys));
      r.recPattern = uint8_t(pattern);
      r.recStep = uint8_t(step);
      r.recStartQ = qCount;
      r.recOffT = offT;
    }
    r.recKeys[ev.note >> 5] |= 1u << (ev.note & 31);
    r.recHeld = 1;
  } else {
    s.addFlags(kStepOn);
    for (uint8_t b = r.live; b; b = uint8_t(b & (b - 1))) {
      const int m = __builtin_ctz(b);
      s.setMacroValue(m, int(macroPos(r, m, qCount) + 0.5f));
    }
  }
  motDirty = uint8_t(motDirty | (1u << t));
  bump();
}

// --------------------------------------------------------------- render ----

// MONO OUTPUT (device mode, Engine::setMonoOutput): the codec plays one
// channel, so the whole chain runs once:
//   1. every track accumulates into one bus at its centred gain (pan is
//      ignored), reading only outL of mono voices and (L+R)/2 of the others;
//   2. the FX return is only the mono sum: Delay emits e1+e2 (its two taps,
//      no width law), Reverb skips its side tap set, one upsampler;
//   3. MasterBus runs filter/drive/limiter on one channel and writes the
//      sample to both int16 slots.
// For centred sources the result equals (L+R)/2 of the stereo engine (the
// delay and reverb mono sums are exact by construction) except where the
// master's nonlinear stages (drive, limiter knee) act differently on L/R
// than on their sum.
// STEM TAP (host only, PG_STEM_TAP, see stem_tap.h): the PG_TAP_* lines
// below copy values the mixer already computes into the tap's buffers. On the
// device the macros expand to nothing.
#if PG_STEM_TAP
#define PG_TAP_DRY(i, a, b) do { if (tapL) { tapL[i] = (a); tapR[i] = (b); } } while (0)
#else
#define PG_TAP_DRY(i, a, b) do { } while (0)
#endif

#if PG_STEM_TAP
namespace {
void tapZero(float* const (&buf)[2], long pos) {
  for (int c = 0; c < 2; ++c)
    if (buf[c]) for (int i = 0; i < kQuantum; ++i) buf[c][pos + i] = 0.0f;
}
}  // namespace
#endif

// A track with a voice fading out after a switch (VOICE KINDS): the track's voice (if it
// sounds) plus the fading one, in stereo. Clears fadeV once it has faded.
__attribute__((noinline)) void State::renderFading(Voice* v, float* L, float* R) {
  float L2[kQuantum], R2[kQuantum];
  for (int i = 0; i < kQuantum; ++i) L[i] = R[i] = 0.0f;
  if (v && v->active()) {
    v->render(L, R, kQuantum);
    if (v->mono())
      for (int i = 0; i < kQuantum; ++i) R[i] = L[i];
  }
  if (fadeV && fadeV->active()) {
    fadeV->render(L2, R2, kQuantum);
    const bool m = fadeV->mono();
    for (int i = 0; i < kQuantum; ++i) {
      L[i] += L2[i];
      R[i] += m ? L2[i] : R2[i];
    }
  }
  if (fadeV && !fadeV->active()) fadeV = nullptr;
}

void State::render(int16_t* out) {
  float busL[kQuantum], busR[kQuantum], sD[kQuantum], sR[kQuantum];
  const bool mono = monoOut;
  for (int i = 0; i < kQuantum; ++i) { busL[i] = 0.0f; busR[i] = 0.0f; sD[i] = 0.0f; sR[i] = 0.0f; }
#if PG_STEM_TAP
  StemTap* const tp = (tap && !mono && tap->pos + kQuantum <= tap->capacity) ? tap : nullptr;
  if (tap && !mono && !tp) tap->overflow = true;
  const long tpos = tp ? tp->pos : 0;
  if (tp) {
    for (int t = 0; t < kNumTracks; ++t) tapZero(tp->track[t], tpos);
    tapZero(tp->delayRet, tpos);
    tapZero(tp->reverbRet, tpos);
    tapZero(tp->preMaster, tpos);
  }
#endif
  // Scope tap: pick up the UI's request (one load per quantum; a change restarts the ring's history).
  const uint16_t scReq = scopeReq.load(std::memory_order_relaxed);
  if (scReq != scopeCfg) {
    scopeCfg = scReq;
    scopeTrack = (scReq & 0xFF) ? uint8_t((scReq & 0xFF) - 1) : uint8_t(0xFF);
    scopeDecim = uint8_t(scReq >> 8) ? uint8_t(scReq >> 8) : uint8_t(1);
    scopePhase = 0;
    scopeAcc = 0.0f;
    scopeInvD = 1.0f / float(scopeDecim);
    scopeHitCount = 0;
    scopeHits.store(0, std::memory_order_relaxed);
    scopeSince.store(scopeWr, std::memory_order_relaxed);
    scopeCfgA.store(scReq, std::memory_order_release);
  }
  const int scT = scopeTrack;
  const int fadeT = fadeV ? int(fadeTrack) : -1;
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
    } else if (r.settled && key == r.mixKey) {
      // Only the ducker moved (a pumping track, every quantum): the general
      // path below reduces to exactly this for a settled track.
      const float duckF = 1.0f + float(r.eff[kMixDuck]) * (1.0f / 127.0f) * (duckEnd - 1.0f);
      endL = r.gL * duckF;
      endR = r.gR * duckF;
      endD = r.sendD;
      endR2 = r.sendR;
      steady = endL == r.outL && endR == r.outR;
      sends = r.hasSends != 0;
    } else {
      r.mixKey = key;
      const int pan = mono ? 64 : r.eff[kMixPan];
      // A macro in motion moves the level by fractions (MACRO MOTION).
      const float level = r.fine ? r.fMix[0] : float(r.eff[kMixLevel]);
      if (level != r.cLevel || pan != r.cPan) {
        if (pan != r.cPan) {
          const float p = clampf(float(pan - 64) * (1.0f / 63.0f), -1.0f, 1.0f);
          const float th = (p + 1.0f) * 0.78539816f;
          r.panC = cosf(th);                      // libm only when the pan actually changes
          r.panS = sinf(th);
        }
        r.cLevel = level;
        r.cPan = pan;
        // Level: 100 = unity, cubic taper. Pan: constant-power balance, unity at centre.
        const float lv = clampf(level / 100.0f, 0.0f, 1.27f);
        const float g = lv * lv * lv;
        r.tgtL = g * 1.41421356f * r.panC;
        r.tgtR = g * 1.41421356f * r.panS;
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
      const float dv = (r.fine ? r.fMix[1] : float(r.eff[kMixDelaySend])) * (1.0f / 127.0f);
      const float rv = (r.fine ? r.fMix[2] : float(r.eff[kMixReverbSend])) * (1.0f / 127.0f);
      endD = r.sendD + (dv * dv - r.sendD) * gainK;
      endR2 = r.sendR + (rv * rv - r.sendR) * gainK;
      if (absf(dv * dv - endD) < 1.0e-5f) endD = dv * dv;
      if (absf(rv * rv - endR2) < 1.0e-5f) endR2 = rv * rv;
      r.settled = (r.gL == tL && r.gR == tR && endD == dv * dv && endR2 == rv * rv) ? 1 : 0;
      r.hasSends = (endD > 0.0f || endR2 > 0.0f) ? 1 : 0;
      steady = endL == r.outL && endR == r.outR && endD == r.sendD && endR2 == r.sendR;
      sends = endD > 0.0f || endR2 > 0.0f || r.sendD > 0.0f || r.sendR > 0.0f;
    }
#if PG_STEM_TAP
    float* tapL = nullptr;
    float* tapR = nullptr;
    if (tp) {
      if (tp->track[t][0] && tp->track[t][1]) { tapL = tp->track[t][0] + tpos; tapR = tp->track[t][1] + tpos; }
      if (!((tp->sendMask >> t) & 1u)) {   // this track's sends are masked out for this render
        endD = 0.0f; endR2 = 0.0f; r.sendD = 0.0f; r.sendR = 0.0f;
        sends = false;
        steady = endL == r.outL && endR == r.outR;
      }
    }
#endif

    Voice* v = voice[t];
    const bool fadeHere = t == fadeT;
    if ((v && v->active()) || fadeHere) {
      float L[kQuantum], R[kQuantum];
      if (fadeHere) {
        // VOICE KINDS: a switch left the old voice fading out on this track: both render.
        renderFading(v, L, R);
      } else {
        v->render(L, R, kQuantum);
      }
      if (t == scT) {
        // Scope tap: the voice's left channel averaged over scopeDecim samples (a box
        // filter: a decimated bright wave keeps a steady shape) into the ring.
        float acc = scopeAcc;
        int ph = scopePhase;
        uint32_t w = scopeWr;
        const int d = scopeDecim;
        for (int i = 0; i < kQuantum; ++i) {
          acc += L[i];
          if (++ph == d) {
            scopeRing[w++ & (kScopeRing - 1)] = acc * scopeInvD;
            acc = 0.0f;
            ph = 0;
          }
        }
        scopeAcc = acc;
        scopePhase = ph;
        scopeWr = w;
        scopeEnd.store(w, std::memory_order_release);
      }
      // Mono voices write L == R: read only L. (renderFading writes both.)
      const bool vMono = !fadeHere && v->mono();
      const float* Rp = vMono ? L : R;
      // A voice that went non-finite (or absurdly large) is silenced and
      // choked instead of poisoning the buses and FX states.
      if (!(absf(L[kQuantum - 1]) < 64.0f) || !(absf(Rp[kQuantum - 1]) < 64.0f) ||
          !(absf(L[0]) < 64.0f) || !(absf(Rp[0]) < 64.0f)) {
        if (v) v->choke();
        ++faults;
      } else if (mono) {
        // One bus. Centred gain (endL == endR here); a stereo voice
        // contributes (L+R)/2. Send = x * send gain, the same as the stereo
        // path's (a + b) * send / 2 for a centred source.
        float M[kQuantum];
        const float* x = L;
        if (!vMono) {
          for (int i = 0; i < kQuantum; ++i) M[i] = 0.5f * (L[i] + R[i]);
          x = M;
        }
        if (steady) {
          const float g = endL;
          if (!sends) {
            for (int i = 0; i < kQuantum; ++i) busL[i] += x[i] * g;
          } else {
            const float gd = endD, gv = endR2;
            for (int i = 0; i < kQuantum; ++i) {
              const float a = x[i] * g;
              busL[i] += a;
              sD[i] += a * gd;
              sR[i] += a * gv;
            }
          }
        } else if (sends && endD == r.sendD && endR2 == r.sendR) {
          // Gain ramping (ducking), send gains steady: ramp only the gain.
          float g = r.outL;
          const float dg = (endL - g) * invN, gd = endD, gv = endR2;
          for (int i = 0; i < kQuantum; ++i) {
            g += dg;
            const float a = x[i] * g;
            busL[i] += a;
            sD[i] += a * gd;
            sR[i] += a * gv;
          }
        } else {
          float g = r.outL, gd = r.sendD, gv = r.sendR;
          const float dg = (endL - g) * invN, dd = (endD - gd) * invN, dvv = (endR2 - gv) * invN;
          if (sends) {
            for (int i = 0; i < kQuantum; ++i) {
              g += dg; gd += dd; gv += dvv;
              const float a = x[i] * g;
              busL[i] += a;
              sD[i] += a * gd;
              sR[i] += a * gv;
            }
          } else {
            for (int i = 0; i < kQuantum; ++i) {
              g += dg;
              busL[i] += x[i] * g;
            }
          }
        }
        if (sends) {
          if (endD > 0.0f || r.sendD > 0.0f) dSend = true;
          if (endR2 > 0.0f || r.sendR > 0.0f) rSend = true;
        }
      } else {
        if (steady && !sends) {
          const float gl = endL, gr = endR;
          for (int i = 0; i < kQuantum; ++i) {
            busL[i] += L[i] * gl;
            busR[i] += Rp[i] * gr;
            PG_TAP_DRY(i, L[i] * gl, Rp[i] * gr);
          }
        } else if (steady) {
          const float gl = endL, gr = endR, gd = 0.5f * endD, gv = 0.5f * endR2;
          for (int i = 0; i < kQuantum; ++i) {
            const float a = L[i] * gl, b = Rp[i] * gr;
            busL[i] += a;
            busR[i] += b;
            PG_TAP_DRY(i, a, b);
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
              const float a = L[i] * gl, b = Rp[i] * gr;
              busL[i] += a;
              busR[i] += b;
              PG_TAP_DRY(i, a, b);
              const float m = a + b;
              sD[i] += m * gd;
              sR[i] += m * gv;
            }
          } else {
            for (int i = 0; i < kQuantum; ++i) {
              gl += dl; gr += dr;
              busL[i] += L[i] * gl;
              busR[i] += Rp[i] * gr;
              PG_TAP_DRY(i, L[i] * gl, Rp[i] * gr);
            }
          }
        } else {
          // Sends moving too: ramp everything linearly.
          float gl = r.outL, gr = r.outR, gd = 0.5f * r.sendD, gv = 0.5f * r.sendR;
          const float dl = (endL - gl) * invN, dr = (endR - gr) * invN;
          const float dd = (0.5f * endD - gd) * invN, dvv = (0.5f * endR2 - gv) * invN;
          for (int i = 0; i < kQuantum; ++i) {
            gl += dl; gr += dr; gd += dd; gv += dvv;
            const float a = L[i] * gl, b = Rp[i] * gr;
            busL[i] += a;
            busR[i] += b;
            PG_TAP_DRY(i, a, b);
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
#if PG_STEM_TAP
      if (tp) for (int t = 0; t < kNumTracks; ++t) tapZero(tp->track[t], tpos);
#endif
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
      if (mono) delay.processMono(in, wL, kHalf);
      else delay.process(in, wL, wR, kHalf);
    }
#if PG_STEM_TAP
    float tdL[kHalf], tdR[kHalf];   // the delay's share of the return, before the reverb adds in
    if (tp) for (int i = 0; i < kHalf; ++i) { tdL[i] = wL[i]; tdR[i] = wR[i]; }
#endif
    if (runReverb) {
      decR.process(sR, in, kHalf);
      if (mono) reverb.processMono(in, wL, kHalf);
      else reverb.process(in, wL, wR, kHalf);
    }
    if (!(absf(wL[kHalf - 1]) < 64.0f) || !(absf(wR[kHalf - 1]) < 64.0f)) {
      delay.reset();
      reverb.reset();
      decD.reset(); decR.reset(); upL.reset(); upR.reset();
#if PG_STEM_TAP
      tapUpL.reset(); tapUpR.reset();
#endif
      ++faults;
    } else {
      upL.process(wL, fL, kHalf);
      float g = fxDuckPrev;
      const float dg = (fxEnd - g) * invN;
      if (mono) {
        for (int i = 0; i < kQuantum; ++i) {
          g += dg;
          busL[i] += fL[i] * g;
        }
      } else {
        upR.process(wR, fR, kHalf);
#if PG_STEM_TAP
        if (tp) {
          // Return = the upsampled sum times the FX duck, exactly as added
          // below; the delay part goes through its own (linear, identical)
          // upsamplers and the reverb part is the remainder.
          float dL[kQuantum], dR[kQuantum];
          tapUpL.process(tdL, dL, kHalf);
          tapUpR.process(tdR, dR, kHalf);
          float gt = g;
          for (int i = 0; i < kQuantum; ++i) {
            gt += dg;
            const float tl = fL[i] * gt, trr = fR[i] * gt, xl = dL[i] * gt, xr = dR[i] * gt;
            if (tp->delayRet[0]) tp->delayRet[0][tpos + i] = xl;
            if (tp->delayRet[1]) tp->delayRet[1][tpos + i] = xr;
            if (tp->reverbRet[0]) tp->reverbRet[0][tpos + i] = tl - xl;
            if (tp->reverbRet[1]) tp->reverbRet[1][tpos + i] = trr - xr;
          }
        }
#endif
        for (int i = 0; i < kQuantum; ++i) {
          g += dg;
          busL[i] += fL[i] * g;
          busR[i] += fR[i] * g;
        }
      }
    }
    fxRunning = true;
  } else if (fxRunning) {
    decD.reset(); decR.reset(); upL.reset(); upR.reset();
#if PG_STEM_TAP
    tapUpL.reset(); tapUpR.reset();
#endif
    fxRunning = false;
  }
  fxDuckPrev = fxEnd;

#if PG_STEM_TAP
  if (tp) {
    if (tp->preMaster[0]) for (int i = 0; i < kQuantum; ++i) tp->preMaster[0][tpos + i] = busL[i];
    if (tp->preMaster[1]) for (int i = 0; i < kQuantum; ++i) tp->preMaster[1][tpos + i] = busR[i];
    tp->pos = tpos + kQuantum;
  }
#endif
  master.setParams(mFilter, mReso, mDrive, mVolume);
  if (mono) master.processMono(busL, out, kQuantum, &peakL, &peakR);
  else master.process(busL, busR, out, kQuantum, &peakL, &peakR);
}

void State::publishStatus() {
  const std::memory_order rx = std::memory_order_relaxed;
  aPlaying.store(playing, rx);
  aRecording.store(recording, rx);
  aPattern.store(curPattern, rx);
  aQueued.store(queued, rx);
  aSongSlot.store(songSlot, rx);
  aSongRep.store(songRep, rx);
  aMasterStep.store(uint8_t(masterPos), rx);
  aWritesDone.store(writesDone, rx);
  aInstRefreshes.store(instRefreshes, rx);
  for (int t = 0; t < kNumTracks; ++t) {
    aStep[t].store(tr[t].pos, rx);
    aTrig[t].store(tr[t].trig, rx);
  }
  aDuck.store(duck.gain(), rx);
  aRevision.store(revision, rx);
  // Heard macro positions (the page's sliders): tracks that jumped or settled, and
  // every 4th quantum the moving ones. Nothing moving: this compare.
  const uint8_t pub = uint8_t(pubMask | ((qCount & 3u) == 0 ? fineTracks : 0));
  if (pub) {
    for (int t = 0; t < kNumTracks; ++t) {
      if (!((pub >> t) & 1u)) continue;
      uint32_t w = 0;
      for (int m = 0; m < kTrackMacros; ++m) w |= uint32_t(clampi(int(macroPos(tr[t], m, qCount) + 0.5f), 0, 255)) << (8 * m);
      aMacPos[t].store(w, rx);
    }
    pubMask = 0;
  }
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
  // Instruments follow the Project bytes (engine.h): a compare per track, an evaluation only on a change.
  for (int t = 0; t < kNumTracks; ++t) {
    const TrackRt& r = tr[t];
    const TrackSettings& ts = proj->tracks[t];
    if (!r.bindValid || instrumentId(ts) != r.boundId || (r.inst && std::memcmp(ts.macro, r.evPos, kTrackMacros) != 0))
      refreshInstrument(t, false);
  }
  if (motDirty) {                 // edits, a queued pattern, tempo: motion re-aims (rare)
    if (playing) refreshMotion();
    motDirty = 0;
  }
  if (fineTracks) fineTick();     // nothing moving: this compare is all macro motion costs
  refreshGlobals();
  if (playing) sequencerTick();
  else rollTick();
  for (int t = 0; t < kNumTracks; ++t) ensureParams(t);
  render(out);
  publishStatus();
  ++qCount;
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
  s.nativeV[track] = voice;
  s.cacheVoiceRanges(track);
  if (voice) voice->setMonoOutput(s.monoOut);
  if (voice && s.gValid) voice->setTempo(float(s.bpm));
  if (s.inited && s.proj) s.sanitizeTrackSound(track);
  s.tr[track].paramsSent = 0;
}

// VOICE KINDS: the same rules as setVoice. A second voice of the same track and kind
// replaces the first; nullptr removes it. The track rebinds at the next quantum (so an
// instrument of that kind already on the track switches to it there).
void Engine::setAltVoice(int track, int kind, Voice* voice) {
  if (track < 0 || track >= kNumTracks || kind <= kVoiceNative || kind >= kNumVoiceKinds) return;
  engine_detail::State& s = *impl_;
  engine_detail::State::AltVoice* slot = nullptr;
  for (engine_detail::State::AltVoice& a : s.alt)
    if (a.v && a.track == track && a.kind == kind) slot = &a;
  if (!slot)
    for (engine_detail::State::AltVoice& a : s.alt)
      if (!a.v && !slot) slot = &a;
  if (!slot) return;   // kMaxAltVoices in use
  if (s.voice[track] == slot->v && slot->v) s.voice[track] = s.nativeV[track];
  if (s.fadeV == slot->v) s.fadeV = nullptr;
  slot->v = voice;
  slot->track = uint8_t(track);
  slot->kind = uint8_t(kind);
  if (voice) voice->setMonoOutput(s.monoOut);
  if (voice && s.gValid) voice->setTempo(float(s.bpm));
  s.cacheSoundRanges(track);
  s.tr[track].bindValid = 0;
  s.tr[track].paramsSent = 0;
}

// DECISION: switching mode clears the FX tails and the master's filter
// states (the mono path keeps only one channel's state), so call it before
// audio starts, as engine.h says.
void Engine::setMonoOutput(bool mono) {
  engine_detail::State& s = *impl_;
  s.monoOut = mono;
  for (int t = 0; t < kNumTracks; ++t)
    if (s.nativeV[t]) s.nativeV[t]->setMonoOutput(mono);
  for (const engine_detail::State::AltVoice& a : s.alt)
    if (a.v) a.v->setMonoOutput(mono);
  s.delay.setMono(mono);   // mono: always the plain (D-long) delay line
  s.resetFx();
  s.gValid = false;        // re-derive the delay time / mode for the new layout
  for (int t = 0; t < kNumTracks; ++t) { s.tr[t].cPan = -1; s.tr[t].settled = 0; }
}

void Engine::sanitizeProject(Project& p) const { impl_->sanitizeAll(p); }

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
  if (index < kSoundParams) return impl_->nativeV[track] ? impl_->nativeV[track]->paramInfo(index) : nullptr;
  return engine_detail::mixParamInfo(track, index);
}

const ParamInfo* Engine::trackParamInfo(int track, int kind, int index) const {
  if (track < 0 || track >= kNumTracks || index < 0 || index >= kParamsPerTrack) return nullptr;
  if (index >= kSoundParams) return engine_detail::mixParamInfo(track, index);
  const Voice* v = impl_->kindVoice(track, kind);
  return v ? v->paramInfo(index) : nullptr;
}

bool Engine::hostsKind(int track, int kind) const {
  return track >= 0 && track < kNumTracks && kind >= 0 && kind < kNumVoiceKinds && impl_->kindVoice(track, kind);
}

void Engine::process(int16_t* out, int frames) {
  engine_detail::State& s = *impl_;
  // Dekker-style handshake with suspend(): we raise inProcess, THEN look at
  // suspendReq; suspend() raises suspendReq, THEN waits for inProcess to drop.
  // With sequentially consistent atomics one side always sees the other.
  s.inProcess.store(true);
  // After inProcess (both sequentially consistent): setOutputTap(nullptr) either
  // makes this load see nullptr or waits for this call to end.
  OutputTap* const tap = s.outTap.load();
  const int quanta = frames / kQuantum;
  for (int q = 0; q < quanta; ++q, out += 2 * kQuantum) {
    if (!s.inited) {
      std::memset(out, 0, sizeof(int16_t) * 2 * kQuantum);
      continue;
    }
    if (s.suspendReq.load()) {
      // Suspended: the Project is off limits. Fade the last sound out over a
      // few ms from cached state only (no commands, no sequencer, no project
      // reads), then output silence. Everything queued so far is dropped:
      // the tail is read BEFORE re-checking the request, so a command posted
      // after resume() (which clears the request first) is never dropped here.
      if (!s.suspendAck) {
        s.suspendAck = true;
        s.quiesceForSuspend();
      }
      {
        const uint32_t tail = s.qTail.load();
        if (s.suspendReq.load()) s.discardCommands(tail);
      }
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
      s.suspendAck = false;
      // Commands posted before resume() (left over when no process() call
      // ran during the suspend) belong to the old project: drop them.
      s.discardCommands(s.resumeTail.load());
      for (int t = 0; t < kNumTracks; ++t) s.vChoke(t);
      s.resetRuntime();
      s.resetFx();
      s.sanitizeProject();
      s.gValid = false;
      s.bump();
    }
    s.suspendFade = engine_detail::kSuspendFadeQuanta;
    if (tap) {
      const uint32_t bars = s.barCount;
      s.quantum(out);
      tap->quantum(out, kQuantum,
                   uint8_t((s.playing ? OutputTap::kTapPlaying : 0) | (s.barCount != bars ? OutputTap::kTapBar : 0)));
    } else {
      s.quantum(out);
    }
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
  if (t + 1 - h > s.qPeak) s.qPeak = t + 1 - h;
  return true;
}

uint32_t Engine::queuePeak() const { return impl_->qPeak; }

bool Engine::isPlaying() const { return impl_->aPlaying.load(std::memory_order_relaxed); }

uint8_t Engine::instrumentRefreshes() const { return impl_->aInstRefreshes.load(std::memory_order_relaxed); }

void Engine::setOutputTap(OutputTap* tap) {
  engine_detail::State& s = *impl_;
  s.outTap.store(tap);
  if (tap) return;
  // process() loads the tap once per call after raising inProcess: once inProcess
  // has been seen low, no call is inside the old tap (a call that starts later
  // loads nullptr). A call lasts one audio block at most.
  while (s.inProcess.load()) {
  }
}

void Engine::setScope(int track, int decimation) {
  const int d = decimation < 1 ? 1 : (decimation > 255 ? 255 : decimation);
  const uint16_t req = (track >= 0 && track < kNumTracks) ? uint16_t((track + 1) | (d << 8)) : uint16_t(0);
  impl_->scopeReq.store(req, std::memory_order_relaxed);
}

// The ring holds sample j at scopeRing[j % kScopeRing] until sample j + kScopeRing
// is written. The engine writes at most kQuantum samples past the published end
// before it publishes again, so samples [end2 + kQuantum - kScopeRing, end2) are
// intact if end2 is read after the copy.
int Engine::readScope(float* out, int n, ScopeInfo* info) const {
  const engine_detail::State& s = *impl_;
  const std::memory_order rx = std::memory_order_relaxed, acq = std::memory_order_acquire;
  ScopeInfo in;
  const uint16_t req = s.scopeReq.load(rx);
  const uint16_t cfg = s.scopeCfgA.load(acq);   // what the audio thread taps now
  in.hits = s.scopeHits.load(acq);
  in.mark = s.scopeMark.load(rx);
  in.note = 0;
  in.end = s.scopeEnd.load(acq);
  in.since = s.scopeSince.load(rx);
  in.track = (cfg & 0xFF) ? uint8_t((cfg & 0xFF) - 1) : uint8_t(0xFF);
  in.decimation = uint8_t(cfg >> 8) ? uint8_t(cfg >> 8) : uint8_t(1);
  if (in.track < kNumTracks) in.note = s.lastNote[in.track].load(rx);
  in.sampleRate = s.sr;
  if (info) *info = in;
  const int32_t avail = int32_t(in.end - in.since);
  if (n > kScopeReadMax) n = kScopeReadMax;
  if (avail < n) n = avail;
  if (n <= 0 || !out || cfg != req) return 0;   // nothing, or a change the audio thread has not taken yet
  const uint32_t first = in.end - uint32_t(n);
  for (int i = 0; i < n; ++i) out[i] = s.scopeRing[(first + uint32_t(i)) & (kScopeRing - 1)];
  std::atomic_thread_fence(acq);
  const uint32_t end2 = s.scopeEnd.load(acq);
  if (int32_t(end2 + kQuantum - kScopeRing - first) > 0) return 0;          // overwritten while copying
  if (s.scopeSince.load(rx) != in.since || s.scopeCfgA.load(rx) != cfg) return 0;   // the tap changed
  return n;
}

Command setMacroCommand(int track, int macro, uint8_t position, uint16_t rampQuanta) {
  Command c;
  std::memset(&c, 0, sizeof(c));
  c.type = CmdType::SetMacro;
  c.a = uint8_t(track);
  c.b = uint8_t(macro);
  c.c = position;
  c.step.setLock(uint8_t(rampQuanta & 0xFF), uint8_t(rampQuanta >> 8));
  return c;
}

Command macroOffsetCommand(int track, int macro, int offset16, uint16_t rampMs, bool hold) {
  Command c;
  std::memset(&c, 0, sizeof(c));
  c.type = CmdType::SetMacroOffset;
  c.a = uint8_t(track);
  c.b = uint8_t((macro & 3) | (hold ? kOffsetHold : 0));
  const int lim = 255 * kOffsetScale;
  const uint16_t v = uint16_t(int16_t(offset16 < -lim ? -lim : (offset16 > lim ? lim : offset16)));
  c.step.note = uint8_t(v & 0xFF);
  c.step.vel = uint8_t(v >> 8);
  c.step.setLock(uint8_t(rampMs & 0xFF), uint8_t(rampMs >> 8));
  return c;
}

Command setInstrumentCommand(int track, uint16_t id, const uint8_t positions[4]) {
  Command c;
  std::memset(&c, 0, sizeof(c));
  c.type = CmdType::SetInstrument;
  c.a = uint8_t(track);
  c.step.setLock(uint8_t(id & 0xFF), uint8_t(id >> 8));
  c.step.note = positions[0];
  c.step.flagsRaw = positions[1];
  c.step.vel = positions[2];
  c.step.gate = positions[3];
  return c;
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
  st.songRepeat = s.aSongRep.load(rx);
  st.masterStep = s.aMasterStep.load(rx);
  st.writesDone = s.aWritesDone.load(rx);
  for (int t = 0; t < kNumTracks; ++t) {
    st.step[t] = s.aStep[t].load(rx);
    st.trig[t] = s.aTrig[t].load(rx);
  }
  st.peakL = s.aPeakL.exchange(0.0f, rx);
  st.peakR = s.aPeakR.exchange(0.0f, rx);
  st.duckGain = s.aDuck.load(rx);
  st.revision = s.aRevision.load(rx);
  st.instrumentRefreshes = s.aInstRefreshes.load(rx);
  for (int t = 0; t < kNumTracks; ++t) {
    const uint32_t w = s.aMacPos[t].load(rx);
    for (int m = 0; m < kTrackMacros; ++m) st.macroPos[t][m] = uint8_t(w >> (8 * m));
    st.motion[t] = s.aMotion[t].load(rx);
    st.live[t] = s.aLive[t].load(rx);
  }
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
  s.resumeTail.store(s.qTail.load(std::memory_order_relaxed));   // before the epoch bump
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
void setRumbleFilterBypass(bool bypass) {
  if (g_state) g_state->master.setRumbleBypass(bypass);
}
void effectiveParams(int track, uint8_t* out) {
  if (!g_state || track < 0 || track >= kNumTracks) return;
  std::memcpy(out, g_state->tr[track].eff, kParamsPerTrack);
}
uint32_t faultCount() { return g_state ? g_state->faults + g_state->master.faults() : 0; }
float macroPosition(int track, int m) {
  if (!g_state || track < 0 || track >= kNumTracks || m < 0 || m >= kTrackMacros) return -1.0f;
  return g_state->macroPos(g_state->tr[track], m, g_state->qCount - 1u);
}

#if PG_STEM_TAP
void setStemTap(StemTap* tap) {
  if (!g_state) return;
  g_state->tap = tap;
  g_state->tapUpL.reset();
  g_state->tapUpR.reset();
}
void setEndAfterPatternPlays(int plays) {
  if (g_state) g_state->endAfter = plays > 0 ? plays : -1;
}
long endedAtFrame() { return g_state ? g_state->endedAt : -1; }
#endif

}  // namespace engine_detail
}  // namespace pg
