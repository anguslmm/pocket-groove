#include "sampler_params.h"

namespace pg {
namespace {

bool ieq(const char* a, const char* b) {
  for (; *a && *b; ++a, ++b) {
    const char x = (*a >= 'A' && *a <= 'Z') ? char(*a - 'A' + 'a') : *a;
    const char y = (*b >= 'A' && *b <= 'Z') ? char(*b - 'A' + 'a') : *b;
    if (x != y) return false;
  }
  return *a == *b;
}

const char* const kFilterNames[] = {"LP", "BP", "HP"};
const char* const kRetrigNames[] = {"OFF", "1/2", "1/4", "1/8", "1/8T", "1/16", "1/16T", "1/32"};

constexpr ParamInfo kTable[sampler::kNumParams] = {
    {"SLCS", "Slices", 1, 16, 1, ParamFormat::Number, nullptr},
    {"SLCE", "Slice", 0, 15, 0, ParamFormat::Number, nullptr},
    {"STRT", "Start", 0, 127, 0, ParamFormat::Percent, nullptr},
    {"LEN", "Length", 0, 127, 127, ParamFormat::Percent, nullptr},
    {"PTCH", "Pitch", 16, 112, 64, ParamFormat::Semitones, nullptr},
    {"FINE", "Fine", 0, 127, 64, ParamFormat::Bipolar, nullptr},
    {"SPED", "Speed", 0, 127, 96, ParamFormat::Number, nullptr},
    {"RVRS", "Reverse", 0, 1, 0, ParamFormat::OnOff, nullptr},
    {"ATK", "Attack", 0, 127, 0, ParamFormat::Number, nullptr},
    {"REL", "Release", 0, 127, 24, ParamFormat::Number, nullptr},
    {"CUT", "Cutoff", 0, 127, 127, ParamFormat::Number, nullptr},
    {"RES", "Resonance", 0, 127, 0, ParamFormat::Number, nullptr},
    {"FILT", "Filter", 0, sampler::kNumFilters - 1, 0, ParamFormat::Enum, kFilterNames},
    {"BITS", "Bits", 0, 127, 0, ParamFormat::Number, nullptr},
    {"DOWN", "Downsample", 0, 127, 0, ParamFormat::Number, nullptr},
    {"DRV", "Drive", 0, 127, 0, ParamFormat::Number, nullptr},
    {"RTRG", "Retrigger", 0, sampler::kNumRetrig - 1, 0, ParamFormat::Enum, kRetrigNames},
    {"RPIT", "Repeat pitch", 0, 127, 64, ParamFormat::Bipolar, nullptr},
    {"RFAD", "Repeat fade", 0, 127, 0, ParamFormat::Number, nullptr},
    {"LOOP", "Loop", 0, 1, 0, ParamFormat::OnOff, nullptr},
};
static_assert(sampler::kNumParams <= kSoundParams, "the sampler has at most 24 sound parameters");

}  // namespace

const char* voiceKindName(int kind) {
  switch (kind) {
    case kVoiceNative: return "native";
    case kVoiceSampler: return "sampler";
    default: return nullptr;
  }
}

int voiceKindFromName(const char* name) {
  for (int k = 0; k < kNumVoiceKinds; ++k)
    if (ieq(name, voiceKindName(k))) return k;
  return -1;
}

bool voiceKindOnTrack(int kind, int track) {
  if (track < 0 || track >= kNumTracks) return false;
  if (kind == kVoiceNative) return true;
  if (kind == kVoiceSampler) return ((kSamplerTracks >> track) & 1u) != 0;
  return false;
}

const ParamInfo* voiceKindParam(int kind, int index) {
  if (kind == kVoiceSampler) return sampler::paramInfo(index);
  return nullptr;
}

namespace sampler {
const ParamInfo* paramInfo(int index) { return index >= 0 && index < kNumParams ? &kTable[index] : nullptr; }
}  // namespace sampler

}  // namespace pg
