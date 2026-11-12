// Pocket Groove hardware spike -- the self-diagnosing microphone matrix
// (SAMPLER page, key m; serial M). docs/hw-spike.md section 4b.
//
// One run (about 30 s, no human timing needed) tries every candidate setup of
// the ES8311 ADC and the I2S input, each for 0.5 s with a 1 kHz tone on the
// speaker as the stimulus, plus an electrical test of the DIN pin (GPIO46),
// two M5.Mic control runs and a full register dump, then ranks the setups and,
// if one works, leaves the sampler running it. The result prints as the
// "===== PG-HWSPIKE MICMATRIX" block.
//
// Threads: start() / tick() / report() on the UI thread; the run itself in a
// job task on core 1 (priority 5); process() in the audio task.
#pragma once
#include <cstdint>

namespace hws {
namespace micmatrix {

bool start();          // UI: false if a run is already going
bool running();
bool finished();       // a result exists (from this boot)
bool works();          // the last run found a working setup
const char* statusLine();   // short text for the LCD (progress, or the verdict)

// Audio task: while running() the matrix owns the output (test tone) and
// reads the input; the page's render() is not called.
void process(const int16_t* in, int16_t* out, int frames);

// UI, every loop: prints the report when a run ends. Returns true once after
// a run that switched the sampler to a working setup; *slot = I2S slot (0 L,
// 1 R) to record from.
bool tick(int* slot);
void report();         // prints the last block again (or "no run yet")

}  // namespace micmatrix
}  // namespace hws
