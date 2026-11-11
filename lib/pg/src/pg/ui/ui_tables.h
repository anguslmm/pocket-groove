// Tables shared by ui.cpp and ui_draw.cpp (private to the ui component).
#pragma once
#include <cstdint>
#include "../params.h"
#include "ui.h"

namespace pg {
namespace ui {

inline constexpr uint8_t kVelRow[8] = {16, 32, 48, 64, 80, 96, 112, 127};   // drum note row = velocity
inline constexpr uint8_t kMixRowParam[kNumMixRows] = {kMixLevel, kMixDuck, kMixDelaySend, kMixReverbSend, kMixPan};
inline constexpr const char* kMixRowNames[kNumMixRows] = {"LEVEL", "DUCK", "DELAY", "REVERB", "PAN"};
inline constexpr uint8_t kRollCode[kNumRollRates] = {1, 2, 3, 4};   // engine.h Roll c: 1/8 1/16 1/16T 1/32
inline constexpr const char* kRollNames[kNumRollRates] = {"1/8", "1/16", "1/16T", "1/32"};
// FX view pages: global ids in column order (z x c v b n m ,); 0xFF = no column.
inline constexpr uint8_t kFxPage[kNumFxTabs][8] = {
    {kGDuckSource, kGDuckDepth, kGDuckHold, kGDuckRelease, kGDuckCurve, kGDuckFx, 0xFF, 0xFF},
    {kGDelayTime, kGDelayFeedback, kGDelayTone, kGReverbSize, kGReverbDamp, kGDelayWidth, 0xFF, 0xFF},
    {kGTempo, kGSwing, kGMasterVolume, kGMasterFilter, kGMasterReso, kGMasterDrive, kGScaleRoot, kGScaleType},
};
inline constexpr const char* kFxTabNames[kNumFxTabs] = {"DUCK", "SPACE", "MASTER"};
inline constexpr const char* kTrackNames[8] = {"KICK", "SNAR", "CLAP", "CHH", "OHH", "PERC", "BASS", "LEAD"};
inline constexpr char kPadKeys[] = "ASDFGHJK";
inline constexpr char kNoteKeys[] = "ZXCVBNM,";
inline constexpr char kStepKeys[] = "12345678QWERTYUI";

inline int fxTabOfGlobal(int g) {
  for (int t = 0; t < kNumFxTabs; ++t)
    for (int c = 0; c < 8; ++c)
      if (kFxPage[t][c] == g) return t;
  return kFxMaster;
}

}  // namespace ui
}  // namespace pg
