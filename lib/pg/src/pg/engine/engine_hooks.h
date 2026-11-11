// Test / diagnostics hooks into the engine that are not part of the contract
// header (engine.h). The engine keeps ONE statically allocated state block, so
// these free functions act on "the" engine.
#pragma once
#include <cstddef>
#include <cstdint>

namespace pg {
namespace engine_detail {

// Seed for the probability RNG (xorshift32). Thread-safe: picked up at the
// start of the next quantum. Engine::init() reseeds with kDefaultSeed, so a
// freshly initialised engine is always reproducible. 0 is mapped to the default.
constexpr uint32_t kDefaultSeed = 0x9E3779B9u;
void setRngSeed(uint32_t seed);

// Bytes of static RAM the engine owns (its whole state: sequencer, queue,
// mixer, delay line, reverb pool, master).
size_t staticRamBytes();
size_t delayRamBytes();
size_t reverbRamBytes();

// Effective delay time in engine-rate samples after tempo sync and "halve
// until it fits"; and whether the send FX are currently being skipped.
int delayTimeSamples();
bool fxIdle();

// Number of times the engine had to defend itself against a non-finite or
// absurd signal (voice choked, FX state cleared). Stays 0 in a healthy system.
uint32_t faultCount();

// TEST ONLY: bypass the master bus's always-on 20 Hz rumble/DC high-pass so
// unit tests can probe gains with DC signals. Persists across init(); never
// call it in the firmware.
void setRumbleFilterBypass(bool bypass);

}  // namespace engine_detail
}  // namespace pg
