// Built-in test grooves for host tools and system tests. They write straight
// into a Project before the engine starts (no Commands needed), on top of
// Engine::initProject defaults, so they exercise the DEFAULT sound and mix.
#pragma once
#include <cstdint>

#include "pg/engine.h"
#include "pg/project.h"

namespace grooves {

inline pg::Step on(int note, int vel, int gate = 2, uint8_t extra = 0) {
  pg::Step s = pg::kDefaultStep;
  s.note = uint8_t(note);
  s.vel = uint8_t(vel);
  s.gate = uint8_t(gate);
  s.setFlags(uint8_t(pg::kStepOn | extra));
  return s;
}

// "groove": 124 BPM house in A minor, pattern 0, 16 steps.
//   kick   4 on the floor
//   clap   2 and 4
//   ch     16ths, accented 16ths before the off-beat, a few probability ghosts
//   oh     off-beats (each is choked a 16th later by the next closed hat)
//   bass   acid line with slides and accents (A1..E3), default bass patch
//   lead   min7 stabs, two bars (Am7 / Dm7), track length 32
// Snare and perc stay empty. Mixer, FX and ducker at their defaults.
inline void programGroove(pg::Project& p, int bpm = 124) {
  using namespace pg;
  p.globals[kGTempo] = uint8_t(bpm);
  Pattern& pat = p.patterns[0];
  pat.length = 16;
  for (int s = 0; s < 16; s += 4) pat.tracks[kTrackKick].steps[s] = on(60, 112);
  pat.tracks[kTrackClap].steps[4] = on(60, 104);
  pat.tracks[kTrackClap].steps[12] = on(60, 104);
  for (int s = 0; s < 16; ++s) {
    if ((s & 3) == 2) continue;                       // the open hat's steps
    const bool acc = (s & 3) == 1;                    // the "and" before the off-beat drives it
    Step st = on(60, acc ? 100 : 72, 2, acc ? kStepAccent : 0);
    if ((s & 3) == 3) { st.vel = 58; st.setProb(85); }  // ghosts, a little human variation
    pat.tracks[kTrackHatClosed].steps[s] = st;
  }
  for (int s = 2; s < 16; s += 4) pat.tracks[kTrackHatOpen].steps[s] = on(60, 92);

  // Bass: A minor acid. MIDI 33 = A1, 45 = A2.
  struct B { int step, note, vel; uint8_t fl; };
  const B line[] = {
      {2, 45, 110, kStepAccent}, {3, 45, 90, 0},  {5, 48, 92, kStepSlide}, {6, 45, 96, 0},
      {7, 43, 88, 0},            {9, 45, 90, 0},  {10, 52, 112, kStepAccent | kStepSlide}, {11, 50, 92, 0},
      {13, 48, 90, 0},           {14, 45, 112, kStepAccent}, {15, 43, 90, kStepSlide},
  };
  for (const B& b : line) pat.tracks[kTrackBass].steps[b.step] = on(b.note, b.vel, 2, b.fl);

  // Lead: min7 stabs over two bars.
  TrackPattern& ld = pat.tracks[kTrackLead];
  ld.length = 32;
  auto stab = [&](int s, int root, int vel, int gate) {
    Step st = on(root, vel, gate);
    st.setChord(kChordMin7);
    ld.steps[s] = st;
  };
  stab(3, 57, 96, 2);  stab(6, 57, 84, 2);  stab(10, 57, 100, 3);
  stab(19, 62, 96, 2); stab(22, 62, 84, 2); stab(26, 62, 100, 3); stab(30, 60, 88, 2);
}

// "busy": every track plays on most 16ths, ratchets and p-locks here and
// there, every send turned up, delay/reverb long, DJ filter, drive and the
// ducker on. For CPU benchmarks and stress tests (worst case, not musical).
inline void programBusy(pg::Project& p, int bpm = 128) {
  using namespace pg;
  p.globals[kGTempo] = uint8_t(bpm);
  p.globals[kGSwing] = 12;
  p.globals[kGReverbSize] = 110;
  p.globals[kGDelayFeedback] = 90;
  p.globals[kGMasterFilter] = 40;    // low-pass engaged
  p.globals[kGMasterReso] = 80;
  p.globals[kGMasterDrive] = 50;
  Pattern& pat = p.patterns[0];
  pat.length = 16;
  for (int t = 0; t < kNumTracks; ++t) {
    p.tracks[t].params[kMixDelaySend] = 70;
    p.tracks[t].params[kMixReverbSend] = 70;
    p.tracks[t].params[kMixDuck] = t == kTrackKick ? 0 : 80;
    TrackPattern& tp = pat.tracks[t];
    for (int s = 0; s < 16; ++s) {
      const bool hit = t == kTrackKick ? (s % 4 == 0) : (t == kTrackSnare || t == kTrackClap) ? (s % 4 == 2) : true;
      if (!hit) continue;
      Step st = on(t == kTrackBass ? 36 + (s * 5) % 12 : (t == kTrackLead ? 60 + (s * 7) % 12 : 60), 80 + (s * 13) % 47, 3);
      if (s % 3 == 0) st.addFlags(kStepAccent);
      if (t >= kTrackBass && s % 5 == 4) st.addFlags(kStepSlide);
      if (s == 7 || s == 15) st.addFlags(uint8_t(2 << kStepRatchetShift));
      if (t == kTrackLead) { st.setChord((s & 1) ? kChordMin9 : kChordMin7); st.gate = 6; }   // 4-note chords
      if (s % 4 == 1) { st.setLock(1, uint8_t(20 + s * 6)); }   // sound param 0 p-lock
      if (s % 6 == 5) st.setProb(60);                                                 // seed-dependent hits
      tp.steps[s] = st;
    }
  }
}

}  // namespace grooves
