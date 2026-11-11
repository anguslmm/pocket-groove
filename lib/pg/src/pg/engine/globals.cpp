// Global and mixer parameter tables.
#include "../engine.h"
#include "scales.h"
#include "tables.h"

namespace pg {
namespace engine_detail {
namespace {

const char* const kDuckSourceNames[] = {"OFF", "KICK", "BEAT"};
const char* const kDelayTimeNames[kNumDelayDivisions] = {
    "1/32", "1/16T", "1/16", "1/8T", "1/16D", "1/8", "1/4T", "1/8D", "1/4"};
const char* const kRootNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
const char* const kScaleNames[kNumScales] = {
    kScales[0].name, kScales[1].name, kScales[2].name, kScales[3].name, kScales[4].name,
    kScales[5].name, kScales[6].name, kScales[7].name, kScales[8].name, kScales[9].name};

const ParamInfo kGlobalInfo[kGNumUsed] = {
    //  name    longName        min  max  def  format                 enum names
    {"BPM",  "Tempo",         60, 200, 124, ParamFormat::Bpm,     nullptr},           // kGTempo
    {"SWNG", "Swing",          0,  50,   0, ParamFormat::Swing,   nullptr},           // kGSwing: 0..50 -> 50..75 %
    {"VOL",  "Master Vol",     0, 127, 100, ParamFormat::Number,  nullptr},           // kGMasterVolume: 100 = unity
    {"DSRC", "Duck Source",    0,   2,   1, ParamFormat::Enum,    kDuckSourceNames},  // kGDuckSource
    {"DDEP", "Duck Depth",     0, 127, 100, ParamFormat::Percent, nullptr},           // kGDuckDepth: floor = 1 - v/127
    {"DHLD", "Duck Hold",      0, 127,  49, ParamFormat::Number,  nullptr},           // kGDuckHold: 200 ms * (v/127)^2
    {"DREL", "Duck Release",   0, 127,  78, ParamFormat::Number,  nullptr},           // kGDuckRelease: 20 ms * 50^(v/127)
    {"DCRV", "Duck Curve",     0, 127,  64, ParamFormat::Bipolar, nullptr},           // kGDuckCurve: log / lin / exp
    {"DFX",  "Duck FX",        0, 127,  64, ParamFormat::Percent, nullptr},           // kGDuckFx
    {"DTIM", "Delay Time",     0, kNumDelayDivisions - 1, 7, ParamFormat::Enum, kDelayTimeNames},  // kGDelayTime
    {"DFB",  "Delay Fdbk",     0, 127,  58, ParamFormat::Percent, nullptr},           // kGDelayFeedback: 0.95 * v/127 per echo
    {"DTON", "Delay Tone",     0, 127,  64, ParamFormat::Bipolar, nullptr},           // kGDelayTone: dark .. thin
    {"DWID", "Delay Width",    0, 127, 100, ParamFormat::Percent, nullptr},           // kGDelayWidth: mono .. ping-pong
    {"RSIZ", "Reverb Size",    0, 127,  60, ParamFormat::Number,  nullptr},           // kGReverbSize: RT60 0.3 .. 5 s
    {"RDMP", "Reverb Damp",    0, 127,  48, ParamFormat::Number,  nullptr},           // kGReverbDamp: 9 kHz .. 900 Hz
    {"FILT", "DJ Filter",      0, 127,  64, ParamFormat::DjFilter, nullptr},          // kGMasterFilter: 64 = OFF, < LP, > HP
    {"FRES", "Filter Reso",    0, 127,  36, ParamFormat::Number,  nullptr},           // kGMasterReso: Q 0.7 .. 6
    {"DRIV", "Drive",          0, 127,   0, ParamFormat::Number,  nullptr},           // kGMasterDrive
    {"ROOT", "Scale Root",     0,  11,   0, ParamFormat::Enum,    kRootNames},        // kGScaleRoot
    {"SCAL", "Scale",          0, kNumScales - 1, 2, ParamFormat::Enum, kScaleNames}, // kGScaleType (default minor)
};

// Per-track mixer defaults: level, pan, delay send, reverb send, duck amount.
//                                  KICK  SNR  CLAP   CH   OH  PERC BASS LEAD
// LEVEL: clap +3.6 dB, open hat +3 dB, lead +3.3 dB after the first full-instrument
// renders (integration): with the voices' own levels they sat 4-6 dB under the
// gain-staging plan in master.h (clap ~0.4, lead ~0.3 in a busy bar).
constexpr uint8_t kDefLevel[kNumTracks] = {104,  96, 106,  84,  94,  86,  98, 100};
constexpr uint8_t kDefPan[kNumTracks]   = { 64,  64,  64,  58,  70,  74,  64,  64};
constexpr uint8_t kDefDelay[kNumTracks] = {  0,   0,  12,   0,  10,  28,   0,  48};
constexpr uint8_t kDefRev[kNumTracks]   = {  0,  28,  44,  10,  24,  30,   0,  44};
constexpr uint8_t kDefDuck[kNumTracks]  = {  0,   0,  20,   0,  36,  36, 110,  64};

struct MixTable {
  ParamInfo info[kNumTracks][kNumMixParams];
};
constexpr ParamInfo mk(const char* n, const char* ln, uint8_t def, ParamFormat f) {
  return ParamInfo{n, ln, 0, 127, def, f, nullptr};
}
constexpr MixTable buildMixTable() {
  MixTable t{};
  for (int i = 0; i < kNumTracks; ++i) {
    t.info[i][0] = mk("LVL", "Level", kDefLevel[i], ParamFormat::Number);       // 100 = unity, (v/100)^3
    t.info[i][1] = mk("PAN", "Pan", kDefPan[i], ParamFormat::Bipolar);
    t.info[i][2] = mk("DLY", "Delay Send", kDefDelay[i], ParamFormat::Percent); // (v/127)^2
    t.info[i][3] = mk("REV", "Reverb Send", kDefRev[i], ParamFormat::Percent);  // (v/127)^2
    t.info[i][4] = mk("DUCK", "Duck Amount", kDefDuck[i], ParamFormat::Percent);
  }
  return t;
}
const MixTable kMixTable = buildMixTable();

}  // namespace

const float kDelayDivisionSteps[kNumDelayDivisions] = {
    0.5f, 2.0f / 3.0f, 1.0f, 4.0f / 3.0f, 1.5f, 2.0f, 8.0f / 3.0f, 3.0f, 4.0f};

const ParamInfo* mixParamInfo(int track, int index) {
  if (track < 0 || track >= kNumTracks) return nullptr;
  const int i = index - kMixLevel;
  if (i < 0 || i >= kNumMixParams) return nullptr;
  return &kMixTable.info[track][i];
}

}  // namespace engine_detail

const ParamInfo* Engine::globalParamInfo(int id) {
  if (id < 0 || id >= kGNumUsed) return nullptr;
  return &engine_detail::kGlobalInfo[id];
}

}  // namespace pg
