#include "demo.h"

#include <cstring>

namespace pg::demo {

namespace {

void on(Step& s, int vel, int note = 60, bool acc = false, bool slide = false, int prob = 100, int ratch = 1,
        int chord = 0, int gate = 2) {
  s = kDefaultStep;
  s.flags = uint8_t(kStepOn | (acc ? kStepAccent : 0) | (slide ? kStepSlide : 0) |
                    ((ratch - 1) << kStepRatchetShift));
  s.vel = uint8_t(vel);
  s.note = uint8_t(note);
  s.prob = uint8_t(prob);
  s.chord = uint8_t(chord);
  s.gate = uint8_t(gate);
}

// P01 "groove": exactly the pattern of the UX mockups (screens.demo_pattern()).
void groove(Pattern& P) {
  auto st = [&](int t, int i) -> Step& { return P.tracks[t].steps[i]; };
  for (int i : {0, 4, 8, 12}) on(st(0, i), 110, 60, i == 0);
  on(st(1, 12), 96);
  on(st(1, 15), 60, 60, false, false, 50);
  on(st(1, 7), 50, 60, false, false, 40);
  for (int i : {4, 12}) on(st(2, i), 105);
  const int hv[4] = {100, 55, 80, 55};
  for (int i = 0; i < 16; ++i) on(st(3, i), hv[i % 4]);
  on(st(3, 15), 80, 60, false, false, 100, 2);
  for (int i : {2, 6, 10, 14}) on(st(4, i), 95);
  on(st(5, 3), 80);
  on(st(5, 9), 70, 60, false, false, 60);
  on(st(5, 14), 85, 63);
  const int A1 = 33;
  on(st(6, 0), 100, A1, true);
  on(st(6, 2), 100, A1);
  on(st(6, 3), 100, A1 + 12, false, true);
  on(st(6, 5), 100, A1 + 3);
  on(st(6, 6), 100, A1);
  on(st(6, 8), 100, A1, true);
  on(st(6, 10), 100, A1 - 2);
  on(st(6, 11), 100, A1, false, true);
  on(st(6, 13), 100, A1 + 7, true);
  st(6, 13).lockParam = 1;   // CUT 110
  st(6, 13).lockValue = 110;
  on(st(6, 14), 100, A1 + 5);
  on(st(7, 2), 100, 57, false, false, 100, 1, kChordMin7, 3);
  on(st(7, 7), 100, 57, false, false, 100, 1, kChordMin7, 2);
  on(st(7, 10), 100, 53, false, false, 100, 1, kChordMaj7, 6);
}

}  // namespace

int count() { return 1; }

const char* name(int i) { return i == 0 ? "ACIDJAM" : nullptr; }

void build(const Engine& engine, Project& p, int i) {
  engine.initProject(p);
  if (i != 0) return;
  std::memset(p.name, 0, sizeof(p.name));
  std::memcpy(p.name, "ACIDJAM", 7);
  groove(p.patterns[0]);
  // P02 "lift": open hats doubled, the bass an octave-jump variant, hat ratchets at the end
  p.patterns[1] = p.patterns[0];
  for (int i2 : {0, 4, 8, 12}) on(p.patterns[1].tracks[4].steps[i2 + 3], 70);
  for (int i2 : {3, 8, 14}) p.patterns[1].tracks[6].steps[i2].note = uint8_t(p.patterns[1].tracks[6].steps[i2].note + 12);
  for (int i2 = 12; i2 < 16; ++i2) on(p.patterns[1].tracks[3].steps[i2], 70 + 8 * (i2 - 12), 60, false, false, 100, 2);
  // P03 "break": no kick, MIN9 chords held, bass on 1 only
  p.patterns[2] = p.patterns[0];
  for (int i2 = 0; i2 < kMaxSteps; ++i2) {
    p.patterns[2].tracks[0].steps[i2] = kDefaultStep;
    p.patterns[2].tracks[6].steps[i2].flags &= uint8_t(~kStepOn);
    p.patterns[2].tracks[7].steps[i2] = kDefaultStep;
  }
  p.patterns[2].tracks[6].steps[0].flags |= kStepOn;
  on(p.patterns[2].tracks[7].steps[0], 90, 57, false, false, 100, 1, kChordMin9, 16);
  on(p.patterns[2].tracks[7].steps[8], 90, 53, false, false, 100, 1, kChordMin9, 16);
  // P04 "fill": snare 13-16 ratcheted, accented kick
  p.patterns[3] = p.patterns[0];
  for (int i2 = 12; i2 < 16; ++i2)
    on(p.patterns[3].tracks[1].steps[i2], 80 + 10 * (i2 - 12), 60, i2 == 15, false, 100, i2 == 12 ? 1 : i2 - 11);
  on(p.patterns[3].tracks[0].steps[14], 120, 60, true);
  // Sound (UX 14)
  uint8_t* b = p.tracks[kTrackBass].params;
  b[0] = 58; b[1] = 96; b[2] = 84; b[3] = 40; b[4] = 100; b[5] = 52; b[8] = 0; b[9] = 70; b[13] = 24;
  uint8_t* l = p.tracks[kTrackLead].params;
  l[0] = 70; l[1] = 24; l[4] = 3; l[6] = 64; l[11] = 60;
  // Globals: A minor, 124 BPM, swing 54 %, ducker on the kick, delay 1/8D (default), DJ filter off
  p.globals[kGTempo] = 124;
  p.globals[kGSwing] = 8;
  p.globals[kGDuckDepth] = 100;
  p.globals[kGDuckHold] = 30;
  p.globals[kGDuckRelease] = 70;
  p.globals[kGDuckCurve] = 80;
  p.globals[kGReverbSize] = 70;
  p.globals[kGMasterDrive] = 20;
  p.globals[kGScaleRoot] = 9;
  p.globals[kGScaleType] = 2;
  // Song: P01 x4, P02 x4, P03 x2, P04 x1, END (song mode off at boot)
  const uint8_t chain[4][2] = {{0, 4}, {1, 4}, {2, 2}, {3, 1}};
  for (int s = 0; s < 4; ++s) { p.song[s].pattern = chain[s][0]; p.song[s].repeats = chain[s][1]; }
  p.songMode = 0;
}

}  // namespace pg::demo
