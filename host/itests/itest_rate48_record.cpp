// Recording at 48 kHz (docs/ARCHITECTURE.md "Sample rates", recorder.h RecRates):
//
//   rates     what a platform stores: 44,100 in -> 22,050 (the Cardputer's pool, the
//             half-band); 48,000 in -> 48,000 (the Daisy's: no filter); 48,000 into a
//             24,000 Hz pool -> the half-band; invalid pairs fail at once
//   mic       a synthetic room (quiet noise) and a 440 Hz tone at 48 kHz into a 48 kHz
//             slot: the floor, the threshold, 40 ms of pre-roll and the 60 ms tail in
//             ms as at 22,050 Hz; the slot's header says 48,000; the stored tone is
//             440 Hz; played back by both samplers at a 48 kHz and a 44.1 kHz engine it
//             is 440 Hz (and lasts as long as it was recorded); 48 -> 24 kHz too
//   commit    the same 48 kHz recording through the core's flash path, as the Daisy
//             commits it (SampleWriter::beginRecording / write / commitRecording at the
//             Recorder's stored rate into a FlashPool on a RAM flash): the header says
//             48,000 and both samplers play it at 440 Hz and for as long as it was
//             recorded; the 44.1 -> 22.05 kHz recording (the Cardputer's) the same
//   resample  the engine at 48 kHz, in time: the recording starts on a bar line and
//             ends on the bar line after the release; its length is the frames between
//             them, and every sample IS the engine's own mono output of that frame (no
//             filter at 48 -> 48); the same at 44.1 kHz stays the half-band (22,050,
//             (end - start) / 2 samples); HostResample stores at the engine's rate at 48
//             and offers nothing at 32
#include <cmath>
#include <cstring>
#include <vector>

#include "common/host_resample.h"
#include "common/ui_harness.h"
#include "itests/itest_util.h"
#include "itests/rate_util.h"
#include "pg/sampler/flash_pool.h"
#include "pg/sampler/melodic.h"
#include "pg/sampler/recorder.h"
#include "pg/sampler/resampler.h"
#include "pg/sampler/sample_pool.h"
#include "pg/sampler/sampler.h"

#pragma GCC diagnostic ignored "-Wdouble-promotion"

using namespace pg;
using sampler::RecRates;
using sampler::RecState;
using sampler::Recorder;
using sampler::Resampler;

namespace {

Instrument g_inst;
sampler::MemoryPool g_pool;

void testRates() {
  const RecRates c = sampler::recRatesFor(44100, sampler::kPoolRate);
  it::check(c.in == 44100 && c.out == 22050 && c.valid() && c.factor() == 2, "Cardputer: 44.1 -> 22.05");
  const RecRates d = sampler::recRatesFor(48000, sampler::kPoolRate);
  it::check(d.in == 48000 && d.out == 48000 && d.valid() && d.factor() == 1, "48 kHz into a 22,050 pool: stored at 48");
  const RecRates h = sampler::recRatesFor(48000, 24000);
  it::check(h.out == 24000 && h.valid() && h.factor() == 2, "48 kHz into a 24 kHz pool: halved");
  it::check(!RecRates{48000, 32000}.valid() && !RecRates{96000, 96000}.valid() && !RecRates{44100, 44100 / 3}.valid(),
            "invalid pairs");
  Recorder r;
  hostrs::VecSink s;
  r.start(&s, sampler::kSlotMaxSamples, RecRates{48000, 32000});
  it::check(r.state() == RecState::Failed, "an invalid pair fails at once");
  r.start(&s);
  it::check(r.state() == RecState::Settling && r.rates().out == 22050 && r.preRollSamples() == Recorder::kPreRoll &&
                r.tailSamples() == Recorder::kTail,
            "the default: the Cardputer's rates and counts");
}

// A room at -60 dBFS for 0.3 s, then a 440 Hz tone at -12 dBFS for 1.2 s, at `in` Hz;
// recorded with `rates`. Returns the recording.
std::vector<int16_t> recordTone(RecRates rates, Recorder& r, uint32_t* onset) {
  hostrs::VecSink sink;
  r.start(&sink, 48000u * 6u, rates);
  const uint32_t in = rates.in;
  std::vector<int16_t> x;
  uint32_t seed = 12345;
  const uint32_t quiet = uint32_t(0.3 * in), tone = uint32_t(1.2 * in);
  for (uint32_t i = 0; i < quiet + tone; ++i) {
    seed = seed * 1664525u + 1013904223u;
    double v = (double(int32_t(seed)) / 2147483648.0) * 32.0;
    if (i >= quiet) v += 8192.0 * std::sin(2.0 * 3.14159265358979 * 440.0 * double(i - quiet) / in);
    x.push_back(int16_t(std::lround(v)));
  }
  for (size_t i = 0; i < x.size(); i += 256) r.feed(x.data() + i, int(std::min<size_t>(256, x.size() - i)));
  r.release();
  r.finish();
  // where the tone starts in the recording (the first sample above the threshold)
  *onset = 0;
  for (uint32_t i = 0; i < sink.v.size(); ++i)
    if (std::abs(int(sink.v[i])) > int(r.threshold())) {
      *onset = i;
      break;
    }
  std::vector<int16_t> out(sink.v.begin(), sink.v.begin() + std::min<size_t>(sink.v.size(), r.length()));
  return out;
}

template <class V>
double playbackHz(V& v, float fs, int slot, int note, double* lastMs, const sampler::SamplePool* pool = &g_pool) {
  uint8_t p[kSoundParams];
  rt::defaults(v, p);
  v.setPool(pool);
  v.init(fs);
  std::vector<float> out;
  float l[kQuantum], r[kQuantum];
  const long n = long(1.4 * fs / kQuantum);
  for (long q = 0; q < n; ++q) {
    v.setParams(p);
    if (q == 0) {
      v.setSampleSlot(uint8_t(slot));
      v.noteOn(NoteEvent{uint8_t(note), 100, false, false, 0});
    }
    if (v.active()) {
      v.render(l, r, kQuantum);
      for (int i = 0; i < kQuantum; ++i) out.push_back(0.5f * (l[i] + r[i]));
    } else {
      for (int i = 0; i < kQuantum; ++i) out.push_back(0.0f);
    }
  }
  // the last sounding sample: how long the one-shot plays
  size_t last = 0;
  for (size_t i = 0; i < out.size(); ++i)
    if (std::fabs(out[i]) > 0.02f) last = i;
  *lastMs = double(last) * 1000.0 / fs;
  const rt::Spectrum s = rt::spectrum(out, fs, size_t(0.2 * fs), size_t(0.6 * fs), 0.1);
  return rt::peakHz(s, 400.0, 480.0);
}

void testMic() {
  static Recorder r;
  for (const RecRates rates : {RecRates{48000, 48000}, RecRates{48000, 24000}, RecRates{44100, 22050}}) {
    uint32_t onset = 0;
    static std::vector<int16_t> rec[3];
    const int k = rates.out == 48000 ? 0 : (rates.out == 24000 ? 1 : 2);
    rec[k] = recordTone(rates, r, &onset);
    const double fsOut = rates.out;
    it::check(r.state() == RecState::Done, "%u -> %u: %s", rates.in, rates.out, sampler::recStateName(r.state()));
    // the pre-roll: 40 ms in front of the onset (the threshold crossing), and the tail
    const double preMs = onset * 1000.0 / fsOut;
    it::check(std::fabs(preMs - 40.0) < 1.5, "%u -> %u: pre-roll %.2f ms", rates.in, rates.out, preMs);
    const double lenMs = double(r.length()) * 1000.0 / fsOut;
    // recorded: 40 ms pre-roll + the 1.2 s tone - the 60 ms tail
    it::check(std::fabs(lenMs - (40.0 + 1200.0 - 60.0)) < 3.0, "%u -> %u: %.1f ms long", rates.in, rates.out, lenMs);
    it::check(r.preRollSamples() == uint32_t(rates.out) * 40 / 1000 && r.tailSamples() == uint32_t(rates.out) * 60 / 1000,
              "%u -> %u: the times in samples at the stored rate", rates.in, rates.out);
    // the stored tone
    std::vector<float> f(rec[k].begin(), rec[k].end());
    const rt::Spectrum s = rt::spectrum(f, fsOut, onset + size_t(0.1 * fsOut), size_t(0.8 * fsOut), 0.1);
    const double hz = rt::peakHz(s, 400, 480);
    it::check(std::fabs(rt::cents(hz, 440.0)) < 1.0, "%u -> %u: stored tone %.3f Hz", rates.in, rates.out, hz);
    // into a slot with its rate in the header; both samplers at 48 and 44.1 kHz play 440 Hz
    it::check(g_pool.set(2 + k, rec[k].data(), uint32_t(rec[k].size()), "MIC", rates.out), "pool takes it");
    it::check(g_pool.header(2 + k)->rate == rates.out, "header rate %u", g_pool.header(2 + k)->rate);
    for (float fs : {48000.0f, 44100.0f}) {
      sampler::SamplerVoice sv;
      sampler::MelodicSampler ms;
      double lastA = 0, lastB = 0;
      const double a = playbackHz(sv, fs, 2 + k, 60, &lastA), b = playbackHz(ms, fs, 2 + k, 48, &lastB);
      it::check(std::fabs(rt::cents(a, 440.0)) < 1.0 && std::fabs(rt::cents(b, 440.0)) < 1.0,
                "%u Hz slot at a %.0f Hz engine: %.3f Hz (sampler), %.3f (melodic)", rates.out, double(fs), a, b);
      // the one-shot plays as long as it was recorded (to within a quantum and its release)
      it::check(std::fabs(lastA - lenMs) < 15.0, "%u Hz slot at %.0f Hz: plays %.1f ms of %.1f", rates.out, double(fs),
                lastA, lenMs);
    }
    std::printf("  mic    %5u -> %5u: %s, pre-roll %.2f ms, %.1f ms long, stored %.3f Hz, played back at 440 Hz\n",
                rates.in, rates.out, sampler::recStateName(r.state()), preMs, lenMs, hz);
  }
}

// The recording committed as the devices commit it: streamed into an erased slot of a
// FlashPool (a RAM flash with NOR rules) and closed by SampleWriter::commitRecording
// at the Recorder's stored rate.
void testCommit() {
  static std::vector<uint8_t> image(sampler::kPoolBytes, 0xFF);
  static sampler::RamFlash flash(image.data());
  static sampler::FlashPool pool;
  static sampler::SampleWriter w;
  pool.attach(image.data());
  w.attach(&flash, &pool);
  static Recorder r;
  for (const RecRates rates : {RecRates{48000, 48000}, RecRates{44100, 22050}}) {
    const int slot = rates.out == 48000 ? 3 : 4;
    uint32_t onset = 0;
    const std::vector<int16_t> rec = recordTone(rates, r, &onset);
    const double lenMs = double(r.length()) * 1000.0 / double(rates.out);
    it::check(w.beginRecording(slot) == sampler::SampleWriter::kOk, "%u Hz: recording opened", rates.out);
    it::check(w.write(0, reinterpret_cast<const uint8_t*>(rec.data()), uint32_t(rec.size() * 2)) ==
                  sampler::SampleWriter::kOk,
              "%u Hz: written", rates.out);
    char name[12];
    std::snprintf(name, sizeof name, "REC%02d", slot + 1);
    it::check(w.commitRecording(r.length(), name, r.rates().out) == sampler::SampleWriter::kOk, "%u Hz: committed",
              rates.out);
    const sampler::SlotHeader* h = pool.header(slot);
    it::check(h && h->rate == rates.out && h->samples == r.length(), "%u Hz: the header says %u Hz, %u samples",
              rates.out, h ? h->rate : 0u, h ? h->samples : 0u);
    for (float fs : {48000.0f, 44100.0f}) {
      sampler::SamplerVoice sv;
      sampler::MelodicSampler ms;
      double lastA = 0, lastB = 0;
      const double a = playbackHz(sv, fs, slot, 60, &lastA, &pool), b = playbackHz(ms, fs, slot, 48, &lastB, &pool);
      it::check(std::fabs(rt::cents(a, 440.0)) < 1.0 && std::fabs(rt::cents(b, 440.0)) < 1.0,
                "committed %u Hz slot at a %.0f Hz engine: %.3f Hz (sampler), %.3f (melodic)", rates.out, double(fs), a,
                b);
      it::check(std::fabs(lastA - lenMs) < 15.0, "committed %u Hz slot at %.0f Hz: plays %.1f ms of %.1f", rates.out,
                double(fs), lastA, lenMs);
      if (fs == 48000.0f)
        std::printf("  commit %5u Hz through SampleWriter: header %u Hz, %u samples, played at a 48 kHz engine "
                    "%.3f Hz for %.1f ms of %.1f\n",
                    rates.out, h ? h->rate : 0u, h ? h->samples : 0u, a, lastA, lenMs);
    }
  }
}

// ---- resampling ---------------------------------------------------------------------

struct Stream {
  std::vector<int16_t> mono;
};
// A tap in front of the resampler: everything the engine plays (the mono sum, as the
// resampler sums it) and where the bar lines fire.
struct SpyTap final : OutputTap {
  Resampler* rs = nullptr;
  Stream* s = nullptr;
  std::vector<long> bars;
  void quantum(const int16_t* st, int frames, uint8_t flags) override {
    if (flags & kTapBar) bars.push_back(long(s->mono.size()));
    for (int i = 0; i < frames; ++i) s->mono.push_back(int16_t((int32_t(st[2 * i]) + int32_t(st[2 * i + 1])) >> 1));
    if (rs) rs->quantum(st, frames, flags);
  }
};

void testResample(float fs) {
  g_inst.init(fs, false);
  g_inst.setSamplePool(&g_pool);
  g_inst.engine().suspend();
  uih::buildMockup(g_inst.project(), g_inst.engine());
  g_inst.project().globals[kGTempo] = 124;
  g_inst.project().globals[kGSwing] = 8;
  g_inst.engine().sanitizeProject(g_inst.project());
  g_inst.engine().resume();
  static Stream s;
  s.mono.clear();
  static SpyTap spy;
  spy = SpyTap{};
  spy.s = &s;
  g_inst.engine().setOutputTap(&spy);
  it::post(g_inst, CmdType::Play);
  int16_t buf[2 * 128];
  auto run = [&](long frames, Resampler* rs) {
    for (long f = 0; f < frames; f += 128) {
      g_inst.process(buf, 128);
      if (rs) rs->pump();
    }
  };
  run(long(fs * 0.7), nullptr);   // into the first bar
  static Recorder rec;
  static hostrs::VecSink sink;
  sink.v.clear();
  static std::vector<int16_t> ring(1u << 17);
  static Resampler rs;
  const RecRates rates = sampler::recRatesFor(uint32_t(fs), sampler::kPoolRate);
  const long t0 = long(s.mono.size());
  rs.start(&rec, &sink, ring.data(), uint32_t(ring.size()), true, sampler::kSlotMaxSamples, rates);
  spy.rs = &rs;
  const double bar = 16.0 * fs * 15.0 / 124.0;
  run(long(bar * 1.6), &rs);   // past the next bar line, into the second bar of the take
  rs.release();
  for (int i = 0; i < 4000 && !rs.ended(); ++i) run(128, &rs);
  spy.rs = nullptr;
  it::check(rs.ended() && rs.state() == RecState::Done, "%.0f Hz: resample %s", double(fs),
            sampler::recStateName(rs.state()));
  const long start = t0 + long(rs.startFrame());
  // the bar lines it starts and ends on
  const uint32_t f = rates.factor();
  const long end = start + long(rs.endFrame());
  bool onStart = false, onEnd = false;
  for (long b : spy.bars) {
    onStart |= b == start;
    onEnd |= b == end;
  }
  it::check(onStart, "%.0f Hz: starts on a bar line (frame %ld)", double(fs), start);
  it::check(onEnd, "%.0f Hz: ends on a bar line (frame %ld)", double(fs), end);
  const long frames = end - start;
  const double nBars = std::round(double(frames) / bar);
  it::check(nBars >= 1.0 && long(rs.length()) == (frames + long(f) - 1) / long(f), "%.0f Hz: %u samples for %ld frames",
            double(fs), rs.length(), frames);
  it::check(std::fabs(double(frames) - nBars * bar) < kQuantum, "%.0f Hz: %.0f bar(s) are %ld frames (exact %.1f)",
            double(fs), nBars, frames, nBars * bar);
  long bad = 0;
  if (f == 1) {
    for (uint32_t j = 0; j < rs.length(); ++j)
      if (sink.v[j] != s.mono[size_t(start) + j]) ++bad;
  } else {
    // the half-band's value centred on frame start + 2k (itest_resample checks the
    // same thing at length): spot-check that the decimated take follows the stream
    for (uint32_t j = 100; j < rs.length(); j += 997) {
      const long c = start + 2 * long(j);
      float y = 0.5f * float(s.mono[size_t(c)]);
      for (int k = 0; k < sampler::HalfBand::kUnique; ++k) {
        const int d = 2 * k + 1;
        y += sampler::HalfBand::kCoef[k] * (float(s.mono[size_t(c - d)]) + float(s.mono[size_t(c + d)]));
      }
      const float yr = y + (y < 0.0f ? -0.5f : 0.5f);
      if (sink.v[j] != int16_t(yr > 32767.0f ? 32767 : (yr < -32768.0f ? -32768 : int(yr)))) ++bad;
    }
  }
  it::check(bad == 0, "%.0f Hz: %ld samples differ from the engine's own output", double(fs), bad);
  std::printf("  resample at %5.0f Hz: stored at %5u Hz, %u samples = %.0f bar(s) of %ld frames (exact %.1f), %s\n",
              double(fs), rates.out, rs.length(), nBars, frames, nBars * bar,
              bad ? "DIFFERS" : (f == 1 ? "bit for bit the engine's output" : "the half-band of the engine's output"));
  g_inst.engine().setOutputTap(nullptr);
}

void testHostResample() {
  hostrs::HostResample h;
  const char* why = "";
  for (float fs : {44100.0f, 48000.0f, 32000.0f}) {
    g_inst.init(fs, false);
    h.init(&g_inst.engine(), &g_inst.sampler(), fs,
           [](int, const int16_t*, uint32_t, uint32_t, std::string*) { return true; });
    const bool ok = h.ready(&why);
    if (fs == 32000.0f) it::check(!ok && std::strcmp(why, "RESAMPLE: 44.1K ONLY") == 0, "32 kHz: not offered");
    else it::check(ok && h.storedRate() == (fs == 48000.0f ? 48000u : 22050u), "%.0f Hz: stores at %u", double(fs),
                   h.storedRate());
  }
}

}  // namespace

int main() {
  std::printf("itest_rate48_record: recording at 48 kHz\n");
  testRates();
  testMic();
  testCommit();
  testResample(48000.0f);
  testResample(44100.0f);
  testHostResample();
  return it::finish("itest_rate48_record");
}
