// tape_demo: listening renders of the tape (pg/tape/tape.h) through the real engine, as
// the desktop app runs it (44.1 kHz stereo), on factory demo ACIDJAM's main groove (P1,
// 124 BPM, a 4-bar loop on tape). For listening; not a test (itest_tape checks the same).
//
//   tape_demo [out_dir]          (default build/tape)
// writes
//   <out>/layered.wav   16 bars: (A) the groove live while the bass goes onto T1 (TRACK);
//                       (B) the bass muted, T1 plays it, the lead's macros pushed while the
//                       lead goes onto T2; (C) bass and lead muted, T1 + T2 play them, the
//                       drums live, everything bounced onto T3 (MIX); (D) every instrument
//                       muted, T1 and T2 muted: only the bounce T3 plays
//   <out>/bounce.wav    8 bars: 4 bars of the groove live, then its bounce from tape alone
//   <out>/brake.wav     8 bars: drums live, bass + lead from tape; BRAKE held from bar 3's
//   <out>/half.wav      second beat for a bar, then let go (each its trick)
//   <out>/reverse.wav
#include <sys/stat.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "common/wav.h"
#include "pg/demo/demo.h"
#include "pg/instrument/instrument.h"
#include "pg/macro/macro.h"
#include "pg/tape/tape.h"

using namespace pg;

namespace {

Instrument g_inst;
tape::Tape g_tape;
std::vector<uint8_t> g_mem;
constexpr float kSr = 44100.0f;
std::vector<int16_t> g_out;
bool g_keep = false;

void post(const Command& c) { g_inst.engine().post(c); }
void tc(tape::Op op, int b = 0, int c = 0, int note = 0) { post(tape::tapeCommand(op, b, c, note)); }
Command cmd(CmdType t, int a = 0, int b = 0, int c = 0) {
  Command x;
  std::memset(&x, 0, sizeof(x));
  x.type = t;
  x.a = uint8_t(a);
  x.b = uint8_t(b);
  x.c = uint8_t(c);
  return x;
}
void run(long frames) {
  int16_t buf[128];
  for (long d = 0; d < frames; d += 64) {
    g_inst.process(buf, 64);
    g_tape.service();
    if (g_keep) g_out.insert(g_out.end(), buf, buf + 128);
  }
}
long bar() { return long(kSr * 240.0f / 124.0f); }
bool rec() { const tape::Rec r = g_tape.view().rec; return r == tape::Rec::Rec || r == tape::Rec::Ending; }
void untilRec() {
  for (int i = 0; i < 100000 && !rec(); ++i) run(64);
}
void untilIdle() {
  for (int i = 0; i < 200000 && !g_tape.idle(); ++i) run(64);
}
void mute(int t, bool m) { post(cmd(CmdType::SetMute, t, 0, m ? 1 : 0)); }

void fresh() {
  g_inst.engine().setTape(nullptr);
  g_inst.init(kSr, false);
  g_inst.engine().suspend();
  demo::build(g_inst.engine(), g_inst.project(), 0);
  g_inst.project().songMode = 0;
  g_inst.engine().sanitizeProject(g_inst.project());
  g_inst.engine().resume();
  g_mem.assign(tape::Tape::bytesFor(300, uint32_t(kSr)), 0);
  g_tape.init(g_mem.data(), g_mem.size(), uint32_t(kSr));
  g_inst.engine().setTape(&g_tape);
  tc(tape::kOpLoop, 0, 4);
  g_out.clear();
  g_keep = false;
  run(64);
}

// A 4-bar take armed from stop (the count-in bar is not kept) or while playing.
void take(int track, int source, int srcTrack, bool fromStop) {
  g_tape.prepareTake();
  tc(tape::kOpArm, track, source, srcTrack);
  if (fromStop) {
    untilRec();
    g_keep = true;
  } else {
    untilRec();
  }
  run(3 * bar() + bar() / 2);
  tc(tape::kOpEnd);
  for (int i = 0; i < 100000 && rec(); ++i) run(64);
}

bool save(const std::string& path) {
  if (!wav::write(path.c_str(), g_out, int(kSr), 2)) {
    std::fprintf(stderr, "cannot write %s\n", path.c_str());
    return false;
  }
  std::printf("tape_demo: wrote %s (%.1f s)\n", path.c_str(), double(g_out.size() / 2) / double(kSr));
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string out = argc > 1 ? argv[1] : "build/tape";
  mkdir("build", 0755);
  mkdir(out.c_str(), 0755);

  // ---- layered ----
  fresh();
  take(0, tape::kSrcTrack, kTrackBass, true);                // (A) the bass onto T1, from the top
  untilIdle();
  mute(kTrackBass, true);                                     // (B) the bass from tape
  post(setMacroCommand(kTrackLead, 0, 220));
  post(setMacroCommand(kTrackLead, 1, 40));
  take(1, tape::kSrcTrack, kTrackLead, false);                //     the pushed lead onto T2
  untilIdle();
  mute(kTrackLead, true);                                     // (C) bass + lead from tape, drums live
  take(2, tape::kSrcMix, 0, false);                           //     everything onto T3
  untilIdle();
  for (int t = 0; t < kNumTracks; ++t) mute(t, true);         // (D) only the bounce
  tc(tape::kOpParam, 0, 1, tape::kPMute);
  tc(tape::kOpParam, 1, 1, tape::kPMute);
  run(4 * bar());
  if (!save(out + "/layered.wav")) return 1;

  // ---- bounce ----
  fresh();
  take(0, tape::kSrcMix, 0, true);                            // the groove, live, onto T1 from the top
  post(cmd(CmdType::Stop));
  g_keep = false;
  run(long(kSr) * 6);                                         // the tails ring out
  for (int t = 0; t < kNumTracks; ++t) mute(t, true);
  run(4410);
  post(cmd(CmdType::Play));
  g_keep = true;
  run(4 * bar());                                             // the same 4 bars from tape alone
  if (!save(out + "/bounce.wav")) return 1;

  // ---- the tricks: drums live, bass + lead from tape ----
  const struct { uint8_t trick; const char* name; } kT[] = {
      {tape::kTrickBrake, "brake"}, {tape::kTrickHalf, "half"}, {tape::kTrickReverse, "reverse"}};
  for (const auto& tr : kT) {
    fresh();
    for (int t = 0; t < kNumDrumTracks; ++t) mute(t, true);
    take(0, tape::kSrcMix, 0, true);                          // bass + lead onto T1
    post(cmd(CmdType::Stop));
    run(long(kSr) * 6);
    g_out.clear();
    for (int t = 0; t < kNumTracks; ++t) mute(t, t >= kNumDrumTracks);
    run(4410);
    post(cmd(CmdType::Play));
    g_keep = true;
    run(2 * bar() + bar() / 4);
    tc(tape::kOpTrick, 0, tr.trick);
    run(bar());
    tc(tape::kOpTrick, 0, tape::kTrickNone);
    run(5 * bar() - bar() / 4);
    if (!save(out + "/" + tr.name + ".wav")) return 1;
  }
  g_inst.engine().setTape(nullptr);
  return 0;
}
