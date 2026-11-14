// melodic_demo: the lead's sampler instruments (pg/sampler/melodic.h) playing a
// short phrase with a chord and a slide, for listening; not a test
// (itest_lead_sampler and test_sampler_melodic check the same paths).
//
//   melodic_demo [out_dir]       (default build/melodic)
// writes, 44.1 kHz stereo, the lead alone:
//   <out>/pluck.wav, cloud.wav, reel.wav   each set on slot 1's built-in sound: a
//        line with a slide, a MIN7 chord held two beats, a triad, a slide down;
//        twice (the second pass moves the set's first macro, as a tilt would)
//   <out>/resample_then_play.wav   a factory demo's lead soloed for two bars, the
//        second bar resampled into slot 2 as SHIFT+L does it, then PLUCK and CLOUD
//        playing that take melodically (the same phrase)
//   <out>/resampled_bar_22k.wav    the take: 22,050 Hz mono
#include <sys/stat.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "common/host_resample.h"
#include "common/wav.h"
#include "pg/demo/demo.h"
#include "pg/instrument/instrument.h"
#include "pg/macro/macro.h"

using namespace pg;

namespace {

Instrument g_inst;
void post(const Command& c) { g_inst.engine().post(c); }
std::vector<int16_t> g_wav;
int16_t g_buf[2 * 128];
void block() {
  g_inst.process(g_buf, 128);
  g_wav.insert(g_wav.end(), g_buf, g_buf + 256);
}
void frames(double n) {
  for (long f = 0; f < long(n); f += 128) block();
}

const macro::MacroSet* builtinNamed(int track, const char* name) {
  for (int i = 0; i < macro::instrumentCount(track); ++i)
    if (std::strcmp(macro::instrumentAt(track, i)->name, name) == 0) return macro::instrumentAt(track, i);
  return nullptr;
}

void soloLead() {
  for (int t = 0; t < kNumTracks; ++t)
    post(Command{CmdType::SetMute, uint8_t(t), 0, uint8_t(t == kTrackLead ? 0 : 1), kDefaultStep, nullptr});
}

// The phrase: 16 steps. C3, E3 sliding into G3, a D3 MIN7 chord for 4 steps, an F3
// MAJ triad, A3 sliding down to G3 (48 = the sample as recorded).
void writePhrase() {
  struct N { int step, note, chord, gate; bool slide; };
  const N ns[] = {{0, 48, kChordNone, 2, false}, {2, 52, kChordNone, 4, true}, {3, 55, kChordNone, 4, false},
                  {6, 50, kChordMin7, 16, false}, {10, 53, kChordMaj, 6, false}, {13, 57, kChordNone, 4, true},
                  {14, 55, kChordNone, 8, false}};
  for (int i = 0; i < 16; ++i) post(Command{CmdType::SetStep, 0, uint8_t(kTrackLead), uint8_t(i), kDefaultStep, nullptr});
  for (const N& n : ns) {
    Step s = kDefaultStep;
    s.setFlags(uint8_t(kStepOn | (n.slide ? kStepSlide : 0)));
    s.note = uint8_t(n.note);
    s.vel = 110;
    s.gate = uint8_t(n.gate);
    s.setChord(uint8_t(n.chord));
    post(Command{CmdType::SetStep, 0, uint8_t(kTrackLead), uint8_t(n.step), s, nullptr});
  }
  post(Command{CmdType::SetTrackLength, 0, uint8_t(kTrackLead), 16, kDefaultStep, nullptr});
  post(Command{CmdType::SetPatternLength, 0, 0, 16, kDefaultStep, nullptr});
}

// Two passes of the phrase on `set` (slot `slot`), the second moving its first macro
// from its start to the far end; then a second of tail. Macro `om` (if >= 0) starts at `ov`.
void playPhrase(const char* set, int slot, int om = -1, int ov = 0) {
  const macro::MacroSet* s = builtinNamed(kTrackLead, set);
  if (!s) {
    std::fprintf(stderr, "melodic_demo: no lead set %s\n", set);
    return;
  }
  uint8_t pos[4];
  for (int m = 0; m < 4; ++m) pos[m] = uint8_t(m < s->numMacros ? int(s->macros[m].start() * 255.0f + 0.5f) : 0);
  if (om >= 0 && om < 4) pos[om] = uint8_t(ov);
  post(setInstrumentCommand(kTrackLead, s->id, pos));
  post(Command{CmdType::SetSampleSlot, uint8_t(kTrackLead), 0, uint8_t(slot), kDefaultStep, nullptr});
  writePhrase();
  g_inst.process(g_buf, 128);
  const double bar = 44100.0 * 60.0 / g_inst.project().globals[kGTempo] * 4.0;
  post(Command{CmdType::Play, 0, 0, 0, kDefaultStep, nullptr});
  frames(bar);
  const int from = pos[0], to = from < 128 ? 230 : 20;
  for (long f = 0; f < long(bar); f += 128) {
    const int v = from + int(double(to - from) * double(f) / bar);
    if ((f / 128) % 12 == 0) post(setMacroCommand(kTrackLead, 0, uint8_t(v)));
    block();
  }
  post(Command{CmdType::Stop, 0, 0, 0, kDefaultStep, nullptr});
  frames(44100.0);
}

bool save(const std::string& path) {
  const bool ok = wav::write(path.c_str(), g_wav, 44100, 2);
  std::printf("melodic_demo: %s %s (%.2f s)\n", ok ? "wrote" : "CANNOT WRITE", path.c_str(), double(g_wav.size() / 2) / 44100.0);
  g_wav.clear();
  return ok;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string out = argc > 1 ? argv[1] : "build/melodic";
  mkdir("build", 0755);
  mkdir(out.c_str(), 0755);
  static sampler::MemoryPool pool;
  g_inst.init(44100.0f, false);
  g_inst.setSamplePool(&pool);
  bool ok = true;

  // 1. each set on slot 1's built-in sound
  for (const char* set : {"PLUCK", "CLOUD", "REEL"}) {
    g_inst.engine().suspend();
    g_inst.engine().initProject(g_inst.project());
    g_inst.project().globals[kGTempo] = 96;
    g_inst.engine().sanitizeProject(g_inst.project());
    g_inst.engine().resume();
    soloLead();
    playPhrase(set, 0);
    std::string name = set;
    for (char& c : name) c = char(c - 'A' + 'a');
    ok &= save(out + "/" + name + ".wav");
  }

  // 2. a demo's lead resampled, then played melodically
  int demo = -1;
  for (int d = 0; d < demo::count() && demo < 0; ++d) {
    static Project p;
    demo::build(g_inst.engine(), p, d);
    int notes = 0;
    for (int i = 0; i < 16; ++i) notes += (p.patterns[0].tracks[kTrackLead].steps[i].flags() & kStepOn) ? 1 : 0;
    if (notes >= 6) demo = d;
  }
  if (demo < 0) demo = 0;
  g_inst.engine().suspend();
  demo::build(g_inst.engine(), g_inst.project(), demo);
  g_inst.project().songMode = 0;
  g_inst.engine().sanitizeProject(g_inst.project());
  g_inst.engine().resume();
  std::printf("melodic_demo: demo %d \"%s\", %d BPM, the lead soloed and resampled into slot 2\n", demo,
              demo::name(demo), g_inst.project().globals[kGTempo]);
  soloLead();
  sampler::Recorder rec;
  hostrs::VecSink sink;
  std::vector<int16_t> ring(1u << 17);
  sampler::Resampler rs;
  post(Command{CmdType::Play, 0, 0, 0, kDefaultStep, nullptr});
  for (int i = 0; i < 8; ++i) block();
  rs.start(&rec, &sink, ring.data(), uint32_t(ring.size()), true);
  g_inst.sampler().setSilentSlot(1);
  g_inst.engine().setOutputTap(&rs);
  while (!rs.started()) {
    block();
    rs.pump();
  }
  rs.release();
  while (!rs.ended()) {
    block();
    rs.pump();
  }
  g_inst.engine().setOutputTap(nullptr);
  g_inst.sampler().setSilentSlot(-1);
  const uint32_t len = rs.length();
  static std::vector<int16_t> bufs[sampler::kPoolSlots];
  std::string err;
  if (rs.state() != sampler::RecState::Done || !hostrs::commitToPool(&pool, bufs, 1, sink.v.data(), len, &err)) {
    std::fprintf(stderr, "melodic_demo: no take (%s)\n", err.c_str());
    return 1;
  }
  wav::write((out + "/resampled_bar_22k.wav").c_str(), std::vector<int16_t>(sink.v.begin(), sink.v.begin() + len),
             22050, 1);
  post(Command{CmdType::Stop, 0, 0, 0, kDefaultStep, nullptr});
  frames(0.5 * 44100.0);
  // the take, played: the lead's own track now on PLUCK, then CLOUD, at the demo's tempo
  g_inst.engine().suspend();
  for (int pt = 0; pt < kNumPatterns; ++pt)
    for (int i = 0; i < kMaxSteps; ++i) g_inst.project().patterns[pt].tracks[kTrackLead].steps[i] = kDefaultStep;
  g_inst.engine().sanitizeProject(g_inst.project());
  g_inst.engine().resume();
  post(Command{CmdType::QueuePattern, 0, 0, 0, kDefaultStep, nullptr});
  // where the take is loudest (a 0.15 s window, the first half for PLUCK's START):
  // PLUCK's notes start there, CLOUD's loop sits there (SCAN: START 6..104)
  auto loudest = [&](uint32_t from, uint32_t to) {
    const uint32_t w = 3300;
    double best = -1.0;
    uint32_t at = from;
    for (uint32_t a = from; a + w <= to; a += 200) {
      double e = 0.0;
      for (uint32_t i = a; i < a + w; ++i) e += double(sink.v[i]) * double(sink.v[i]);
      if (e > best) { best = e; at = a; }
    }
    return at;
  };
  const float stP = 128.0f * float(loudest(0, len / 2)) / float(len);
  const float stC = 128.0f * float(loudest(0, len)) / float(len);
  std::printf("melodic_demo: the take's loudest stretch: START %.0f (first half), %.0f (anywhere)\n", double(stP),
              double(stC));
  playPhrase("PLUCK", 1, 2, std::min(255, int(stP / 64.0f * 255.0f + 0.5f)));
  playPhrase("CLOUD", 1, 0, std::max(0, std::min(255, int((stC - 6.0f) / 98.0f * 255.0f + 0.5f))));
  ok &= save(out + "/resample_then_play.wav");
  return ok ? 0 : 1;
}
