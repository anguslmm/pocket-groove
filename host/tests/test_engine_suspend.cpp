// Engine::suspend()/resume() for project loads: commands queued before the
// suspend is acknowledged, or posted while suspended, are discarded (never
// applied to the newly loaded project), dropped WriteTrack/WritePattern still
// bump writesDone, rolls stop and held notes are released; commands posted
// after resume() are applied normally.
#include <cstring>

#include "engine_test_util.h"

using namespace pg;
using namespace et;

static Rig rig;

static Command setStep(int pattern, int track, int s) {
  Command c = cmd(CmdType::SetStep, pattern, track, s);
  c.step = mkStep(60);
  return c;
}
static bool stepOn(int pattern, int track, int s) { return (rig.step(pattern, track, s).flags & kStepOn) != 0; }

// 1. The audio thread keeps running while suspended (the normal device case).
static void testDiscardWhileRunning() {
  static TrackPattern bufA, bufB;
  std::memset(&bufA, 0, sizeof bufA);
  bufA.length = 16;
  for (int s = 0; s < kMaxSteps; ++s) bufA.steps[s] = mkStep(60);
  bufB = bufA;
  rig.init();
  rig.run(10);
  const uint8_t w0 = rig.eng.status().writesDone;
  // Queued, not yet consumed when the suspend is acknowledged.
  rig.post(setStep(0, kTrackSnare, 1));
  Command wt = cmd(CmdType::WriteTrack, 2, kTrackClap);
  wt.ptr = &bufA;
  rig.post(wt);
  rig.eng.suspend();
  // Posted while suspended.
  rig.post(setStep(0, kTrackSnare, 2));
  Command wp = cmd(CmdType::WriteTrack, 3, kTrackClap);
  wp.ptr = &bufB;
  rig.post(wp);
  rig.run(20);
  // The UI may wait for its buffers while still suspended.
  CHECKF(uint8_t(rig.eng.status().writesDone - w0) == 2, "writesDone advanced by %d while suspended, expected 2",
         int(uint8_t(rig.eng.status().writesDone - w0)));
  rig.post(setStep(0, kTrackSnare, 4));     // still suspended: dropped as well
  rig.run(5);
  rig.eng.initProject(rig.proj);             // "load" a new project
  rig.eng.resume();
  rig.post(setStep(0, kTrackSnare, 3));     // after resume: kept
  rig.run(10);
  CHECK(!stepOn(0, kTrackSnare, 1) && !stepOn(0, kTrackSnare, 2) && !stepOn(0, kTrackSnare, 4));
  CHECK(stepOn(0, kTrackSnare, 3));
  CHECK(!(rig.proj.patterns[2].tracks[kTrackClap].steps[0].flags & kStepOn));
  CHECK(!(rig.proj.patterns[3].tracks[kTrackClap].steps[0].flags & kStepOn));
  CHECK(uint8_t(rig.eng.status().writesDone - w0) == 2);
  std::printf("  suspend with a running audio thread: 5 stale commands dropped (2 writes counted), post-resume edit applied\n");
}

// 2. No process() call at all between suspend() and resume() (audio stopped):
//    the stale commands are dropped on the first quantum after resume.
static void testDiscardWithoutAudio() {
  static TrackPattern buf;
  std::memset(&buf, 0, sizeof buf);
  buf.length = 16;
  for (int s = 0; s < kMaxSteps; ++s) buf.steps[s] = mkStep(60);
  rig.init();
  rig.run(4);
  const uint8_t w0 = rig.eng.status().writesDone;
  rig.post(setStep(0, kTrackPerc, 5));
  rig.eng.suspend();
  Command wt = cmd(CmdType::WriteTrack, 1, kTrackPerc);
  wt.ptr = &buf;
  rig.post(wt);
  rig.post(cmd(CmdType::Play));
  rig.eng.initProject(rig.proj);
  rig.eng.resume();
  rig.post(setStep(0, kTrackPerc, 6));
  rig.run(4);
  const EngineStatus st = rig.eng.status();
  CHECK(!stepOn(0, kTrackPerc, 5) && stepOn(0, kTrackPerc, 6));
  CHECK(!(rig.proj.patterns[1].tracks[kTrackPerc].steps[0].flags & kStepOn));
  CHECK(!st.playing);
  CHECK(uint8_t(st.writesDone - w0) == 1);
  // A WritePattern dropped the same way counts too.
  static Pattern pat;
  std::memset(&pat, 0, sizeof pat);
  pat.length = 16;
  rig.eng.suspend();
  Command wp = cmd(CmdType::WritePattern, 4);
  wp.ptr = &pat;
  rig.post(wp);
  rig.eng.resume();
  rig.run(2);
  CHECK(uint8_t(rig.eng.status().writesDone - w0) == 2);
  std::printf("  suspend without audio callbacks: stale commands dropped at resume, writes counted\n");
}

// 3. Rolls stop and held live notes are released; nothing restarts after resume.
static void testRollAndHeldNotes() {
  rig.init();
  rig.neutralMix();
  rig.v[kTrackBass].mode = StubVoice::Gate;
  rig.v[kTrackBass].amp = 0.3f;
  Command roll = cmd(CmdType::Roll, kTrackHatClosed, 0, 2);   // 1/16 roll, transport stopped: free-running
  roll.step = mkStep(60);
  rig.post(roll);
  Command on = cmd(CmdType::NoteOn, kTrackBass, 45, 100);
  on.step = mkStep(45);
  rig.post(on);                                                // held: no NoteOff
  rig.runSeconds(0.5);
  const size_t rollHits = rig.v[kTrackHatClosed].ons().size();
  CHECKF(rollHits >= 3, "roll did not run (%zu hits)", rollHits);
  CHECK(rig.v[kTrackBass].ons().size() == 1);
  rig.eng.suspend();
  rig.run(20);
  const auto offs = rig.v[kTrackBass].offs().size() + rig.v[kTrackBass].chokes().size();
  CHECKF(offs >= 1, "held live note not released on suspend");
  rig.eng.resume();
  rig.runSeconds(1.0);
  CHECKF(rig.v[kTrackHatClosed].ons().size() == rollHits, "roll kept firing after suspend/resume (%zu -> %zu)", rollHits,
         rig.v[kTrackHatClosed].ons().size());
  CHECK(engine_detail::faultCount() == 0);
  std::printf("  suspend: roll stopped (%zu hits before), held note released\n", rollHits);
}

// isPlaying(): the transport flag without status()'s side effect (peak-hold reset).
static void testIsPlaying() {
  rig.init();
  rig.v[kTrackBass].mode = StubVoice::Dc;
  rig.v[kTrackBass].amp = 0.5f;
  CHECK(!rig.eng.isPlaying());
  rig.post(cmd(CmdType::Play));
  rig.run(20);
  CHECK(rig.eng.isPlaying() && rig.eng.isPlaying());
  CHECK(rig.eng.status().peakL > 0.1f);      // isPlaying() left the peak hold alone
  rig.post(cmd(CmdType::Stop));
  rig.run(2);
  CHECK(!rig.eng.isPlaying());
}

int main() {
  std::printf("engine suspend/resume command semantics\n");
  testIsPlaying();
  testDiscardWhileRunning();
  testDiscardWithoutAudio();
  testRollAndHeldNotes();
  return finish("test_engine_suspend");
}
