// The instrument page's picture (docs/UX.md 10.3): an oscilloscope trace of the
// selected track's own voice, read from the engine's scope tap (engine.h).
//
// Once per UI frame, Scope::update() tells the engine which track to tap and
// how much to decimate, copies the newest samples and turns them into kCols
// trace values (one per screen column), so the drawing code only plots them.
//   * Synth tracks (bass, lead): the time window is chosen from the note the
//     engine last started on the track, so about kCycles periods fill the
//     width in every register (decimation is a power of two that changes only
//     when the window would leave a comfortable range). The window starts at a
//     rising zero crossing; of the crossings within one period, the one whose
//     picture best matches the previous frame wins, so a steady tone stands
//     still and a changing one moves smoothly.
//   * Drum tracks: the window starts at the hit (the engine marks the note-on
//     in the ring) and covers a fixed time per track, so the trace shows the
//     hit's shape and decay; it grows from the left as the hit plays and holds
//     until the next one.
//     A note that started within the last window is drawn from its start
//     (the attack at the left edge), growing like a drum hit.
//   * Gain: the trace is scaled so its peak sits near the top of the box. It
//     falls at once (never clips) and rises slowly (about 15 dB/s), so quiet
//     instruments (the hats) still show a shape and a decaying note still
//     decays on screen. Never more than kMaxGain.
//   * Silence: once no new samples come (the voice has stopped), or a synth's
//     window falls far below its recent level (the note's release), the last
//     picture fades to the flat centre line within about a quarter of a second
//     (drums: their last hit is held for a while first).
// Plain data, no heap; about 0.5 KB. UI thread only.
#pragma once
#include <cstdint>

#include "../engine.h"

namespace pg {
namespace ui {

class Scope {
 public:
  static constexpr int kCols = 232;        // trace values, one per screen column
  static constexpr float kMaxGain = 400.0f; // +52 dB at most: the closed hat, averaged for its window, peaks near -45 dBFS

  // Starts tapping `track` (or with track < 0 stops the tap) and refreshes the
  // trace from what the engine wrote. Returns true when the picture changed.
  bool update(Engine& eng, int track, uint32_t nowMs);
  void stop(Engine& eng);                  // the page is not shown: the engine taps nothing
  void reset();                            // forget the picture (another track, a new rig)

  // The picture: y[c] in -1..1 (already scaled: 1 = the box's half height),
  // valid for columns c < filled (drums fill from the left; synths fill all).
  const int8_t* trace() const { return y_; }    // -127..127
  int filled() const { return filled_; }
  float level() const { return live_; }    // 1 while the voice sounds, falling to 0 in silence
  bool drum() const { return drum_; }
  int track() const { return track_; }

  // Tuning, public for the tests.
  static constexpr float kCyclesBass = 2.0f, kCyclesLead = 3.0f;
  static int drumWindowMs(int track);
  // The decimation for a window of `samples` input samples showing `cycles`
  // periods: a power of two that leaves the most points the ring can give (the
  // window plus 1.1 periods to search for its start); `cur` is kept while it
  // still fits and gives at least a quarter of that (a bass line within an
  // octave or so keeps one time scale).
  static int pickDecimation(float samples, float cycles, int cur);

 private:
  bool updateSynth(const float* x, int n, const ScopeInfo& in);
  bool updateDrum(const float* x, int n, const ScopeInfo& in);
  float match(const float* x, int n, float start, float len) const;   // SSD against y_
  void resample(const float* x, int n, float start, float len, int cols, float g, int8_t* out) const;
  bool commit(const int8_t* next, int filled);

  int8_t y_[kCols] = {};
  int16_t filled_ = 0;
  int8_t track_ = -1;
  bool drum_ = false;
  bool havePrev_ = false;        // y_ holds a synth window to match against
  uint8_t decim_ = 0;            // what we asked the engine for
  uint32_t lastEnd_ = 0;         // ScopeInfo::end at the last frame that saw new samples
  uint32_t lastNewMs_ = 0;
  uint32_t hits_ = 0, hitMark_ = 0;
  bool haveHit_ = false;
  float gain_ = 1.0f;
  bool gainSet_ = false;         // gain_ has been set from a picture of this track
  float refPeak_ = 0.0f;         // synths: the recent window peak (falls ~3 % a frame)
  bool fading_ = false;          // synths: the note is ending, the last picture fades
  float live_ = 0.0f;            // amplitude factor applied to the drawn trace
  float hitPeak_ = 0.0f;         // drums: the peak of the current hit so far
  float lastStart_ = 0.0f;
};

}  // namespace ui
}  // namespace pg
