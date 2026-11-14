// Host test of the line-in recording path (daisy/core/line_mic.h) over the Daisy's
// pool (an SDRAM image with a flash mirror, pool_mirror.h; pg's SampleWriter and
// FlashPool unchanged), fed as the audio interrupt feeds it: 96-frame interleaved
// stereo float blocks, polled every UI frame (33 ms, 16-17 blocks).
//   mono      a 440 Hz tone on L, R digital silence (a mono plug): recorded as L
//             alone; the slot's header says 48,000 Hz; the stored tone is 440 Hz at
//             that rate; the length is what was played minus the tail plus the
//             pre-roll; the header's gain levels it
//   stereo    L 440 Hz and R 660 Hz: (L+R)/2, both in the sample
//   hum on R  a right input that is not silent (an open breadboard pin): summed
//   frozen    R silent until the recording has started: stays L alone (no level step)
//   silence   no sound: Cancelled, the slot empty and still erased
//   overrun   the main loop away for longer than the ring: counted, the recording ends
//   restart   the mirror flushed to its flash, a new image loaded from it: the slot
//             is there with its rate and audio (a reboot)
//   commit    the core's SampleWriter::commitRecording at the stored rate: 48,000 in
//             the header; a rate outside kMinRate..kMaxRate refused, nothing written
#include <cmath>
#include <cstring>
#include <vector>

#include "check.h"
#include "core/line_mic.h"
#include "core/pool_mirror.h"
#include "pg/sampler/sample_format.h"

using namespace pgd;
using pg::sampler::RecState;

namespace {

constexpr uint32_t kRate = 48000;
constexpr int kBlock = 96;
constexpr int kSlot = 5;

struct Rig {
  std::vector<uint8_t> image = std::vector<uint8_t>(pg::sampler::kPoolBytes, 0xFF);
  std::vector<uint8_t> flash = std::vector<uint8_t>(pg::sampler::kPoolBytes, 0xFF);
  std::vector<uint8_t> staging = std::vector<uint8_t>(4096);
  std::vector<int16_t> ring = std::vector<int16_t>(65536);
  pg::sampler::RamFlash backing{flash.data()};
  MirrorFlash mirror;
  pg::sampler::FlashPool pool;
  pg::sampler::SampleWriter writer;
  LineMic mic;
  uint64_t n = 0;   // input frames so far
  Rig() {
    mirror.init(image.data(), &backing, staging.data());
    mirror.loadFrom();
    pool.attach(mirror.image());
    writer.attach(&mirror, &pool);
    mic.init(&writer, ring.data(), uint32_t(ring.size()), kRate);
    mic.setAvailable(true);
  }
};

// One block of the input: l(t), r(t) in -1..1.
template <class L, class R>
void block(Rig& rig, L l, R r) {
  float in[2 * kBlock];
  for (int i = 0; i < kBlock; ++i, ++rig.n) {
    const double t = double(rig.n) / kRate;
    in[2 * i] = float(l(t));
    in[2 * i + 1] = float(r(t));
  }
  rig.mic.audioIn(in, kBlock);
}

double tone(double f, double a, double t) { return a * std::sin(2.0 * 3.14159265358979 * f * t); }

uint32_t g_seed = 99;
double noise(double a) {
  g_seed = g_seed * 1664525u + 1013904223u;
  return a * double(int32_t(g_seed)) / 2147483648.0;
}

// Plays `ms` of input, polling every 33 ms; returns the last view.
template <class L, class R>
pg::sampler::RecView play(Rig& rig, double ms, L l, R r, bool pollIt = true) {
  pg::sampler::RecView v{};
  const int blocks = int(ms * kRate / 1000.0 / kBlock + 0.5);
  for (int b = 0; b < blocks; ++b) {
    block(rig, l, r);
    if (pollIt && (b % 16) == 15) rig.mic.poll(&v);
  }
  return v;
}

// Polls until the recording has ended; returns the final view.
pg::sampler::RecView finish(Rig& rig) {
  pg::sampler::RecView v{}, last{};
  for (int i = 0; i < 100 && rig.mic.poll(&v); ++i) last = v;
  return last;
}

// Zero crossings per second of the slot's audio.
double frequency(const int16_t* d, uint32_t n, uint32_t rate) {
  int cross = 0;
  for (uint32_t i = 1; i < n; ++i) cross += (d[i - 1] < 0) != (d[i] < 0);
  return double(cross) / 2.0 / (double(n) / rate);
}

const int16_t* audioOf(const Rig& rig, int slot) {
  return reinterpret_cast<const int16_t*>(rig.pool.base() + size_t(slot) * pg::sampler::kSlotBytes +
                                          pg::sampler::kSectorBytes);
}

void testMono() {
  Rig rig;
  const char* why = "";
  CHECK(rig.mic.ready(&why));
  CHECK(rig.mic.slotErased(kSlot));
  CHECK(rig.mic.start(kSlot));
  CHECK(rig.mic.armed());
  // 300 ms of room (-66 dBFS), then 1.0 s of tone on L (-12 dBFS); R is digital silence
  play(rig, 300, [](double) { return noise(0.0005); }, [](double) { return 0.0; });
  pg::sampler::RecView v = play(rig, 1000, [](double t) { return tone(440, 0.25, t) + noise(0.0005); },
                                [](double) { return 0.0; });
  CHECK(v.state == RecState::Recording);
  CHECK_EQ(v.rate, kRate);
  CHECK_EQ(v.capacity, pg::sampler::kSlotMaxSamples);
  rig.mic.release();
  v = finish(rig);
  CHECK(v.state == RecState::Done);
  CHECK(!rig.mic.armed());
  const pg::sampler::SlotHeader* h = rig.pool.header(kSlot);
  CHECK(h != nullptr);
  if (!h) return;
  CHECK_EQ(h->rate, kRate);
  char name[pg::sampler::kNameLen + 1];
  pg::sampler::headerName(*h, name);
  CHECK(std::strcmp(name, "REC06") == 0);
  // 1.0 s of tone + 40 ms of pre-roll - 60 ms of tail (and up to a poll's worth of input)
  const double secs = double(h->samples) / kRate;
  std::printf("  mono: %u samples = %.3f s at %u Hz, peak %u, gain %u/256\n", unsigned(h->samples), secs,
              unsigned(h->rate), unsigned(h->peak), unsigned(h->gain));
  CHECK(secs > 0.95 && secs < 1.02);
  CHECK(h->peak > 7000 && h->peak < 8600);   // -12 dBFS = 8192, as it came in
  CHECK(h->gain > 256);                       // levelled at playback (setPlaybackGain)
  const double f = frequency(audioOf(rig, kSlot) + 4800, h->samples - 9600, h->rate);
  CHECK(std::fabs(f - 440.0) < 5.0);
  CHECK(rig.mic.last().mono == LineMic::Mono::LeftOnly);
  CHECK_EQ(rig.mic.last().overruns, 0u);
  CHECK(rig.mic.last().peakR == 0);
  pg::sampler::SampleRef ref;
  CHECK(rig.pool.user(kSlot, &ref) && ref.rate == kRate && ref.length == h->samples);   // the voices see 48 kHz
  // the next poll: Off
  CHECK(!rig.mic.poll(&v));

  // RESTART: the mirror's dirty sectors to the flash, a new image from the flash
  while (rig.mirror.flushStep()) {
  }
  CHECK(!rig.mirror.dirty());
  std::vector<uint8_t> image2(pg::sampler::kPoolBytes, 0);
  std::vector<uint8_t> staging2(4096);
  MirrorFlash m2;
  m2.init(image2.data(), &rig.backing, staging2.data());
  CHECK(m2.loadFrom());
  pg::sampler::FlashPool p2;
  p2.attach(m2.image());
  CHECK_EQ(m2.dropBadSlots(p2), 0);
  const pg::sampler::SlotHeader* h2 = p2.header(kSlot);
  CHECK(h2 && h2->rate == kRate && h2->samples == h->samples && h2->dataCrc == h->dataCrc);
}

void testStereoAndHum() {
  {
    Rig rig;
    CHECK(rig.mic.start(kSlot));
    play(rig, 300, [](double) { return noise(0.0005); }, [](double) { return noise(0.0005); });
    play(rig, 600, [](double t) { return tone(440, 0.25, t); }, [](double t) { return tone(660, 0.25, t); });
    rig.mic.release();
    CHECK(finish(rig).state == RecState::Done);
    CHECK(rig.mic.last().mono == LineMic::Mono::Sum);
    const pg::sampler::SlotHeader* h = rig.pool.header(kSlot);
    CHECK(h && h->rate == kRate);
    // both sides in it: (L+R)/2 peaks near 0.25 (two tones of 0.25 at half gain each)
    if (h) CHECK(h->peak > 6000 && h->peak < 8700);
  }
  {
    Rig rig;   // hum on an open right input (-40 dBFS, 50 Hz): not silent, so summed
    CHECK(rig.mic.start(kSlot));
    play(rig, 300, [](double) { return noise(0.0005); }, [](double t) { return tone(50, 0.01, t); });
    play(rig, 400, [](double t) { return tone(440, 0.25, t); }, [](double t) { return tone(50, 0.01, t); });
    rig.mic.release();
    CHECK(finish(rig).state == RecState::Done);
    CHECK(rig.mic.last().mono == LineMic::Mono::Sum);
  }
  {
    Rig rig;   // R silent until the recording runs, then a signal: stays L alone
    CHECK(rig.mic.start(kSlot));
    play(rig, 300, [](double) { return noise(0.0005); }, [](double) { return 0.0; });
    play(rig, 300, [](double t) { return tone(440, 0.25, t); }, [](double) { return 0.0; });
    play(rig, 300, [](double t) { return tone(440, 0.25, t); }, [](double t) { return tone(660, 0.2, t); });
    rig.mic.release();
    CHECK(finish(rig).state == RecState::Done);
    CHECK(rig.mic.last().mono == LineMic::Mono::LeftOnly);
    CHECK(rig.mic.last().peakR > 6000);   // R did carry something (the report says so)
  }
}

void testSilenceAndOverrun() {
  {
    Rig rig;
    CHECK(rig.mic.start(kSlot));
    play(rig, 800, [](double) { return noise(0.0005); }, [](double) { return 0.0; });
    rig.mic.release();
    CHECK(finish(rig).state == RecState::Cancelled);
    CHECK(rig.pool.header(kSlot) == nullptr);
    CHECK(rig.writer.erased(kSlot));   // nothing was written: the next hold starts at once
    CHECK(!rig.writer.active());
  }
  {
    Rig rig;   // the main loop away for 2 s (longer than the 1.37 s ring)
    CHECK(rig.mic.start(kSlot));
    play(rig, 300, [](double) { return noise(0.0005); }, [](double) { return 0.0; });
    play(rig, 2000, [](double t) { return tone(440, 0.25, t); }, [](double) { return 0.0; }, false);
    rig.mic.release();
    const pg::sampler::RecView v = finish(rig);
    CHECK(v.state == RecState::Done);
    CHECK(rig.mic.last().overruns > 0);
  }
  {
    Rig rig;   // no input running: NO INPUT
    rig.mic.setAvailable(false);
    const char* why = "";
    CHECK(!rig.mic.ready(&why));
    CHECK(std::strcmp(why, "NO INPUT") == 0);
    CHECK(!rig.mic.start(kSlot));
  }
  {
    Rig rig;   // a slot that holds something is not erased: erase() first, as the UI does
    CHECK(rig.writer.begin(kSlot, 1000) == pg::sampler::SampleWriter::kOk);
    std::vector<int16_t> d(1000, 100);
    CHECK(rig.writer.write(0, reinterpret_cast<const uint8_t*>(d.data()), 2000) == pg::sampler::SampleWriter::kOk);
    CHECK(rig.writer.commit(pg::sampler::crc32(d.data(), 2000), "OLD") == pg::sampler::SampleWriter::kOk);
    CHECK(!rig.mic.slotErased(kSlot));
    CHECK(!rig.mic.start(kSlot));
    CHECK(rig.mic.erase(kSlot));
    CHECK(rig.mic.slotErased(kSlot));
    CHECK(rig.mic.start(kSlot));
  }
}

// The core's commit at the recording's rate (what LineMic::poll calls).
void testCoreCommit() {
  Rig rig;
  std::vector<int16_t> d(48000);
  for (size_t i = 0; i < d.size(); ++i) d[i] = int16_t(8000.0 * std::sin(double(i) * 0.0576));
  CHECK(rig.writer.beginRecording(kSlot) == pg::sampler::SampleWriter::kOk);
  CHECK(rig.writer.write(0, reinterpret_cast<const uint8_t*>(d.data()), uint32_t(d.size() * 2)) ==
        pg::sampler::SampleWriter::kOk);
  // a rate no header may hold: refused, the session still open, nothing written
  CHECK(rig.writer.commitRecording(uint32_t(d.size()), "X", 200000) == pg::sampler::SampleWriter::kBadSize);
  CHECK(rig.writer.recording() && !rig.pool.header(kSlot));
  CHECK(rig.writer.commitRecording(uint32_t(d.size()), "OURS", kRate) == pg::sampler::SampleWriter::kOk);
  const pg::sampler::SlotHeader* h = rig.pool.header(kSlot);
  CHECK(h && h->rate == kRate && h->samples == d.size());
  CHECK(!rig.writer.active());
  // refused: no session, a bad length
  CHECK(rig.writer.commitRecording(1000, "X", kRate) == pg::sampler::SampleWriter::kNoSession);
  CHECK(rig.writer.remove(kSlot) == pg::sampler::SampleWriter::kOk);
  CHECK(rig.writer.beginRecording(kSlot) == pg::sampler::SampleWriter::kOk);
  CHECK(rig.writer.commitRecording(1000, "X", kRate) == pg::sampler::SampleWriter::kBadSize);
  rig.writer.cancel();
}

}  // namespace

int main() {
  testMono();
  testStereoAndHum();
  testSilenceAndOverrun();
  testCoreCommit();
  return pgdt::finish("test_daisy_linein");
}
