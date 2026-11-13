// The instrument page's picture: see scope.h.
#include "scope.h"

#include <cmath>
#include <cstring>

namespace pg {
namespace ui {

namespace {

constexpr int kDrumPts = 176;           // ring samples per drum window (<= kScopeReadMax)
constexpr uint32_t kSynthSettleMs = 100; // no new samples for this long: the voice has stopped
constexpr uint32_t kDrumHoldMs = 700;    // a drum's last hit stays this long after its voice stopped

inline float absf(float v) { return v < 0.0f ? -v : v; }
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Linear interpolation of x at fractional index p (clamped to the data).
inline float at(const float* x, int n, float p) {
  if (p <= 0.0f) return x[0];
  const int i = int(p);
  if (i >= n - 1) return x[n - 1];
  const float f = p - float(i);
  return x[i] + (x[i + 1] - x[i]) * f;
}

// Moves the gain toward `peak` sitting at 0.85 of the box: down at once, up slowly
// (the first picture of a track jumps straight there).
float nextGain(float gain, float peak, bool* set) {
  if (!(peak > 1.0e-5f)) return gain;
  const float target = clampf(0.85f / peak, 0.05f, Scope::kMaxGain);
  if (!*set) {
    *set = true;
    return target;
  }
  if (target < gain) return target;
  const float up = gain * 1.06f;   // ~0.5 dB per frame, ~15 dB/s at 30 fps
  return up < target ? up : target;
}

}  // namespace

int Scope::drumWindowMs(int track) {
  switch (track) {
    case kTrackKick: return 340;
    case kTrackSnare: return 260;
    case kTrackClap: return 260;
    case kTrackHatClosed: return 70;
    case kTrackHatOpen: return 420;
    default: return 300;   // perc
  }
}

int Scope::pickDecimation(float samples, float cycles, int cur) {
  // the window and the search for its start (1.1 periods before it) must fit what one read copies
  const float most = float(kScopeReadMax - 2) / (1.0f + 1.1f / cycles);
  if (cur > 0) {
    const float pts = samples / float(cur);
    if (pts >= most * 0.25f && pts <= most) return cur;
  }
  int d = 1;
  while (d < 128 && samples / float(d) > most) d *= 2;
  return d;
}

void Scope::reset() {
  std::memset(y_, 0, sizeof(y_));
  filled_ = 0;
  havePrev_ = false;
  decim_ = 0;
  lastEnd_ = 0;
  lastNewMs_ = 0;
  hits_ = 0;
  hitMark_ = 0;
  haveHit_ = false;
  gain_ = 1.0f;
  gainSet_ = false;
  refPeak_ = 0.0f;
  fading_ = false;
  live_ = 0.0f;
  hitPeak_ = 0.0f;
  lastStart_ = 0.0f;
}

void Scope::stop(Engine& eng) {
  if (track_ >= 0) eng.setScope(-1, 1);
  track_ = -1;
  reset();
}

bool Scope::commit(const int8_t* next, int filled) {
  const bool same = filled == filled_ && std::memcmp(next, y_, kCols) == 0;
  if (!same) {
    std::memcpy(y_, next, kCols);
    filled_ = int16_t(filled);
  }
  return !same;
}

void Scope::resample(const float* x, int n, float start, float len, int cols, float g, int8_t* out) const {
  const float stepPx = len / float(kCols - 1);
  for (int c = 0; c < cols; ++c) {
    float v = clampf(at(x, n, start + stepPx * float(c)) * g, -1.0f, 1.0f);
    if (drum_) v = v < 0.0f ? -sqrtf(-v) : sqrtf(v);   // drums: lift the decay so the tail reads
    out[c] = int8_t(lrintf(v * 127.0f));
  }
  for (int c = cols; c < kCols; ++c) out[c] = 0;
}

float Scope::match(const float* x, int n, float start, float len) const {
  // Every 4th column against the previous picture, at the current gain.
  const float stepPx = len / float(kCols - 1);
  float e = 0.0f;
  for (int c = 0; c < kCols; c += 4) {
    const float v = clampf(at(x, n, start + stepPx * float(c)) * gain_, -1.0f, 1.0f) - float(y_[c]) * (1.0f / 127.0f);
    e += v * v;
  }
  return e;
}

bool Scope::updateSynth(const float* x, int n, const ScopeInfo& in) {
  // The window: kCycles periods of the note last started on the track.
  const int note = in.note ? in.note : (track_ == kTrackBass ? 36 : 60);
  const float f = 440.0f * exp2f(float(note - 69) * (1.0f / 12.0f));
  const float cycles = track_ == kTrackBass ? kCyclesBass : kCyclesLead;
  const float len = cycles * in.sampleRate / f / float(in.decimation);   // window, in ring samples
  const float period = len / cycles;
  // A note started within the last window: draw from its start, the attack at the left edge.
  const int32_t off = int32_t(in.mark) - (int32_t(in.end) - n);   // x index of the note's first sample
  if (in.hits && off >= 0 && off < n && float(n - off) < len) {
    float pk = 0.0f;
    for (int i = off; i < n; ++i) pk = absf(x[i]) > pk ? absf(x[i]) : pk;
    gain_ = nextGain(gain_, pk, &gainSet_);
    refPeak_ = pk > refPeak_ ? pk : refPeak_;
    fading_ = false;
    const int cols = int(float(n - off) / len * float(kCols));
    int8_t next[kCols];
    resample(x + off, n - off, 0.0f, len, cols, gain_, next);
    havePrev_ = false;   // the next whole window takes the latest crossing
      return commit(next, cols);
  }
  if (float(n) < len + 2.0f) return false;   // not enough yet (just after a change of decimation)
  const float hi = float(n - 1) - len;        // the latest start with a whole window
  float lo = hi - period * 1.1f;
  if (lo < 1.0f) lo = 1.0f;
  // Rising crossings of the middle level (halfway between the lowest and the highest
  // sample, so a lopsided or offset wave still has one per period) in [lo, hi],
  // with a small hysteresis: the signal must have been clearly below it shortly before.
  float mx = x[int(lo)], mn = x[int(lo)];
  for (int i = int(lo); i < n; ++i) {
    mx = x[i] > mx ? x[i] : mx;
    mn = x[i] < mn ? x[i] : mn;
  }
  const float mid = 0.5f * (mx + mn), hyst = 0.05f * (mx - mn);
  float best = hi, bestScore = 0.0f;
  bool found = false;
  for (int i = int(lo) + 1; i <= int(hi) + 1 && i < n; ++i) {
    if (!(x[i - 1] < mid && x[i] >= mid)) continue;
    bool dipped = false;
    for (int j = i - 1; j >= 0 && float(i - j) <= period * 0.5f; --j)
      if (x[j] < mid - hyst) { dipped = true; break; }
    if (!dipped) continue;
    const float s = float(i - 1) + (mid - x[i - 1]) / (x[i] - x[i - 1]);
    if (s < lo || s > hi) continue;
    if (!havePrev_) {   // nothing to match: the latest crossing
      best = s;
      found = true;
      continue;
    }
    // of the crossings, the one whose picture is closest to the last frame's
    const float score = match(x, n, s, len);
    if (!found || score <= bestScore) {
      best = s;
      bestScore = score;
      found = true;
    }
  }
  if (!found && havePrev_) {
    // no clean crossing (noise, a chord): the offset that best matches the last frame
    for (float s = lo; s <= hi; s += 1.0f) {
      const float score = match(x, n, s, len);
      if (!found || score <= bestScore) {
        best = s;
        bestScore = score;
        found = true;
      }
    }
  }
  float wpeak = 0.0f;
  const int a = int(best), b = int(best + len) + 1;
  for (int i = a; i <= b && i < n; ++i) wpeak = absf(x[i]) > wpeak ? absf(x[i]) : wpeak;
  // The note is ending (its window fell far below the recent level): keep the last
  // picture and let it fade, instead of blowing the release tail up to full size.
  refPeak_ = refPeak_ * 0.97f > wpeak ? refPeak_ * 0.97f : wpeak;
  fading_ = wpeak < 0.12f * refPeak_;
  if (fading_) return false;
  gain_ = nextGain(gain_, wpeak, &gainSet_);
  lastStart_ = best;
  int8_t next[kCols];
  resample(x, n, best, len, kCols, gain_, next);
  havePrev_ = true;
  return commit(next, kCols);
}

bool Scope::updateDrum(const float* x, int n, const ScopeInfo& in) {
  if (in.hits != hits_) {   // a new hit: start a new picture from its note-on
    hits_ = in.hits;
    if (in.hits) {
      hitMark_ = in.mark;
      haveHit_ = true;
      hitPeak_ = 0.0f;
      std::memset(y_, 0, sizeof(y_));
      filled_ = 0;
    }
  }
  if (!haveHit_ || n <= 0) return false;
  const int32_t since = int32_t(in.end - hitMark_);   // samples of this hit written so far
  const int32_t first = int32_t(in.end) - n;          // ring index of x[0]
  const int32_t off = int32_t(hitMark_) - first;      // x index of the hit's first sample
  if (since <= 0 || off < 0) return false;            // nothing yet, or the start is gone (keep what we have)
  const int m = since < kDrumPts ? int(since) : kDrumPts;
  const int prev = filled_;
  const int cols = m >= kDrumPts ? kCols : int(float(m) / float(kDrumPts) * float(kCols));
  if (cols <= prev && prev > 0) return false;
  float pk = hitPeak_;
  for (int i = 0; i < m && off + i < n; ++i) pk = absf(x[off + i]) > pk ? absf(x[off + i]) : pk;
  hitPeak_ = pk;
  gain_ = nextGain(gain_, pk, &gainSet_);
  int8_t next[kCols];
  resample(x + off, n - off, 0.0f, float(kDrumPts - 1), cols, gain_, next);
  return commit(next, cols);
}

bool Scope::update(Engine& eng, int track, uint32_t nowMs) {
  if (track < 0 || track >= kNumTracks) {
    stop(eng);
    return false;
  }
  bool changed = false;
  if (track != track_) {
    reset();
    track_ = int8_t(track);
    drum_ = track < kNumDrumTracks;
    lastNewMs_ = nowMs;
    changed = true;
  }
  float x[kScopeReadMax];
  ScopeInfo in;
  int n = eng.readScope(x, kScopeReadMax, &in);
  for (int i = 0; i < n; ++i)
    if (!(absf(x[i]) < 64.0f)) x[i] = 0.0f;   // never draw a NaN or a blow-up
  // What to ask the engine for.
  const float sr = in.sampleRate > 1000.0f ? in.sampleRate : kDefaultSampleRate;
  int d;
  if (drum_) {
    d = int(float(drumWindowMs(track)) * 0.001f * sr / float(kDrumPts) + 0.5f);
  } else {
    const int note = (in.track == track && in.note) ? in.note : (track == kTrackBass ? 36 : 60);
    const float f = 440.0f * exp2f(float(note - 69) * (1.0f / 12.0f));
    const float cycles = track == kTrackBass ? kCyclesBass : kCyclesLead;
    d = pickDecimation(cycles * sr / f, cycles, decim_);
  }
  d = d < 1 ? 1 : (d > 255 ? 255 : d);
  if (d != decim_ || in.track != track) {
    if (d != decim_) havePrev_ = false;   // a different time scale: nothing to match
    decim_ = uint8_t(d);
    eng.setScope(track, d);
  }
  // The samples (and the note marks) are ours only once the engine taps what we asked for.
  const bool ours = in.track == track && in.decimation == decim_;
  if (!ours) n = 0;
  in.sampleRate = sr;
  const bool fresh = n > 0 && in.end != lastEnd_;
  if (fresh) {
    lastEnd_ = in.end;
    lastNewMs_ = nowMs;
  }
  const float live0 = live_;
  if (drum_) {
    if (ours) changed |= updateDrum(x, n, in);
    // the last hit holds while the voice sounds and for a while after; then it fades
    if (filled_ > 0 && (nowMs - lastNewMs_) < kDrumHoldMs) live_ = 1.0f;
    else live_ *= 0.8f;
  } else {
    if (fresh) changed |= updateSynth(x, n, in);
    if (fresh && !fading_) live_ = live_ < 0.5f ? 0.5f + live_ : 1.0f;
    else if (fading_) live_ *= 0.85f;   // a note's release: the last picture lingers a moment
    else if (nowMs - lastNewMs_ >= kSynthSettleMs) live_ *= 0.75f;
  }
  if (live_ > 1.0f) live_ = 1.0f;
  if (live_ < 0.02f) live_ = 0.0f;
  if (int(live_ * 32.0f) != int(live0 * 32.0f)) changed = true;
  return changed;
}

}  // namespace ui
}  // namespace pg
