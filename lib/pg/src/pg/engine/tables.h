// Engine-internal parameter tables (defined in globals.cpp).
#pragma once
#include "../params.h"

namespace pg {
namespace engine_detail {

constexpr int kNumMixParams = 5;            // kMixLevel .. kMixDuck; 29..31 are reserved (no ParamInfo)
// Mixer ParamInfo for `track`, index 24..31; nullptr for the reserved slots.
// Names/ranges are the same for every track, defaults differ per track.
const ParamInfo* mixParamInfo(int track, int index);

constexpr int kNumDelayDivisions = 9;
// Delay time of each kGDelayTime enum value, in 16th-note steps.
extern const float kDelayDivisionSteps[kNumDelayDivisions];

enum DuckSource : uint8_t { kDuckOff = 0, kDuckKick = 1, kDuckBeat = 2 };

}  // namespace engine_detail
}  // namespace pg
