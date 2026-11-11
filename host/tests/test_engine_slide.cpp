// TB-303 slide semantics, every edge case, checked against the exact order
// and time of the calls the voice receives.
//
// Log notation used below (time in steps):
//   +40@0.00    noteOn  note 40, slide = false
//   +43s@1.00   noteOn  note 43, slide = true (legato)
//   -43@1.50    noteOff note 43
//   X@0.50      choke
#include <string>

#include "tests/engine_test_util.h"

using namespace et;
using namespace pg;

static Rig rig;
static const int A = 40, B = 43, C = 47, D = 52, E = 55;

static std::string logOf(const StubVoice& v, double L) {
  std::string s;
  char buf[48];
  for (const Call& c : v.calls) {
    const double t = double(c.t) / L;
    if (c.type == Call::On) std::snprintf(buf, sizeof(buf), "+%d%s@%.2f ", c.ev.note, c.ev.slide ? "s" : "", t);
    else if (c.type == Call::Off) std::snprintf(buf, sizeof(buf), "-%d@%.2f ", c.note, t);
    else std::snprintf(buf, sizeof(buf), "X@%.2f ", t);
    s += buf;
  }
  if (!s.empty()) s.pop_back();
  return s;
}

static void expectLog(const char* what, int track, const char* expected) {
  const std::string got = logOf(rig.v[track], rig.stepLen());
  ++g_checks;
  if (got != expected) {
    fail(__FILE__, __LINE__, "%s\n     expected: %s\n     got:      %s", what, expected, got.c_str());
  } else {
    std::printf("  ok  %-46s %s\n", what, got.c_str());
  }
}

static Step st(int note, int gate = 2, bool slide = false, int ratchet = 1, int prob = 100) {
  return mkStep(note, kStepOn | (slide ? kStepSlide : 0) | ((ratchet - 1) << kStepRatchetShift), 100, gate, prob);
}
static void fresh(int patternLen = 16) {
  rig.init(44100.0f, 124);
  rig.proj.patterns[0].length = uint8_t(patternLen);
}
static void go(double steps) {
  rig.post(CmdType::Play);
  rig.runSamples(long(steps * rig.stepLen()));
}

int main() {
  const int T = kTrackBass;

  fresh();
  rig.step(0, T, 0) = st(A, 2, true);
  rig.step(0, T, 1) = st(B, 2);
  go(4);
  expectLog("basic slide", T, "+40@0.00 +43s@1.00 -43@1.50");

  fresh();
  rig.step(0, T, 0) = st(A, 2, true);
  go(4);
  expectLog("slide into a rest", T, "+40@0.00 -40@1.00");

  fresh();
  rig.step(0, T, 0) = st(A, 2, true);
  rig.step(0, T, 1) = st(B, 2, false, 1, 0);
  go(4);
  expectLog("slide into a step that loses its roll", T, "+40@0.00 -40@1.00");

  fresh();
  rig.step(0, T, 0) = st(A, 2, true);
  rig.step(0, T, 1) = st(B, 2);
  rig.post(CmdType::Play);
  rig.runSamples(long(0.5 * rig.stepLen()));
  rig.post(CmdType::SetMute, T, 0, 1);
  rig.runSamples(long(3.0 * rig.stepLen()));
  expectLog("slide into a muted step", T, "+40@0.00 -40@1.00");

  fresh();
  rig.step(0, T, 0) = st(A, 2, true);
  rig.step(0, T, 1) = st(B, 2, true);
  rig.step(0, T, 2) = st(C, 2, true);
  rig.step(0, T, 3) = st(D, 2);
  go(6);
  expectLog("slide chain", T, "+40@0.00 +43s@1.00 +47s@2.00 +52s@3.00 -52@3.50");

  fresh();
  rig.step(0, T, 0) = st(A, 2, true);
  rig.step(0, T, 1) = st(A, 2);
  go(4);
  expectLog("slide to the same pitch (tie)", T, "+40@0.00 +40s@1.00 -40@1.50");

  fresh();
  rig.step(0, T, 0) = st(A, 4);
  rig.step(0, T, 1) = st(B, 2, true);
  rig.step(0, T, 2) = st(C, 4);
  rig.step(0, T, 3) = st(D, 1);
  go(6);
  expectLog("slide flag only affects the NEXT note", T, "+40@0.00 -40@1.00 +43@1.00 +47s@2.00 -47@3.00 +52@3.00 -52@3.25");

  // ---- wraps ----
  fresh(4);
  rig.step(0, T, 0) = st(A, 2);
  rig.step(0, T, 3) = st(E, 2, true);
  go(6);
  expectLog("slide across the pattern wrap", T, "+40@0.00 -40@0.50 +55@3.00 +40s@4.00 -40@4.50");

  fresh(16);
  rig.proj.patterns[0].tracks[T].length = 3;
  rig.step(0, T, 0) = st(A, 2);
  rig.step(0, T, 2) = st(E, 2, true);
  go(4.9);
  expectLog("slide across a polymeter track wrap", T, "+40@0.00 -40@0.50 +55@2.00 +40s@3.00 -40@3.50");

  fresh(4);
  rig.proj.patterns[1].length = 4;
  rig.step(0, T, 3) = st(E, 2, true);
  rig.step(1, T, 0) = st(B, 2);
  rig.post(CmdType::Play);
  rig.runSamples(long(1.5 * rig.stepLen()));
  rig.post(CmdType::QueuePattern, 1);
  rig.runSamples(long(4.0 * rig.stepLen()));
  expectLog("slide into a queued pattern", T, "+55@3.00 +43s@4.00 -43@4.50");
  CHECK(rig.eng.status().pattern == 1);

  fresh(4);
  rig.proj.patterns[1].length = 4;
  rig.step(0, T, 3) = st(E, 2, true);          // next pattern starts with a rest
  rig.post(CmdType::Play);
  rig.runSamples(long(1.5 * rig.stepLen()));
  rig.post(CmdType::QueuePattern, 1);
  rig.runSamples(long(4.0 * rig.stepLen()));
  expectLog("slide into a queued pattern's rest", T, "+55@3.00 -55@4.00");

  fresh(3);                                     // master boundary cuts the track short
  rig.step(0, T, 0) = st(A, 2);
  rig.step(0, T, 2) = st(E, 2, true);
  rig.step(0, T, 3) = st(D, 2);                 // never reached
  go(4.9);
  expectLog("slide across a master-length reset", T, "+40@0.00 -40@0.50 +55@2.00 +40s@3.00 -40@3.50");

  // ---- ratchets ----
  fresh();
  rig.step(0, T, 0) = st(A, 4, true, 3);
  rig.step(0, T, 1) = st(B, 2);
  go(4);
  expectLog("ratchet x3 on the slide step", T, "+40@0.00 -40@0.33 +40@0.33 -40@0.67 +40@0.67 +43s@1.00 -43@1.50");

  fresh();
  rig.step(0, T, 0) = st(A, 2, true, 2);
  go(4);
  expectLog("ratchet x2 slide step into a rest", T, "+40@0.00 -40@0.25 +40@0.50 -40@1.00");

  fresh();
  rig.step(0, T, 0) = st(A, 2, true);
  rig.step(0, T, 1) = st(B, 4, false, 2);
  go(4);
  expectLog("slide into a ratchet x2 step", T, "+40@0.00 +43s@1.00 -43@1.50 +43@1.50 -43@2.00");

  fresh();
  rig.step(0, T, 0) = st(A, 2, true, 2);
  rig.step(0, T, 1) = st(B, 2, true, 2);
  rig.step(0, T, 2) = st(C, 2);
  go(5);
  expectLog("ratchet slide into ratchet slide", T, "+40@0.00 -40@0.25 +40@0.50 +43s@1.00 -43@1.25 +43@1.50 +47s@2.00 -47@2.50");

  // ---- long gates ----
  fresh();
  rig.step(0, T, 0) = st(A, 10, true);
  go(5);
  expectLog("long-gate slide, nothing follows", T, "+40@0.00 -40@2.50");

  fresh();
  rig.step(0, T, 0) = st(A, 12, true);
  rig.step(0, T, 2) = st(B, 2);
  go(5);
  expectLog("long-gate slide over a rest into a note", T, "+40@0.00 +43s@2.00 -43@2.50");

  fresh();
  rig.step(0, T, 0) = st(A, 8, true);          // gate ends exactly where step 2 fires
  rig.step(0, T, 2) = st(B, 2);
  go(5);
  expectLog("gate ending exactly on the next note", T, "+40@0.00 +43s@2.00 -43@2.50");

  fresh();
  rig.step(0, T, 0) = st(A, 6, true);          // gate 1.5 steps, next note at step 2: already released
  rig.step(0, T, 2) = st(B, 2);
  go(5);
  expectLog("slide note released before the next note", T, "+40@0.00 -40@1.50 +43@2.00 -43@2.50");

  // ---- swing ----
  fresh();
  rig.proj.globals[kGSwing] = 50;
  rig.eng.init(44100.0f, &rig.proj);
  rig.step(0, T, 0) = st(A, 2, true);
  rig.step(0, T, 1) = st(B, 2);
  rig.step(0, T, 2) = st(C, 2, true);
  go(5);
  // step 1 is swung to 1.50; step 3 (a rest) is swung to 3.50
  expectLog("slide with swing (held to the swung step)", T, "+40@0.00 +43s@1.50 -43@2.00 +47@2.00 -47@3.50");

  // ---- transport ----
  fresh();
  rig.step(0, T, 0) = st(A, 2, true);
  rig.step(0, T, 1) = st(B, 2);
  rig.post(CmdType::Play);
  rig.runSamples(long(0.5 * rig.stepLen()));
  rig.post(CmdType::Stop);
  rig.runSamples(long(3.0 * rig.stepLen()));
  expectLog("stop while holding a slide", T, "+40@0.00 -40@0.50 X@0.50");

  fresh();
  rig.step(0, T, 0) = st(A, 2, true);
  rig.step(0, T, 1) = st(B, 2);
  rig.post(CmdType::Play);
  rig.runSamples(long(0.5 * rig.stepLen()));
  rig.post(CmdType::Play);                      // restart from the top
  rig.runSamples(long(0.3 * rig.stepLen()));
  {
    const auto& c = rig.v[T].calls;
    CHECKF(c.size() == 3 && c[0].type == Call::On && c[1].type == Call::Off && c[1].note == A && c[2].type == Call::On &&
               !c[2].ev.slide && c[1].t == c[2].t,
           "restart while holding a slide: %s", logOf(rig.v[T], rig.stepLen()).c_str());
  }

  // ---- other track types ----
  fresh();
  rig.step(0, kTrackKick, 0) = st(60, 2, true);
  rig.step(0, kTrackKick, 1) = st(62, 2);
  go(3);
  expectLog("drums ignore the slide flag", kTrackKick, "+60@0.00 -60@0.50 +62@1.00 -62@1.50");

  fresh();
  rig.step(0, kTrackLead, 0) = st(A, 2, true);
  rig.step(0, kTrackLead, 1) = st(B, 2);
  go(3);
  expectLog("lead track slides the same way", kTrackLead, "+40@0.00 +43s@1.00 -43@1.50");

  fresh();
  rig.step(0, kTrackLead, 0) = st(A, 8);
  rig.step(0, kTrackLead, 1) = st(B, 2);
  go(4);
  expectLog("lead: different pitches may overlap", kTrackLead, "+40@0.00 +43@1.00 -43@1.50 -40@2.00");

  fresh();
  rig.step(0, kTrackLead, 0) = st(A, 8);
  rig.step(0, kTrackLead, 1) = st(A, 2);
  go(4);
  expectLog("lead: same pitch is released before retrigger", kTrackLead, "+40@0.00 -40@1.00 +40@1.00 -40@1.50");

  fresh();
  for (int s = 0; s < 5; ++s) rig.step(0, kTrackLead, s) = st(60 + s, 40);
  go(6);
  {
    // 5 notes with 10-step gates on 4 pending slots: the 5th releases the earliest one.
    const auto& c = rig.v[kTrackLead].calls;
    CHECKF(c.size() == 6 && c[4].type == Call::Off && c[4].note == 60 && c[5].type == Call::On && c[5].ev.note == 64,
           "lead voice stealing: %s", logOf(rig.v[kTrackLead], rig.stepLen()).c_str());
  }

  fresh();
  rig.step(0, kTrackBass, 0) = st(A, 8);        // mono: a new note always ends the old one first
  rig.step(0, kTrackBass, 1) = st(B, 2);
  go(4);
  expectLog("bass: non-slide notes never overlap", kTrackBass, "+40@0.00 -40@1.00 +43@1.00 -43@1.50");

  // Accent travels with a slid note.
  fresh();
  rig.step(0, T, 0) = st(A, 2, true);
  rig.step(0, T, 1) = mkStep(B, kStepOn | kStepAccent, 90, 2);
  go(3);
  {
    const auto o = rig.v[T].ons();
    CHECK(o.size() == 2 && o[1].ev.slide && o[1].ev.accent && o[1].ev.vel == 90);
  }
  return finish("test_engine_slide");
}
