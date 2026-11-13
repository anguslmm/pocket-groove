// Microphone recording, the portable part (pg/sampler/recorder.h, stage F2), and the
// pool's recording path (flash_pool.h), on the host:
//   halfband   the decimator's response: flat to 9 kHz, -6 dB at 11,025 Hz, and every
//              input tone from 13.05 kHz up comes out (folded) at least 65 dB down
//   dc         a DC offset on the input is gone from what is written
//   preroll    noise, then a burst: the recording starts kPreRoll samples before the
//              burst (within the filter's delay), the start is quiet, the end trimmed
//   settle     a key click in the first 50 ms neither starts it nor lifts the threshold;
//              the threshold follows the room (floor + 20 dB, clamped)
//   states     release while waiting = Cancelled (nothing written); a full slot stops
//              by itself (Full, exactly max samples); a blip shorter than the trim =
//              Cancelled; a refused write = Failed; feed after the end does nothing
//   flash      erased() through the mapping; a recording refuses a dirty slot; remove()
//              erases all 68 sectors; beginRecording / write / commitRecording into a
//              RamFlash image: the header (name, length, peak, gain), a reboot finds it
//   playback   the committed recording through SamplerVoice plays at the level a put
//              sample of the same sound plays at (the header's gain), within 0.5 dB
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#include "pg/sampler/flash_pool.h"
#include "pg/sampler/recorder.h"
#include "pg/sampler/sampler.h"

using namespace pg;
using namespace pg::sampler;

namespace {

int g_fail = 0, g_pass = 0;
void check(bool ok, const char* what, double a = 0, double b = 0) {
  if (ok) {
    ++g_pass;
  } else {
    ++g_fail;
    std::printf("  FAIL %s (%g, %g)\n", what, a, b);
  }
}

constexpr double kPi = 3.14159265358979323846;

// Amplitude of frequency f (Hz at rate fs) in x[from..], by projection.
double amplitude(const std::vector<float>& x, size_t from, double f, double fs) {
  double c = 0, s = 0;
  const size_t n = x.size() - from;
  for (size_t i = from; i < x.size(); ++i) {
    const double ph = 2.0 * kPi * f * double(i) / fs;
    c += double(x[i]) * std::cos(ph);
    s += double(x[i]) * std::sin(ph);
  }
  return 2.0 * std::sqrt(c * c + s * s) / double(n);
}

double rmsOf(const std::vector<float>& x, size_t from) {
  double e = 0;
  for (size_t i = from; i < x.size(); ++i) e += double(x[i]) * double(x[i]);
  return std::sqrt(e / double(x.size() - from));
}

std::vector<float> halfband(double f, double amp, int inSamples) {
  HalfBand hb;
  hb.reset();
  std::vector<float> out;
  for (int i = 0; i + 1 < inSamples; i += 2) {
    const float a = float(amp * std::sin(2.0 * kPi * f * double(i) / 44100.0));
    const float b = float(amp * std::sin(2.0 * kPi * f * double(i + 1) / 44100.0));
    out.push_back(hb.run(a, b));
  }
  return out;
}

void testHalfBand() {
  std::printf("halfband\n");
  const double amp = 10000.0;
  for (double f : {100.0, 1000.0, 4000.0, 8000.0, 9000.0}) {
    const std::vector<float> y = halfband(f, amp, 88200);
    const double db = 20.0 * std::log10(amplitude(y, 1000, f, 22050.0) / amp);
    check(std::fabs(db) < 0.02, "pass band flat", f, db);
  }
  {
    const std::vector<float> y = halfband(11025.0 - 0.5, amp, 88200);   // just below fs/4 (a whole number of cycles)
    const double db = 20.0 * std::log10(rmsOf(y, 1000) * std::sqrt(2.0) / amp);
    check(std::fabs(db + 6.02) < 0.3, "-6 dB at a quarter of the input rate", db);
  }
  for (double f : {13050.0, 14000.0, 15000.0, 17000.0, 19000.0, 21000.0, 22000.0}) {
    const std::vector<float> y = halfband(f, amp, 88200);
    const double db = 20.0 * std::log10(rmsOf(y, 1000) * std::sqrt(2.0) / amp + 1e-12);
    check(db < -65.0, "no aliasing from the stop band", f, db);
  }
  // DC gain exactly 1 (the taps sum to 1)
  double sum = 0.5;
  for (float c : HalfBand::kCoef) sum += 2.0 * double(c);
  check(std::fabs(sum - 1.0) < 1e-6, "taps sum to 1", sum);
}

struct RamSink final : RecordSink {
  std::vector<int16_t> v;
  bool fail = false;
  int writes = 0;
  bool write(uint32_t offset, const int16_t* d, uint32_t n) override {
    ++writes;
    if (fail || offset != v.size()) return false;
    v.insert(v.end(), d, d + n);
    return true;
  }
};

struct FlashSink final : RecordSink {
  SampleWriter* w = nullptr;
  bool write(uint32_t offset, const int16_t* d, uint32_t n) override {
    return w->write(offset * 2u, reinterpret_cast<const uint8_t*>(d), n * 2u) == SampleWriter::kOk;
  }
};

// Feeds x (44.1 kHz) in blocks of 128, as the device's writer task does.
void feedAll(Recorder& r, const std::vector<int16_t>& x) {
  for (size_t i = 0; i < x.size(); i += 128) r.feed(x.data() + i, int(std::min<size_t>(128, x.size() - i)));
}

// Noise (rms ~ noiseRms), optional DC, a burst of a 700 Hz tone of amplitude amp from
// burstAt for burstLen samples (44.1 kHz); n samples in all.
std::vector<int16_t> scene(size_t n, double noiseRms, double dc, size_t burstAt, size_t burstLen, double amp,
                           unsigned seed = 1) {
  std::mt19937 rng(seed);
  std::normal_distribution<double> nd(0.0, noiseRms);
  std::vector<int16_t> x(n);
  for (size_t i = 0; i < n; ++i) {
    double v = dc + nd(rng);
    if (i >= burstAt && i < burstAt + burstLen) v += amp * std::sin(2.0 * kPi * 700.0 * double(i - burstAt) / 44100.0);
    x[i] = int16_t(std::lround(std::max(-32768.0, std::min(32767.0, v))));
  }
  return x;
}

void testPreRoll() {
  std::printf("preroll, dc, trim\n");
  RamSink sink;
  Recorder r;
  r.start(&sink);
  check(r.state() == RecState::Settling, "settling at start");
  const size_t burstAt = 17640;   // 0.4 s
  const std::vector<int16_t> x = scene(44100, 30.0, 2000.0, burstAt, 22050, 8000.0);
  feedAll(r, x);
  check(r.state() == RecState::Recording, "recording after the burst");
  check(r.threshold() >= Recorder::kThrMin && r.threshold() <= 400, "threshold from a quiet floor", r.threshold());
  r.release();
  r.finish();
  check(r.state() == RecState::Done, "done after the release");
  const uint32_t len = r.length();
  check(sink.v.size() == r.recorded() && len + Recorder::kTail == r.recorded(), "the tail is trimmed", len,
        r.recorded());
  // the burst's onset in the recording: the first sample above 1000
  size_t onset = 0;
  while (onset < sink.v.size() && std::abs(int(sink.v[onset])) < 1000) ++onset;
  // the burst starts at 8820 out; the half-band delays it by 11.5 samples
  check(std::abs(int(onset) - int(Recorder::kPreRoll)) < 25, "pre-roll in front of the onset", double(onset),
        Recorder::kPreRoll);
  int quiet = 0;
  for (size_t i = 0; i + 40 < onset; ++i) quiet = std::max(quiet, std::abs(int(sink.v[i])));
  check(quiet < 200, "the pre-roll is the quiet room (no DC step)", quiet);
  double mean = 0;
  for (size_t i = 0; i < len; ++i) mean += sink.v[i];
  mean /= double(len);
  check(std::fabs(mean) < 3.0, "the 2000 LSB DC offset is gone", mean);
  check(r.peak() > 7000 && r.peak() < 8800, "peak of the burst", r.peak());
  // the growing picture covers what was recorded
  const int cols = int((uint64_t(r.recorded() - 1) * kOverviewCols) / kSlotMaxSamples) + 1;
  check(r.colsUsed() == cols, "picture columns", r.colsUsed(), cols);
  // feed after the end does nothing
  const uint32_t rec = r.recorded();
  feedAll(r, x);
  check(r.recorded() == rec && r.state() == RecState::Done, "nothing after the end");
}

void testSettle() {
  std::printf("settle\n");
  {
    // a key click (5 ms at -15 dBFS) 20 ms in: no start, threshold from the room
    RamSink sink;
    Recorder r;
    r.start(&sink);
    std::vector<int16_t> x = scene(44100, 40.0, 0.0, 882, 220, 6000.0);
    feedAll(r, x);
    check(r.state() == RecState::Waiting, "a click in the first 50 ms does not start it");
    check(r.threshold() < 600, "nor lift the threshold", r.threshold());
    check(sink.writes == 0, "nothing written");
    r.release();
    r.finish();
    check(r.state() == RecState::Cancelled, "release while waiting = cancelled");
  }
  {
    // a loud room: the threshold goes up with it, to the cap
    RamSink sink;
    Recorder r;
    r.start(&sink);
    feedAll(r, scene(22050, 900.0, 0.0, 0, 0, 0.0));
    check(r.state() == RecState::Waiting && r.threshold() == Recorder::kThrMax, "loud room: capped threshold",
          r.threshold());
  }
  {
    // released while settling
    RamSink sink;
    Recorder r;
    r.start(&sink);
    feedAll(r, scene(2000, 10.0, 0.0, 0, 0, 0.0));
    check(r.state() == RecState::Settling, "still settling");
    r.release();
    r.finish();
    check(r.state() == RecState::Cancelled && sink.writes == 0, "release while settling = cancelled");
  }
}

void testStates() {
  std::printf("states\n");
  {
    // full: stops by itself at exactly max
    RamSink sink;
    Recorder r;
    r.start(&sink, 20000);
    feedAll(r, scene(110250, 20.0, 0.0, 11025, 100000, 9000.0));
    check(r.state() == RecState::Full, "full");
    check(r.length() == 20000 && sink.v.size() == 20000 && r.recorded() == 20000, "exactly max", r.length(),
          double(sink.v.size()));
    check(r.colsUsed() == kOverviewCols, "the picture is full", r.colsUsed());
  }
  {
    // a blip: shorter than the trim after the pre-roll -> cancelled
    RamSink sink;
    Recorder r;
    r.start(&sink);
    feedAll(r, scene(14000, 20.0, 0.0, 13230, 200, 9000.0));   // let go right after it
    check(r.state() == RecState::Recording, "a blip starts it");
    r.release();
    r.finish();
    check(r.state() == RecState::Cancelled, "a blip shorter than the trim is no sample", r.recorded());
  }
  {
    // the sink refuses
    RamSink sink;
    sink.fail = true;
    Recorder r;
    r.start(&sink);
    feedAll(r, scene(44100, 20.0, 0.0, 8820, 30000, 9000.0));
    check(r.state() == RecState::Failed && r.sinkErrors() == 1, "a refused write fails it");
  }
  {
    // restart resets everything
    RamSink sink;
    Recorder r;
    r.start(&sink);
    feedAll(r, scene(30000, 20.0, 0.0, 8820, 30000, 9000.0));
    RamSink sink2;
    r.start(&sink2);
    check(r.state() == RecState::Settling && r.recorded() == 0 && r.colsUsed() == 0 && r.threshold() == 0, "restart");
  }
}

void testFlashAndPlayback() {
  std::printf("flash, playback\n");
  static std::vector<uint8_t> img(kPoolBytes, 0xFF);
  RamFlash flash(img.data());
  FlashPool pool;
  pool.attach(img.data());
  SampleWriter w;
  w.attach(&flash, &pool);
  check(w.erased(3), "an erased image: slot 4 erased");
  // a transfer cut off: empty but dirty
  std::vector<int16_t> junk(4000, 1234);
  check(w.begin(3, 4000) == SampleWriter::kOk, "put begins");
  w.write(0, reinterpret_cast<const uint8_t*>(junk.data()), 8000);
  w.cancel();
  SampleRef ref;
  check(!pool.user(3, &ref) && !w.erased(3), "cut off: empty, not erased");
  check(w.beginRecording(3) == SampleWriter::kFlash, "a recording refuses a dirty slot");
  const uint32_t e0 = flash.erases;
  check(w.remove(3) == SampleWriter::kOk && w.erased(3), "remove erases all of it");
  check(flash.erases == e0 + 1, "in one driver call", flash.erases - e0);
  check(w.erased(2) && w.erased(4), "the neighbours untouched");
  // record a quiet burst (peak ~2000: -24 dBFS) into slot 4
  check(w.beginRecording(3) == SampleWriter::kOk && w.recording(), "begin recording");
  const uint32_t e1 = flash.erases;
  FlashSink sink;
  sink.w = &w;
  Recorder r;
  r.start(&sink);
  const std::vector<int16_t> x = scene(66150, 15.0, 0.0, 13230, 33075, 2000.0, 7);
  feedAll(r, x);
  r.release();
  r.finish();
  check(r.state() == RecState::Done, "done");
  check(flash.erases == e1, "nothing erased while recording", flash.erases - e1);
  char name[16];
  std::snprintf(name, sizeof name, "REC%02d", 4);
  check(w.commitRecording(r.length(), name) == SampleWriter::kOk, "commit");
  const SlotHeader* h = pool.header(3);
  check(h && h->samples == r.length(), "the header's length", h ? h->samples : 0, r.length());
  check(h && std::strncmp(h->name, "REC04", 5) == 0, "named REC04");
  const double g = h ? double(gainOf(*h)) : 0.0;
  check(h && std::fabs(g - double(kRecordTargetPeak) / double(h->peak)) < 0.01, "gain levels the peak", g,
        h ? h->peak : 0);
  check(pool.user(3, &ref) && std::fabs(double(ref.gain) - g) < 1e-6, "the voice gets the gain", double(ref.gain), g);
  FlashPool reboot;
  reboot.attach(img.data());
  check(reboot.header(3) && reboot.header(3)->gain == h->gain, "a reboot finds it");
  // the overview shows what plays: its biggest column near the full height
  int big = 0;
  for (int c = 0; c < kOverviewCols; ++c) big = std::max(big, std::max(std::abs(int(h->overview[c][0])), std::abs(int(h->overview[c][1]))));
  check(big > 100, "the picture is levelled too", big);
  // playback: the same burst put normalised (as the converter would) into slot 6
  std::vector<int16_t> norm(r.length());
  const int16_t* rec = reinterpret_cast<const int16_t*>(img.data() + 3 * kSlotBytes + kSectorBytes);
  int pk = 0;
  for (uint32_t i = 0; i < r.length(); ++i) pk = std::max(pk, std::abs(int(rec[i])));
  for (uint32_t i = 0; i < r.length(); ++i) norm[i] = int16_t(std::lround(double(rec[i]) * kRecordTargetPeak / pk));
  w.remove(5);
  check(w.begin(5, r.length()) == SampleWriter::kOk, "put a levelled copy");
  w.write(0, reinterpret_cast<const uint8_t*>(norm.data()), r.length() * 2);
  check(w.commit(crc32(norm.data(), r.length() * 2), "COPY") == SampleWriter::kOk, "commit the copy");
  auto play = [&](int slot) {
    SamplerVoice v;
    v.setPool(&pool);
    v.init(44100.0f);
    uint8_t p[kSoundParams] = {};
    for (int i = 0; i < kNumParams; ++i) p[i] = paramInfo(i)->def;
    v.setParams(p);
    v.setSampleSlot(uint8_t(slot));
    NoteEvent e{};
    e.note = 60;
    e.vel = 100;
    v.noteOn(e);
    float l[64], rr[64];
    double peak = 0;
    for (int b = 0; b < 2000; ++b) {
      v.render(l, rr, 64);
      for (float s : l) peak = std::max(peak, double(std::fabs(s)));
    }
    return peak;
  };
  const double pRec = play(3), pPut = play(5);
  const double dDb = 20.0 * std::log10(pRec / pPut);
  check(pRec > 0.1 && std::fabs(dDb) < 0.5, "the recording plays as loud as a put", pRec, dDb);
  // after a recording, rm leaves it erased again; a put still works there
  check(w.remove(3) == SampleWriter::kOk && w.erased(3) && !pool.user(3, &ref), "rm: empty and erased");
}

}  // namespace

int main() {
  testHalfBand();
  testPreRoll();
  testSettle();
  testStates();
  testFlashAndPlayback();
  std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
