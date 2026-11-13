// resample_demo: one bar of a factory demo's lead, soloed, resampled into slot 1 the
// way SHIFT+L does it (pg/sampler/resampler.h through the real engine), then CHOP
// stepping through the sixteen slices of it, one per step (docs/UX.md 10.3
// "Resampling"). For listening; not a test (itest_resample checks the same path).
//
//   resample_demo [out_dir]       (default build/resample)
// writes
//   <out>/lead_then_chop.wav   44.1 kHz stereo: two bars of the soloed lead as played,
//                              then two bars of CHOP playing slice n on step n
//   <out>/lead_bar_22k.wav     the slot's content: the resampled bar, 22,050 Hz mono
//                              (as recorded, before the playback gain)
#include <sys/stat.h>

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

const macro::MacroSet* builtinNamed(int track, const char* name) {
  for (int i = 0; i < macro::instrumentCount(track); ++i)
    if (std::strcmp(macro::instrumentAt(track, i)->name, name) == 0) return macro::instrumentAt(track, i);
  return nullptr;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string out = argc > 1 ? argv[1] : "build/resample";
  mkdir("build", 0755);
  mkdir(out.c_str(), 0755);
  static sampler::MemoryPool pool;
  g_inst.init(44100.0f, false);
  g_inst.setSamplePool(&pool);
  // the first demo whose pattern 1 has a lead line (six notes or more in its first 16 steps)
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
  const int bpm = g_inst.project().globals[kGTempo];
  const double bar = 44100.0 * 60.0 / bpm * 4.0;
  std::printf("resample_demo: demo %d \"%s\", %d BPM, the lead soloed\n", demo, demo::name(demo), bpm);
  // solo the lead: what FN + its note key does
  for (int t = 0; t < kNumTracks; ++t)
    post(Command{CmdType::SetMute, uint8_t(t), 0, uint8_t(t == kTrackLead ? 0 : 1), kDefaultStep, nullptr});

  std::vector<int16_t> wav;
  int16_t buf[2 * 128];
  sampler::Recorder rec;
  hostrs::VecSink sink;
  std::vector<int16_t> ring(1u << 17);
  sampler::Resampler rs;
  auto block = [&](bool pump) {
    g_inst.process(buf, 128);
    wav.insert(wav.end(), buf, buf + 256);
    if (pump) rs.pump();
  };
  post(Command{CmdType::Play, 0, 0, 0, kDefaultStep, nullptr});
  for (int i = 0; i < 8; ++i) block(false);
  // SHIFT+L just after the first bar line: it counts in to bar 2, records it, let go at once
  rs.start(&rec, &sink, ring.data(), uint32_t(ring.size()), true);
  g_inst.sampler().setSilentSlot(0);
  g_inst.engine().setOutputTap(&rs);
  while (!rs.started()) block(true);
  rs.release();
  while (!rs.ended()) block(true);
  g_inst.engine().setOutputTap(nullptr);
  g_inst.sampler().setSilentSlot(-1);
  const uint32_t len = rs.length();
  std::printf("resample_demo: %s, %u samples (%.3f s; a bar is %.3f s), started at frame %u of the take\n",
              sampler::recStateName(rs.state()), unsigned(len), double(len) / 22050.0, bar / 44100.0,
              unsigned(rs.startFrame()));
  static std::vector<int16_t> bufs[sampler::kPoolSlots];
  std::string err;
  if (rs.state() != sampler::RecState::Done || !hostrs::commitToPool(&pool, bufs, 0, sink.v.data(), len, &err)) {
    std::fprintf(stderr, "resample_demo: no take (%s)\n", err.c_str());
    return 1;
  }
  wav::write((out + "/lead_bar_22k.wav").c_str(), std::vector<int16_t>(sink.v.begin(), sink.v.begin() + len), 22050,
             1);
  // keep the first two bars of the lead as heard (from Play)
  wav.resize(size_t(2.0 * bar) * 2);
  // CHOP on PERC, slot 1, the only track heard: step n plays slice n
  post(Command{CmdType::Stop, 0, 0, 0, kDefaultStep, nullptr});
  for (int i = 0; i < 300; ++i) g_inst.process(buf, 128);   // the lead's tail, not kept
  const macro::MacroSet* chop = builtinNamed(kTrackPerc, "CHOP");
  uint8_t mp[4] = {0, 128, 128, 255};   // SLICE (per note), PITCH as recorded, TONE open, LENGTH: the whole slice
  post(setInstrumentCommand(kTrackPerc, chop ? chop->id : 0, mp));
  post(Command{CmdType::SetSampleSlot, uint8_t(kTrackPerc), 0, 0, kDefaultStep, nullptr});
  for (int t = 0; t < kNumTracks; ++t)
    post(Command{CmdType::SetMute, uint8_t(t), 0, uint8_t(t == kTrackPerc ? 0 : 1), kDefaultStep, nullptr});
  for (int i = 0; i < 16; ++i) {
    Step st = kDefaultStep;
    st.setFlags(kStepOn);
    st.note = 60;
    st.vel = 127;
    st.gate = 4;
    st.setMacroValue(0, i * 16 + 8);
    post(Command{CmdType::SetStep, 0, uint8_t(kTrackPerc), uint8_t(i), st, nullptr});
    post(Command{CmdType::SetTrackLength, 0, uint8_t(kTrackPerc), 16, kDefaultStep, nullptr});
  }
  post(Command{CmdType::SetPatternLength, 0, 0, 16, kDefaultStep, nullptr});
  g_inst.process(buf, 128);
  post(Command{CmdType::Play, 0, 0, 0, kDefaultStep, nullptr});
  const long chopFrames = long(2.0 * bar);
  for (long f = 0; f < chopFrames; f += 128) block(false);
  const std::string path = out + "/lead_then_chop.wav";
  if (!wav::write(path.c_str(), wav, 44100, 2)) {
    std::fprintf(stderr, "resample_demo: cannot write %s\n", path.c_str());
    return 1;
  }
  std::printf("resample_demo: wrote %s (%.2f s) and %s/lead_bar_22k.wav\n", path.c_str(),
              double(wav.size() / 2) / 44100.0, out.c_str());
  return 0;
}
