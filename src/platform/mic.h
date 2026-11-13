// Pocket Groove platform layer -- recording from the built-in microphone into the
// sample pool (stage F2; docs/STATE.md, docs/UX.md 10.3 "Recording").
//
// The device's pg::sampler::MicInput (the UI holds L on a sampler page):
//   * input: plat::audio's full-duplex RX (audio_out.h MICROPHONE): the audio task
//     copies each block's left slot into a 2048-sample ring (46 ms at 44.1 kHz);
//   * a writer task (core 1, priority 8, 3 KB stack) drains the ring into
//     pg::sampler::Recorder (half-band to 22,050 Hz, DC block, threshold start with
//     pre-roll), which writes 512 bytes at a time into the slot through
//     SampleWriter's recording path (flash writes of ~0.35 ms each, never an erase);
//   * the UI thread commits it in poll() (CRC through the driver vs the mapping, the
//     header with the playback gain) and frees everything.
// RAM: nothing static but the last recording's figures; the ring, the Recorder and
// the writer task's stack come from the heap for the recording and go after it
// (about 12 KB, the report prints it). The RX DMA ring (~2 KB) is plat::audio's.
//
// erase(): the slot's 68 sectors in one call with the output faded out first (both
// cores stall for the whole erase: the groove pauses; USB audio gets a gap).
//
// UI thread only. No Arduino / ESP types in this header.
#pragma once
#include <cstdint>

#include "pg/sampler/recorder.h"

namespace plat {
namespace mic {

constexpr uint32_t kRingSamples = 2048;      // 46 ms at 44.1 kHz
constexpr uint32_t kTaskStack = 3072;
constexpr int kTaskPrio = 8;                 // above the UI loop (1), below keys (10) and IMU (9)

pg::sampler::MicInput& input();
bool busy();                                 // a recording session exists (flash is being written)

// The report's [mic] section (serial `r`): codec registers, GPIO46, a one-second
// measurement of the input (when not recording: the RX runs for ~1.3 s, the UI loop
// waits), the last recording. `line` gets one line at a time.
void report(void (*line)(const char* text));

}  // namespace mic
}  // namespace plat
