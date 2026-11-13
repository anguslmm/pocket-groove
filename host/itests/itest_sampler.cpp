// The sampler instrument through the whole Instrument (stage F1): the real
// SamplerVoice on PERC behind the built-in sampler sets (CHOP, TAPE, STUTTER).
//
//   switch    a native perc instrument (TRAIL) mid-pattern -> CHOP -> TAPE -> STUTTER
//             -> TAP -> CLASSIC: the sampler plays the steps only while a sampler set
//             is on, it is silent after a switch away, no NaN, no wild jump
//   slices    CHOP's SLICE as each note's own macro value: each note plays its slice
//             (the voice's playhead sits inside it)
//   slot      SetSampleSlot: the next note plays that slot (an empty slot: ROM slot % 3);
//             a user sample in a MemoryPool plays instead of the ROM
//   sanitize  sampleSlot out of range -> 0; a version-1 project zeroes it, version 2 keeps it
//   step note the step's note transposes the chop (playback rate)
#include <cmath>
#include <cstring>
#include <vector>

#include "itests/itest_util.h"
#include "pg/macro/macro.h"
#include "pg/sampler/rom_samples.h"
#include "pg/sampler/sample_pool.h"

using namespace pg;
namespace pm = pg::macro;

namespace {

Instrument g_inst;

const pm::MacroSet* builtinNamed(int track, const char* name) {
  for (int i = 0; i < pm::instrumentCount(track); ++i)
    if (std::strcmp(pm::instrumentAt(track, i)->name, name) == 0) return pm::instrumentAt(track, i);
  return nullptr;
}

void post(const Command& c) { g_inst.engine().post(c); }

void setInst(const pm::MacroSet* s) {
  uint8_t p[4] = {0, 0, 0, 0};
  for (int m = 0; m < 4 && s; ++m) p[m] = m < s->numMacros ? pm::startByte(s->macros[m]) : 0;
  post(setInstrumentCommand(kTrackPerc, s ? s->id : 0, p));
}

void quanta(int n, it::Stats* st = nullptr) {
  int16_t buf[2 * kQuantum];
  for (int i = 0; i < n; ++i) {
    g_inst.process(buf, kQuantum);
    if (st) st->add(buf, kQuantum);
  }
}

Step note(int n, int gate = 4) {
  Step s = kDefaultStep;
  s.setFlags(kStepOn);
  s.note = uint8_t(n);
  s.gate = uint8_t(gate);
  return s;
}

void setStep(int i, const Step& s) { post(Command{CmdType::SetStep, 0, uint8_t(kTrackPerc), uint8_t(i), s, nullptr}); }

void pattern() {
  for (int i = 0; i < 16; ++i) setStep(i, i % 4 == 0 ? note(60) : kDefaultStep);
  post(Command{CmdType::SetGlobal, uint8_t(kGTempo), 0, 120, kDefaultStep, nullptr});
}

void play() { post(Command{CmdType::Play, 0, 0, 0, kDefaultStep, nullptr}); }
void stop() { post(Command{CmdType::Stop, 0, 0, 0, kDefaultStep, nullptr}); }

// Did the sampler sound during the next n quanta? (its playhead seen at any quantum)
bool samplerSounds(int n, it::Stats* st) {
  bool any = false;
  int16_t buf[2 * kQuantum];
  for (int i = 0; i < n; ++i) {
    g_inst.process(buf, kQuantum);
    st->add(buf, kQuantum);
    any |= g_inst.sampler().playhead() >= 0;
  }
  return any;
}

void testSwitch() {
  const char* names[] = {"TRAIL", "CHOP", "TAPE", "STUTTER", "TAP"};
  pattern();
  play();
  it::Stats st;
  for (const char* n : names) {
    const pm::MacroSet* s = builtinNamed(kTrackPerc, n);
    it::check(s != nullptr, "built-in %s", n);
    if (!s) continue;
    setInst(s);
    quanta(40, &st);   // let the old voice fade
    const bool sounds = samplerSounds(5000, &st);   // ~1.8 s: several notes
    const bool want = s->voice == kVoiceSampler;
    it::check(sounds == want, "%s: the sampler %s", n, want ? "plays" : "is silent");
    if (want) it::check(!g_inst.voice(kTrackPerc)->active(), "%s: the native perc voice is silent", n);
  }
  setInst(nullptr);
  quanta(40, &st);
  it::check(!samplerSounds(3000, &st), "CLASSIC: the sampler is silent");
  it::check(st.peak > 1000 && st.minInt == 0, "it all played (peak %d)", st.peak);
  it::check(st.maxJump < 20000, "no wild jump (%d)", st.maxJump);
  stop();
  quanta(20);
}

void testSlicesPerNote() {
  const pm::MacroSet* chop = builtinNamed(kTrackPerc, "CHOP");
  if (!chop) return;
  setInst(chop);
  // four notes, each with its own SLICE (macro 0): slices 3, 9, 0, 14
  const int slices[4] = {3, 9, 0, 14};
  for (int i = 0; i < 16; ++i) {
    Step s = i % 4 == 0 ? note(60, 8) : kDefaultStep;
    if (i % 4 == 0) s.setMacroValue(0, (slices[i / 4] * 255 + 8) / 15);   // a position inside that slice's step
    setStep(i, s);
  }
  quanta(4);
  play();
  const uint32_t len = sampler::kRom[0].length;
  int hits = 0;
  // sample each note shortly after it starts (one 16th = 5512 frames at 120 BPM)
  for (int n = 0; n < 4; ++n) {
    quanta(n == 0 ? 60 : 4 * 5512 / kQuantum - 0);
    const int32_t ph = g_inst.sampler().playhead();
    const int k = slices[n];
    const bool in = ph >= int32_t(len * uint32_t(k) / 16) && ph < int32_t(len * uint32_t(k + 1) / 16);
    it::check(in, "note %d plays slice %d (playhead %d of %u)", n, k + 1, ph, len);
    hits += in;
  }
  stop();
  quanta(20);
  it::check(hits == 4, "every note its own slice");
}

void testSlotAndPool() {
  Project& p = g_inst.project();
  const pm::MacroSet* tape = builtinNamed(kTrackPerc, "TAPE");
  if (!tape) return;
  setInst(tape);
  pattern();
  post(Command{CmdType::SetSampleSlot, uint8_t(kTrackPerc), 0, 4, kDefaultStep, nullptr});
  quanta(2);
  it::check(p.tracks[kTrackPerc].sampleSlot == 4, "SetSampleSlot stores the slot");
  post(Command{CmdType::SetSampleSlot, uint8_t(kTrackPerc), 0, 12, kDefaultStep, nullptr});
  quanta(2);
  it::check(p.tracks[kTrackPerc].sampleSlot == 4, "an out-of-range slot is refused");
  play();
  quanta(100);
  it::check(g_inst.sampler().playingSlot() == 4, "the note plays slot 5 (%d)", g_inst.sampler().playingSlot());
  sampler::SampleRef r;
  sampler::resolve(g_inst.sampler().pool(), 4, &r);
  it::check(r.builtin && r.rom == 4 % sampler::kRomCount, "an empty slot: the ROM sound");
  stop();
  quanta(20);
  // a user sample in RAM: a 441 Hz sine
  static sampler::MemoryPool pool;
  static std::vector<int16_t> sine(sampler::kSlotMaxSamples, 0);
  for (uint32_t i = 0; i < 40000; ++i)
    sine[i] = int16_t(std::lrint(20000.0 * std::sin(2.0 * 3.14159265358979 * 441.0 * double(i) / 22050.0)));
  pool.set(4, sine.data(), 40000, "SINE");
  g_inst.engine().suspend();
  g_inst.setSamplePool(&pool);
  g_inst.engine().resume();
  play();
  quanta(100);
  sampler::resolve(g_inst.sampler().pool(), 4, &r);
  it::check(!r.builtin && r.length == 40000 && std::strcmp(r.name, "SINE") == 0, "the user sample resolves");
  it::check(g_inst.sampler().playhead() > 0 && g_inst.sampler().playhead() < 40000, "and plays (%d)",
            g_inst.sampler().playhead());
  stop();
  quanta(20);
  g_inst.engine().suspend();
  g_inst.setSamplePool(nullptr);
  g_inst.engine().resume();
}

void testSanitize() {
  Engine& e = g_inst.engine();
  static Project p;
  e.initProject(p);
  p.tracks[kTrackPerc].sampleSlot = 200;
  e.sanitizeProject(p);
  it::check(p.tracks[kTrackPerc].sampleSlot == 0, "out of range -> slot 1");
  p.tracks[kTrackPerc].sampleSlot = 7;
  p.version = 2;
  e.sanitizeProject(p);
  it::check(p.tracks[kTrackPerc].sampleSlot == 7, "a version-2 file keeps its byte");
  p.version = 1;
  e.sanitizeProject(p);
  it::check(p.tracks[kTrackPerc].sampleSlot == 0, "a version-1 file is zeroed");
}

void testStepNote() {
  // TAPE at x1 on a 441 Hz sine: note 72 plays an octave up (the playhead moves twice as fast)
  static sampler::MemoryPool pool;
  static std::vector<int16_t> s(sampler::kSlotMaxSamples, 1000);
  pool.set(0, s.data(), sampler::kSlotMaxSamples, "DC");
  g_inst.engine().suspend();
  g_inst.setSamplePool(&pool);
  g_inst.engine().resume();
  setInst(builtinNamed(kTrackPerc, "TAPE"));
  post(Command{CmdType::SetSampleSlot, uint8_t(kTrackPerc), 0, 0, kDefaultStep, nullptr});
  int32_t moved[2] = {0, 0};
  const int notes[2] = {60, 72};
  for (int k = 0; k < 2; ++k) {
    for (int i = 0; i < 16; ++i) setStep(i, i == 0 ? note(notes[k], 60) : kDefaultStep);
    quanta(4);
    play();
    quanta(20);
    const int32_t a = g_inst.sampler().playhead();
    quanta(1000);
    moved[k] = g_inst.sampler().playhead() - a;
    stop();
    quanta(20);
  }
  // 1000 quanta = 16000 frames at 44.1 kHz = 8000 source samples at x1
  it::check(std::abs(moved[0] - 8000) < 20 && std::abs(moved[1] - 16000) < 40, "note 60: x1 (%d), note 72: x2 (%d)",
            moved[0], moved[1]);
  g_inst.engine().suspend();
  g_inst.setSamplePool(nullptr);
  g_inst.engine().resume();
}

}  // namespace

int main() {
  g_inst.init(44100.0f, true);
  testSwitch();
  testSlicesPerNote();
  testSlotAndPool();
  testSanitize();
  testStepNote();
  return it::finish("itest_sampler");
}
