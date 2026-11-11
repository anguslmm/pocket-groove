// Smoke test: synthetic DSP kernels that run INSIDE the audio callback, plus
// raw primitive costs measured with the CPU cycle counter.
//
// Kernels are written the way the instrument will be: float only, per-voice
// loops over a block, no heap, no libm in the inner loop unless that is what
// is being measured. Results go to a volatile sink so nothing is optimised away.
// Portable: no hardware includes (the cycle counter is passed in).
#pragma once
#include <cstdint>

namespace smoke {
namespace bench {

enum Kind : uint8_t {
  kOff = 0,
  kSaw,        // PolyBLEP sawtooth oscillator
  kSvf,        // TPT (Zavalishin) state-variable filter, lowpass out
  kSat,        // one-pole lowpass + rational tanh approximation (has a divide)
  kLadder,     // 4-pole nonlinear ladder, 2x oversampled, 5 tanh approx per step
  kFdn,        // 8-line feedback delay network reverb (Hadamard mix, damping)
  kDiv,        // float divide per sample
  kSinf,       // libm sinf per sample
  kExpf,       // libm expf per sample
  kTanhf,      // libm tanhf per sample
  kNumKinds
};

const char* name(Kind k);
int maxInstances(Kind k);
// N used by the automatic calibration run (keeps every kernel well below 100%).
int autoInstances(Kind k);

// Audio thread. Picks up a new (kind, n) between blocks; runs n instances over
// `frames` samples at `sampleRate`.
void configure(Kind k, int n, float sampleRate);   // UI thread: request
// Allocator for the FDN delay memory (~18 KB), called from configure() on
// first use. Without one, or if it returns nullptr, kFdn falls back to off.
void setAllocator(float* (*alloc)(unsigned bytes));
void process(int frames);                          // audio thread
Kind activeKind();
int  activeInstances();

// Raw primitive costs, cycles per operation (minimum over 7 runs of 2048 ops). UI thread; takes a few ms.
struct Primitives {
  float fadd;       // dependent float add (also the baseline for the composites)
  float fmadd;      // dependent float multiply-add
  float fdiv;       // dependent float divide
  float f2i16;      // float -> int16 with clamp
  float lerpLut;    // 256-entry float table, linear interpolation
  float sinf_;      // libm sinf
  float expf_;
  float tanhf_;
  float tanhRat;    // rational tanh approximation (as used by kSat / kLadder)
  float dmadd;      // DOUBLE multiply-add (to show why the engine is float-only)
  float ddiv;       // DOUBLE divide
  float sinD;       // libm sin() on double
};
using CycleFn = uint32_t (*)();
void measurePrimitives(CycleFn cycles, Primitives* out);

}  // namespace bench
}  // namespace smoke
