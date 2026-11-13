// Resampling (docs/UX.md 10.3 "Resampling"; pg/sampler/resampler.h) through the real
// Instrument: the engine's output into a slot, sample-exact.
//
//   bars      in time, at three tempos (two with swing): the recording starts on the
//             next bar line after the start (where the engine fires the bar's first
//             step: the predicted bar time to within half a quantum) and ends on the
//             first bar line after the release; its length is (end - start) / 2 to the
//             sample, and every sample equals the half-band's value of the engine's own
//             output (mono sum) at frame start + 2k: bit for bit; a second, fresh render
//             of the same pattern gives the same bar (the engine is deterministic)
//   one bar   started mid-bar and let go at once: exactly one bar
//   fits      held: the slot fills; it ends on the last bar line that fits (whole bars,
//             Full); a slot smaller than a bar: filled from the bar line (kNoBarFits)
//   stopped   transport stopped: starts on the first sample at or above -66 dBFS (a
//             note played by hand), ends where it is let go (to the frame, even), nothing
//             trimmed; let go before any sound: Cancelled; all zeros: Cancelled
//   stop      the transport stops mid-recording: it ends on the last bar line passed
//   own slot  CHOP plays the slot being recorded: it is silent meanwhile (the recording
//             equals a render with PERC muted), other slots and tracks are heard; after
//             the end the slot plays again
//   slices    one resampled bar played by CHOP (sixteen slices, SLICE per note): slice n
//             starts where step n's audio starts in the recording (within the engine's
//             half-quantum grid), and the chop at step n plays from there
//   mono      the device's mono output (L == R): the same path
//   overrun   the writer falls behind: Failed, never a take with a hole in it
//   bit-identical: the output with a tap installed equals the output without one
//   cost      the tap's work per quantum on the host (printed)
#include <chrono>
#include <cmath>
#include <cstring>
#include <vector>

#include "common/host_resample.h"
#include "common/ui_harness.h"
#include "itests/itest_util.h"
#include "pg/macro/macro.h"
#include "pg/sampler/resampler.h"
#include "pg/sampler/sample_pool.h"

using namespace pg;
namespace pm = pg::macro;
using sampler::RecState;
using sampler::Resampler;

namespace {

Instrument& inst() {
  static Instrument i;
  return i;
}
sampler::MemoryPool g_pool;

void post(const Command& c) { inst().engine().post(c); }
void play() { post(Command{CmdType::Play, 0, 0, 0, kDefaultStep, nullptr}); }
void stop() { post(Command{CmdType::Stop, 0, 0, 0, kDefaultStep, nullptr}); }
void mute(int t, bool m) { post(Command{CmdType::SetMute, uint8_t(t), 0, uint8_t(m ? 1 : 0), kDefaultStep, nullptr}); }

const pm::MacroSet* builtinNamed(int track, const char* name) {
  for (int i = 0; i < pm::instrumentCount(track); ++i)
    if (std::strcmp(pm::instrumentAt(track, i)->name, name) == 0) return pm::instrumentAt(track, i);
  return nullptr;
}

// A fresh instrument with the mockups' project (kick, snare, clap, hats, perc, bass,
// lead; 124 BPM, swing 8), at this tempo and swing.
void fresh(int bpm, int swing, bool mono = false) {
  Instrument& in = inst();
  in.init(44100.0f, mono);
  in.setSamplePool(&g_pool);
  in.engine().suspend();
  uih::buildMockup(in.project(), in.engine());
  in.project().globals[kGTempo] = uint8_t(bpm);
  in.project().globals[kGSwing] = uint8_t(swing);
  in.engine().sanitizeProject(in.project());
  in.engine().resume();
}

// The stream: everything the engine played (mono sum, as the tap sums it), from the
// first process() after fresh().
struct Stream {
  std::vector<int16_t> mono;
  long frames() const { return long(mono.size()); }
};

// Runs `frames` (a multiple of 128) through the engine in 128-frame blocks (the
// device's block), the writer side pumped after each block.
void render(Stream& s, long frames, Resampler* rs = nullptr) {
  int16_t buf[2 * 128];
  for (long f = 0; f < frames; f += 128) {
    inst().process(buf, 128);
    for (int i = 0; i < 128; ++i) s.mono.push_back(int16_t((int32_t(buf[2 * i]) + int32_t(buf[2 * i + 1])) >> 1));
    if (rs) rs->pump();
  }
}

// The half-band's output centred on frame c of x, computed as HalfBand::run does
// (the same float operations in the same order), rounded as Recorder::out does.
int16_t decim(const std::vector<int16_t>& x, long c) {
  auto at = [&](long i) { return i >= 0 && i < long(x.size()) ? float(x[size_t(i)]) : 0.0f; };
  float y = 0.5f * at(c);
  for (int k = 0; k < sampler::HalfBand::kUnique; ++k) {
    const int d = 2 * k + 1;
    y += sampler::HalfBand::kCoef[k] * (at(c - d) + at(c + d));
  }
  const float yr = y + (y < 0.0f ? -0.5f : 0.5f);
  return int16_t(yr > 32767.0f ? 32767 : (yr < -32768.0f ? -32768 : int(yr)));
}

// How many samples of rec differ from the decimated stream from frame `start` on.
long mismatches(const std::vector<int16_t>& rec, uint32_t len, const std::vector<int16_t>& x, long start) {
  long bad = 0;
  for (uint32_t j = 0; j < len && j < rec.size(); ++j)
    if (rec[j] != decim(x, start + 2 * long(j))) ++bad;
  return bad;
}

double barFrames(int bpm) { return 44100.0 * 60.0 / double(bpm) * 4.0; }

struct Take {
  sampler::Recorder rec;
  hostrs::VecSink sink;
  std::vector<int16_t> ring = std::vector<int16_t>(1u << 17);
  Resampler rs;
  long t0 = 0;   // the stream frame where the tap went in
  void begin(Stream& s, bool sync, uint32_t maxSamples = sampler::kSlotMaxSamples) {
    sink.v.clear();
    rs.start(&rec, &sink, ring.data(), uint32_t(ring.size()), sync, maxSamples);
    inst().engine().setOutputTap(&rs);
    t0 = s.frames();
  }
  void end() { inst().engine().setOutputTap(nullptr); }
  long startAt() const { return t0 + long(rs.startFrame()); }
};

void testBars(int bpm, int swing) {
  fresh(bpm, swing);
  Stream s;
  render(s, 1024);
  const long playAt = s.frames();
  play();
  render(s, 128 * 40);   // into the first bar
  Take tk;
  tk.begin(s, true);
  const double bar = barFrames(bpm);
  render(s, long(bar * 0.8) / 128 * 128, &tk.rs);   // before bar 2's line: counting in
  it::check(!tk.rs.started() && tk.rs.state() == RecState::Waiting && tk.rs.sync(), "%d BPM: counting in", bpm);
  // the next bar line: recording
  render(s, long(bar * 0.4) / 128 * 128, &tk.rs);
  it::check(tk.rs.started() && tk.rs.state() == RecState::Recording, "%d BPM: recording after the bar line", bpm);
  const long start = tk.startAt();
  const double ideal1 = double(playAt) + bar;
  it::check(std::fabs(double(start) - ideal1) <= 8.0 && (start - playAt) % kQuantum == 0,
            "%d BPM: starts on bar 2's line (frame %ld, ideal %.1f)", bpm, start, ideal1);
  // let go in the middle of bar 3: it ends on bar 4's line
  render(s, long(bar * 1.3) / 128 * 128, &tk.rs);
  const long relAt = s.frames();
  tk.rs.release();
  render(s, long(bar * 1.2) / 128 * 128 + 1024, &tk.rs);
  for (int i = 0; i < 50 && !tk.rs.ended(); ++i) render(s, 128, &tk.rs);
  tk.end();
  it::check(tk.rs.ended() && tk.rs.state() == RecState::Done && tk.rs.endReason() == Resampler::kReleased,
            "%d BPM: Done on release (%s)", bpm, sampler::recStateName(tk.rs.state()));
  const long end = start + long(tk.rs.endFrame());
  const double idealEnd = double(playAt) + 3.0 * bar;   // bar 4's line: three bar lines after Play's
  it::check(end > relAt && std::fabs(double(end) - idealEnd) <= 8.0 && (end - playAt) % kQuantum == 0,
            "%d BPM: ends on the first bar line after the release (frame %ld, ideal %.1f, released %ld)", bpm, end,
            idealEnd, relAt);
  const uint32_t len = tk.rs.length();
  it::check(len == uint32_t(end - start) / 2 && (end - start) % 2 == 0, "%d BPM: %u samples = (end - start) / 2", bpm,
            unsigned(len));
  it::check(std::abs(long(len) - long(std::lround(2.0 * bar / 2.0))) <= 8, "%d BPM: two bars (%u samples)", bpm,
            unsigned(len));
  const long bad = mismatches(tk.sink.v, len, s.mono, start);
  it::check(bad == 0, "%d BPM swing %d: %ld of %u samples differ from the decimated output", bpm, swing, bad,
            unsigned(len));
  it::check(tk.rs.bars() == 2 && tk.rs.barAt()[1] == len, "%d BPM: two bar lines seen, the last at the end", bpm);
  it::check(tk.rec.peak() > 3000, "%d BPM: it heard the groove (peak %d)", bpm, int(tk.rec.peak()));
  // a fresh render of the same pattern, no tap: the same bar
  {
    fresh(bpm, swing);
    Stream s2;
    render(s2, 1024);
    play();
    render(s2, s.frames() - 1024);
    long same = 0;
    for (long f = 0; f < s.frames(); ++f) same += s.mono[size_t(f)] == s2.mono[size_t(f)] ? 1 : 0;
    it::check(same == s.frames(), "%d BPM: the tap changes nothing (%ld of %ld frames equal)", bpm, same, s.frames());
    it::check(mismatches(tk.sink.v, len, s2.mono, start) == 0, "%d BPM: equals a fresh render's bar", bpm);
  }
}

void testOneBar() {
  const int bpm = 124;
  fresh(bpm, 8);
  Stream s;
  play();
  render(s, 128 * 100);
  Take tk;
  tk.begin(s, true);
  render(s, 128, &tk.rs);
  tk.rs.release();   // let go at once: still counting in
  render(s, long(barFrames(bpm) * 2.2) / 128 * 128, &tk.rs);
  tk.end();
  // let go before the bar line: nothing (the start had not come)
  it::check(tk.rs.ended() && tk.rs.state() == RecState::Cancelled && tk.rs.endReason() == Resampler::kCancelled,
            "one bar: let go while counting in = nothing (%s)", sampler::recStateName(tk.rs.state()));
  // hold until just after the bar line, then let go: one bar
  Take t2;
  t2.begin(s, true);
  while (!t2.rs.started()) render(s, 128, &t2.rs);
  render(s, 1024, &t2.rs);
  t2.rs.release();
  for (int i = 0; i < 2000 && !t2.rs.ended(); ++i) render(s, 128, &t2.rs);
  t2.end();
  const double bar = barFrames(bpm);
  it::check(t2.rs.state() == RecState::Done && std::fabs(double(t2.rs.endFrame()) - bar) <= 16.0,
            "one bar: %u frames (a bar is %.1f)", unsigned(t2.rs.endFrame()), bar);
  it::check(mismatches(t2.sink.v, t2.rs.length(), s.mono, t2.startAt()) == 0, "one bar: sample-exact");
}

void testFits() {
  // held: the slot (6.2 s) fills at 124 BPM (a bar 1.94 s): three whole bars
  const int bpm = 124;
  fresh(bpm, 8);
  Stream s;
  play();
  render(s, 128 * 50);
  Take tk;
  tk.begin(s, true);
  for (int i = 0; i < 6000 && !tk.rs.ended(); ++i) render(s, 128, &tk.rs);
  tk.end();
  const double bar = barFrames(bpm);
  it::check(tk.rs.state() == RecState::Full && tk.rs.endReason() == Resampler::kFull, "fits: Full (%s)",
            sampler::recStateName(tk.rs.state()));
  it::check(std::fabs(double(tk.rs.endFrame()) - 3.0 * bar) <= 16.0, "fits: three whole bars (%u frames, bar %.1f)",
            unsigned(tk.rs.endFrame()), bar);
  it::check(tk.rs.length() * 2 == tk.rs.endFrame() && tk.rs.length() <= sampler::kSlotMaxSamples,
            "fits: %u samples", unsigned(tk.rs.length()));
  it::check(mismatches(tk.sink.v, tk.rs.length(), s.mono, tk.startAt()) == 0, "fits: sample-exact");
  // at 60 BPM a bar is 4 s: one fits
  fresh(60, 0);
  Stream s60;
  play();
  render(s60, 128 * 50);
  Take t60;
  t60.begin(s60, true);
  for (int i = 0; i < 8000 && !t60.rs.ended(); ++i) render(s60, 128, &t60.rs);
  t60.end();
  it::check(t60.rs.state() == RecState::Full && std::fabs(double(t60.rs.endFrame()) - barFrames(60)) <= 16.0,
            "fits: 60 BPM, one bar of 4 s (%u frames)", unsigned(t60.rs.endFrame()));
  // a slot smaller than a bar (a test-only maximum): filled from the bar line
  fresh(bpm, 8);
  Stream s2;
  play();
  render(s2, 128 * 50);
  Take t2;
  const uint32_t small = uint32_t(bar * 0.75 / 2.0);
  t2.begin(s2, true, small);
  for (int i = 0; i < 6000 && !t2.rs.ended(); ++i) render(s2, 128, &t2.rs);
  t2.end();
  it::check(t2.rs.state() == RecState::Full && t2.rs.endReason() == Resampler::kNoBarFits &&
                t2.rs.length() == small,
            "fits: no whole bar fits: the slot is full from the bar line (%s, %u of %u)",
            sampler::recStateName(t2.rs.state()), unsigned(t2.rs.length()), unsigned(small));
  {
    const double r = double(t2.startAt()) / bar;
    it::check(std::fabs(double(t2.startAt()) - std::round(r) * bar) <= 8.0 && t2.startAt() % kQuantum == 0,
              "fits: it still started on a bar line (frame %ld)", t2.startAt());
  }
  it::check(mismatches(t2.sink.v, t2.rs.length(), s2.mono, t2.startAt()) == 0, "fits: sample-exact");
}

void leadNote(int note, bool on) {
  Step st = kDefaultStep;
  st.setFlags(kStepOn);
  st.vel = 110;
  post(Command{on ? CmdType::NoteOn : CmdType::NoteOff, uint8_t(kTrackLead), uint8_t(note), 110, st, nullptr});
}

void testStopped() {
  fresh(124, 8);
  Stream s;
  render(s, 128 * 20);
  Take tk;
  tk.begin(s, false);
  render(s, 128 * 40, &tk.rs);
  it::check(!tk.rs.started() && tk.rs.state() == RecState::Waiting && !tk.rs.sync(), "stopped: waiting for a sound");
  leadNote(57, true);
  render(s, 128 * 120, &tk.rs);
  leadNote(57, false);
  render(s, 128 * 60 + 37 * 0, &tk.rs);
  const long relAt = s.frames();
  tk.rs.release();
  for (int i = 0; i < 100 && !tk.rs.ended(); ++i) render(s, 128, &tk.rs);
  tk.end();
  long first = -1;
  for (long f = tk.t0; f < s.frames() && first < 0; ++f)
    if (std::abs(int(s.mono[size_t(f)])) >= Resampler::kSilence) first = f;
  it::check(tk.rs.state() == RecState::Done && tk.startAt() == first,
            "stopped: starts on the first sound (frame %ld, first at or above -66 dBFS %ld)", tk.startAt(), first);
  const long end = tk.startAt() + long(tk.rs.endFrame());
  it::check(end >= relAt && end <= relAt + 1, "stopped: ends at the release (%ld, released %ld)", end, relAt);
  it::check(tk.rs.length() == tk.rs.endFrame() / 2, "stopped: nothing trimmed (%u samples)", unsigned(tk.rs.length()));
  it::check(mismatches(tk.sink.v, tk.rs.length(), s.mono, tk.startAt()) == 0, "stopped: sample-exact");
  // let go before any sound: nothing; all zeros never starts
  Take t2;
  render(s, 44100 * 3);   // the tail dies
  t2.begin(s, false);
  render(s, 128 * 30, &t2.rs);
  t2.rs.release();
  render(s, 256, &t2.rs);
  t2.end();
  it::check(t2.rs.ended() && t2.rs.state() == RecState::Cancelled, "stopped: let go before a sound = nothing (%s)",
            sampler::recStateName(t2.rs.state()));
}

void testStop() {
  const int bpm = 124;
  fresh(bpm, 8);
  Stream s;
  play();
  render(s, 128 * 50);
  Take tk;
  tk.begin(s, true);
  while (!tk.rs.started()) render(s, 128, &tk.rs);
  render(s, long(barFrames(bpm) * 1.5) / 128 * 128, &tk.rs);
  stop();
  for (int i = 0; i < 400 && !tk.rs.ended(); ++i) render(s, 128, &tk.rs);
  tk.end();
  it::check(tk.rs.state() == RecState::Done && tk.rs.endReason() == Resampler::kStopped &&
                std::fabs(double(tk.rs.endFrame()) - barFrames(bpm)) <= 16.0,
            "stop: ends on the last bar line passed (%s, %u frames)", sampler::recStateName(tk.rs.state()),
            unsigned(tk.rs.endFrame()));
}

// CHOP on PERC, slot 1, notes on steps 1, 3, 6, 10, 13 (none on step 0): the slot is
// recorded meanwhile.
void chopPattern(int slot) {
  const pm::MacroSet* chop = builtinNamed(kTrackPerc, "CHOP");
  it::check(chop != nullptr, "CHOP exists");
  uint8_t p[4] = {0, 128, 128, 160};
  post(setInstrumentCommand(kTrackPerc, chop ? chop->id : 0, p));
  post(Command{CmdType::SetSampleSlot, uint8_t(kTrackPerc), 0, uint8_t(slot), kDefaultStep, nullptr});
  for (int i = 0; i < 16; ++i) {
    Step st = kDefaultStep;
    if (i == 1 || i == 3 || i == 6 || i == 10 || i == 13) {
      st.setFlags(kStepOn);
      st.note = 60;
      st.vel = 120;
      st.gate = 3;
    }
    post(Command{CmdType::SetStep, 0, uint8_t(kTrackPerc), uint8_t(i), st, nullptr});
  }
}

void testOwnSlot() {
  for (int i = 0; i < sampler::kPoolSlots; ++i) g_pool.clear(i);
  const int bpm = 124;
  // the reference: PERC muted from the start
  fresh(bpm, 8);
  chopPattern(0);
  mute(kTrackPerc, true);
  Stream ref;
  render(ref, 1024);
  play();
  render(ref, 128 * 30);
  Take tr;
  tr.begin(ref, true);
  for (int i = 0; i < 3000 && !tr.rs.started(); ++i) render(ref, 128, &tr.rs);
  render(ref, 128 * 200, &tr.rs);
  tr.rs.release();
  for (int i = 0; i < 3000 && !tr.rs.ended(); ++i) render(ref, 128, &tr.rs);
  tr.end();
  // the same with PERC playing slot 1, which is being recorded
  fresh(bpm, 8);
  chopPattern(0);
  Stream s;
  render(s, 1024);
  play();
  render(s, 128 * 30);
  Take tk;
  inst().sampler().setSilentSlot(0);
  tk.begin(s, true);
  bool sounded = false;
  for (int i = 0; i < 3000 && !tk.rs.started(); ++i) {
    render(s, 128, &tk.rs);
    sounded |= inst().sampler().playhead() >= 0;
  }
  for (int i = 0; i < 200; ++i) {
    render(s, 128, &tk.rs);
    sounded |= inst().sampler().playhead() >= 0;
  }
  tk.rs.release();
  for (int i = 0; i < 3000 && !tk.rs.ended(); ++i) {
    render(s, 128, &tk.rs);
    sounded |= inst().sampler().playhead() >= 0;
  }
  tk.end();
  it::check(!sounded, "own slot: the sampler never sounded while its slot recorded");
  it::check(tk.rs.state() == RecState::Done && tr.rs.state() == RecState::Done && tk.rs.length() == tr.rs.length(),
            "own slot: both recorded (%u / %u samples)", unsigned(tk.rs.length()), unsigned(tr.rs.length()));
  long diff = 0;
  for (uint32_t j = 0; j < tk.rs.length() && j < tr.rs.length(); ++j) diff += tk.sink.v[j] != tr.sink.v[j] ? 1 : 0;
  it::check(diff == 0, "own slot: the recording equals a render with PERC muted (%ld samples differ)", diff);
  // the other tracks are in it
  it::check(tk.rec.peak() > 3000, "own slot: the rest of the groove is heard (peak %d)", int(tk.rec.peak()));
  // over: the slot plays again (its built-in sound until something is committed)
  inst().sampler().setSilentSlot(-1);
  bool after = false;
  for (int i = 0; i < 2000 && !after; ++i) {
    render(s, 128);
    after = inst().sampler().playhead() >= 0;
  }
  it::check(after, "own slot: after the end the slot plays again");
  // another slot is not silenced: PERC on slot 2 while slot 1 records
  fresh(bpm, 8);
  chopPattern(1);
  Stream s3;
  play();
  inst().sampler().setSilentSlot(0);
  bool other = false;
  for (int i = 0; i < 2000 && !other; ++i) {
    render(s3, 128);
    other = inst().sampler().playhead() >= 0 && inst().sampler().playingSlot() == 1;
  }
  inst().sampler().setSilentSlot(-1);
  it::check(other, "own slot: another slot plays meanwhile");
}

// One bar of the lead alone (the rest muted: what a solo does), resampled into slot
// 2, then CHOP plays it, step n with SLICE n.
void testSlices() {
  for (int i = 0; i < sampler::kPoolSlots; ++i) g_pool.clear(i);
  const int bpm = 120;
  fresh(bpm, 0);
  for (int t = 0; t < kNumTracks; ++t) mute(t, t != kTrackLead);
  // a lead note on every step, each a different pitch (short): step n's audio is n's
  for (int i = 0; i < 16; ++i) {
    Step st = kDefaultStep;
    st.setFlags(kStepOn);
    st.note = uint8_t(48 + i);
    st.vel = 110;
    st.gate = 2;
    post(Command{CmdType::SetStep, 0, uint8_t(kTrackLead), uint8_t(i), st, nullptr});
  }
  Stream s;
  render(s, 1024);
  play();
  render(s, 128 * 30);
  Take tk;
  tk.begin(s, true);
  while (!tk.rs.started()) render(s, 128, &tk.rs);
  tk.rs.release();
  for (int i = 0; i < 3000 && !tk.rs.ended(); ++i) render(s, 128, &tk.rs);
  tk.end();
  const uint32_t len = tk.rs.length();
  it::check(tk.rs.state() == RecState::Done && std::fabs(double(len) - barFrames(bpm) / 2.0) <= 8.0,
            "slices: one bar (%u samples)", unsigned(len));
  static std::vector<int16_t> bufs[sampler::kPoolSlots];
  std::string err;
  it::check(hostrs::commitToPool(&g_pool, bufs, 1, tk.sink.v.data(), len, &err), "slices: committed");
  // where step n's notes start in the recording: the quantum edge the engine fires it at
  const double step = barFrames(bpm) / 16.0;
  const long start = tk.startAt();
  for (int n = 0; n < 16; ++n) {
    // the onset: the first frame from the step's quantum on where the output moves
    const long q = start + long(std::lround(double(n) * step / kQuantum)) * kQuantum;
    uint8_t p[sampler::kNumParams];
    for (int i = 0; i < sampler::kNumParams; ++i) p[i] = sampler::paramInfo(i)->def;
    p[sampler::kSlices] = 16;
    p[sampler::kSlice] = uint8_t(n);
    p[sampler::kStart] = 0;
    p[sampler::kLength] = 127;
    int32_t a = 0, b = 0;
    sampler::SamplerVoice::region(p, len, sampler::kPoolRate, &a, &b);
    const long onset = (q - start) / 2;
    it::check(std::abs(long(a) - onset) <= kQuantum / 4 + 1, "slices: slice %d starts at %d, step %d's audio at %ld", n,
              int(a), n, onset);
  }
  // play it through CHOP: step n's note with SLICE n; the playhead starts in slice n
  fresh(bpm, 0);
  const pm::MacroSet* chop = builtinNamed(kTrackPerc, "CHOP");
  uint8_t mp[4] = {0, 128, 128, 255};
  post(setInstrumentCommand(kTrackPerc, chop ? chop->id : 0, mp));
  post(Command{CmdType::SetSampleSlot, uint8_t(kTrackPerc), 0, 1, kDefaultStep, nullptr});
  for (int t = 0; t < kNumTracks; ++t) mute(t, t != kTrackPerc);
  for (int i = 0; i < 16; ++i) {
    Step st = kDefaultStep;
    st.setFlags(kStepOn);
    st.note = 60;
    st.vel = 127;
    st.gate = 4;
    st.setMacroValue(0, i * 16 + 8);
    post(Command{CmdType::SetStep, 0, uint8_t(kTrackPerc), uint8_t(i), st, nullptr});
  }
  render(s, 1024);
  play();
  int16_t buf[2 * kQuantum];
  int seen = 0, okCount = 0;
  uint32_t trig = inst().engine().status().trig[kTrackPerc];
  for (int qd = 0; qd < int(barFrames(bpm) / kQuantum) + 4; ++qd) {
    inst().process(buf, kQuantum);
    const EngineStatus st = inst().engine().status();
    if (st.trig[kTrackPerc] != trig && seen < 16) {
      trig = st.trig[kTrackPerc];
      const int n = st.step[kTrackPerc];
      const int32_t ph = inst().sampler().playhead();
      const int32_t a = int32_t(int64_t(len) * n / 16);
      ++seen;
      if (ph >= a && ph < a + kQuantum) ++okCount;
      else std::printf("  step %d: playhead %d, slice start %d\n", n, int(ph), int(a));
    }
  }
  it::check(seen == 16 && okCount == 16, "slices: CHOP's step n plays from slice n's start (%d of %d)", okCount, seen);
}

void testMono() {
  fresh(124, 8, true);
  Stream s;
  play();
  render(s, 128 * 50);
  Take tk;
  tk.begin(s, true);
  while (!tk.rs.started()) render(s, 128, &tk.rs);
  render(s, 4096, &tk.rs);
  tk.rs.release();
  for (int i = 0; i < 3000 && !tk.rs.ended(); ++i) render(s, 128, &tk.rs);
  tk.end();
  it::check(tk.rs.state() == RecState::Done && std::fabs(double(tk.rs.endFrame()) - barFrames(124)) <= 16.0,
            "mono: one bar");
  it::check(mismatches(tk.sink.v, tk.rs.length(), s.mono, tk.startAt()) == 0, "mono: sample-exact");
}

// The writer falls behind (a ring of 512 frames, never pumped while it plays): the
// take fails (OVERRUN) instead of keeping a recording with a hole in its timing.
void testOverrun() {
  fresh(124, 8);
  Stream s;
  play();
  render(s, 128 * 20);
  sampler::Recorder rec;
  hostrs::VecSink sink;
  std::vector<int16_t> ring(512);
  Resampler rs;
  rs.start(&rec, &sink, ring.data(), 512, false);
  inst().engine().setOutputTap(&rs);
  render(s, 128 * 40);   // no pump
  for (int i = 0; i < 20 && !rs.ended(); ++i) render(s, 128, &rs);
  inst().engine().setOutputTap(nullptr);
  it::check(rs.ended() && rs.state() == RecState::Failed && rs.endReason() == Resampler::kOverrun && rs.dropped() > 0,
            "overrun: Failed (%s), %u frames dropped", sampler::recStateName(rs.state()), unsigned(rs.dropped()));
}

void testCost() {
  Resampler rs;
  sampler::Recorder rec;
  hostrs::VecSink sink;
  std::vector<int16_t> ring(1u << 20);
  rs.start(&rec, &sink, ring.data(), uint32_t(ring.size()), false);
  int16_t blk[2 * kQuantum];
  for (int i = 0; i < 2 * kQuantum; ++i) blk[i] = int16_t((i * 977) % 20000 - 10000);
  const int n = 60000;
  const auto t0 = std::chrono::steady_clock::now();
  for (int q = 0; q < n; ++q) rs.quantum(blk, kQuantum, OutputTap::kTapPlaying);
  const auto t1 = std::chrono::steady_clock::now();
  const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / n;
  std::printf("itest_resample: the tap while recording: %.1f ns per quantum of 16 frames on this host (%.2f ns/frame)\n",
              ns, ns / kQuantum);
  it::check(rs.started(), "cost: it recorded");
}

}  // namespace

int main() {
  testBars(124, 8);
  testBars(87, 30);
  testBars(150, 0);
  testOneBar();
  testFits();
  testStopped();
  testStop();
  testOwnSlot();
  testSlices();
  testMono();
  testOverrun();
  testCost();
  return it::finish("itest_resample");
}
