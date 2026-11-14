// See pads.h.
//
// Settings hand-over: the main loop writes want_ / wantG_ and then bumps wantSeq_;
// scan() (the audio interrupt, which preempts the main loop but is never preempted
// by it) copies them when the sequence moved. A change made while an interrupt
// copies would need the main loop to be half-way through a second change at that
// moment: at worst one scan uses a mix of the two, the next scan the whole second.
#include "pads.h"

#include <cmath>

namespace pgd {

namespace {
float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
}  // namespace

float PadScanner::conductance(float x) {
  x = clampf(x, 0.0f, 0.9995f);
  return kDividerUs * x / (1.0f - x);
}

float PadScanner::readingFor(float uS) {
  if (uS <= 0.0f) return 0.0f;
  return uS / (uS + kDividerUs);
}

PadCal PadScanner::sanitize(const PadCal& c) {
  PadCal o = c;
  if (!(o.threshold == o.threshold)) o.threshold = 10.0f;   // NaN
  if (!(o.ceiling == o.ceiling)) o.ceiling = PadCal{}.ceiling;
  if (!(o.curve == o.curve)) o.curve = PadCal{}.curve;
  o.threshold = clampf(o.threshold, 0.5f, 5000.0f);
  o.ceiling = clampf(o.ceiling, o.threshold * 1.5f, 200000.0f);
  o.curve = clampf(o.curve, 0.25f, 4.0f);
  return o;
}

PadGlobal PadScanner::sanitize(const PadGlobal& g) {
  PadGlobal o = g;
  if (o.windowScans < 1) o.windowScans = 1;
  if (o.windowScans > 4) o.windowScans = 4;
  if (o.releaseScans < 1) o.releaseScans = 1;
  if (o.releaseScans > 4) o.releaseScans = 4;
  if (o.xtalkScans > 20) o.xtalkScans = 20;
  if (!(o.xtalkRatio == o.xtalkRatio)) o.xtalkRatio = 0.4f;
  if (!(o.releaseRatio == o.releaseRatio)) o.releaseRatio = 0.6f;
  o.xtalkRatio = clampf(o.xtalkRatio, 0.0f, 1.0f);
  o.releaseRatio = clampf(o.releaseRatio, 0.1f, 0.95f);
  return o;
}

void PadScanner::setCal(int pad, const PadCal& c) {
  if (pad < 0 || pad >= kPads) return;
  want_[pad] = sanitize(c);
  wantSeq_.fetch_add(1, std::memory_order_release);
}

void PadScanner::setGlobal(const PadGlobal& g) {
  wantG_ = sanitize(g);
  wantSeq_.fetch_add(1, std::memory_order_release);
}

void PadScanner::takeOver() {
  const uint32_t s = wantSeq_.load(std::memory_order_acquire);
  if (s == usedSeq_) return;
  usedSeq_ = s;
  for (int i = 0; i < kPads; ++i) {
    cal_[i] = want_[i];
    if (!cal_[i].on) pad_[i].state = State::Idle;
  }
  g_ = wantG_;
}

void PadScanner::reset() {
  for (Pad& p : pad_) p = Pad{};
  head_.store(0, std::memory_order_relaxed);
  tail_.store(0, std::memory_order_relaxed);
  scan_ = 0;
  dropped_ = 0;
  usedSeq_ = 0;
}

const char* PadScanner::stateName(State s) {
  switch (s) {
    case State::Idle: return "idle";
    case State::Onset: return "onset";
    case State::Held: return "HELD";
    case State::Suppressed: return "ghost";
  }
  return "?";
}

float PadScanner::pressure(int pad) const {
  const PadCal& c = cal_[pad];
  return clampf((pad_[pad].g - c.threshold) / (c.ceiling - c.threshold), 0.0f, 1.0f);
}

void PadScanner::clearPeaks() {
  for (Pad& p : pad_) p.maxG = 0.0f;
}

bool PadScanner::push(uint8_t type, int pad, float value) {
  const uint32_t h = head_.load(std::memory_order_relaxed);
  const uint32_t t = tail_.load(std::memory_order_acquire);
  const uint32_t room = uint32_t(kRing) - (h - t);
  if (room == 0 || (type == PadEvent::Pressure && room <= uint32_t(kReserve))) {
    ++dropped_;
    return false;
  }
  ring_[h & (kRing - 1)] = PadEvent{type, uint8_t(pad), value, scan_};
  head_.store(h + 1, std::memory_order_release);
  return true;
}

bool PadScanner::pop(PadEvent* e) {
  const uint32_t t = tail_.load(std::memory_order_relaxed);
  const uint32_t h = head_.load(std::memory_order_acquire);
  if (t == h) return false;
  *e = ring_[t & (kRing - 1)];
  tail_.store(t + 1, std::memory_order_release);
  return true;
}

// The velocity before the curve: the fastest rise in the window, a rise of the
// whole span within the window being 1.
float PadScanner::linear(const Pad& p, const PadCal& c) const {
  const float span = c.ceiling - c.threshold;
  return clampf(p.maxRise * float(g_.windowScans) / span, 0.0f, 1.0f);
}

// Is pad j's hit (lin) a ghost of a harder hit next door?
bool PadScanner::ghost(int j, float lin) const {
  if (g_.xtalkScans == 0 || g_.xtalkRatio <= 0.0f) return false;
  for (int i = 0; i < kPads; ++i) {
    if (i == j) continue;
    const Pad& o = pad_[i];
    if (o.state != State::Onset && o.state != State::Held) continue;
    const uint32_t d = o.onset > pad_[j].onset ? o.onset - pad_[j].onset : pad_[j].onset - o.onset;
    if (d > g_.xtalkScans) continue;
    if (lin < g_.xtalkRatio * o.estimate) return true;
  }
  return false;
}

void PadScanner::scan(const float* x) {
  takeOver();
  ++scan_;
  // 1. every pad's reading and state, the windows that end now collected
  int due[kPads];
  int nDue = 0;
  for (int i = 0; i < kPads; ++i) {
    Pad& p = pad_[i];
    const PadCal& c = cal_[i];
    p.x = x[i];
    p.g = conductance(x[i]);
    if (p.g > p.maxG) p.maxG = p.g;
    if (!c.on) continue;
    const float g = p.g;
    const float off = c.threshold * g_.releaseRatio;
    switch (p.state) {
      case State::Idle:
        if (g >= c.threshold) {
          p.state = State::Onset;
          p.onset = scan_;
          p.seen = 1;
          p.maxRise = g - c.threshold;
          p.prev = g;
          p.estimate = linear(p, c);
          if (p.seen >= g_.windowScans || g >= c.ceiling) due[nDue++] = i;
        }
        break;
      case State::Onset: {
        if (g < off) {   // gone again inside the window: a blip, no note
          p.state = State::Idle;
          ++p.blips;
          break;
        }
        const float rise = g - p.prev;
        if (rise > p.maxRise) p.maxRise = rise;
        p.prev = g;
        ++p.seen;
        p.estimate = linear(p, c);
        if (p.seen >= g_.windowScans || g >= c.ceiling) due[nDue++] = i;
        break;
      }
      case State::Held:
      case State::Suppressed:
        if (g < off) {
          if (++p.below >= g_.releaseScans) {
            if (p.state == State::Held) push(PadEvent::Up, i, 0.0f);
            p.state = State::Idle;
            p.below = 0;
          }
          break;
        }
        p.below = 0;
        if (p.state == State::Held) {
          const float pr = clampf((g - c.threshold) / (c.ceiling - c.threshold), 0.0f, 1.0f);
          if (std::fabs(pr - p.sentP) >= kPressureStep && push(PadEvent::Pressure, i, pr)) p.sentP = pr;
        }
        break;
    }
  }
  // 2. the windows that end: notes, unless a harder hit next door makes them ghosts
  //    (decided after every pad has this scan's reading, so two pads that cross in
  //    the same scan see each other)
  for (int k = 0; k < nDue; ++k) {
    const int i = due[k];
    Pad& p = pad_[i];
    const float lin = p.estimate;
    if (ghost(i, lin)) {
      p.state = State::Suppressed;
      p.below = 0;
      ++p.ghosts;
      continue;
    }
    float v = cal_[i].curve == 1.0f ? lin : powf(lin, cal_[i].curve);
    if (v < 1.0f / 127.0f) v = 1.0f / 127.0f;
    p.state = State::Held;
    p.below = 0;
    p.lastVel = v;
    p.lastLin = lin;
    p.sentP = -1.0f;
    ++p.hits;
    push(PadEvent::Down, i, v);
  }
}

}  // namespace pgd
