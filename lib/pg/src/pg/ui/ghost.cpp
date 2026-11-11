// Phantom-key filter. See ghost.h and docs/ghosting-audit.md.
#include "ghost.h"

#include "keys.h"

namespace pg {
namespace ui {

namespace {

constexpr uint64_t bit(int k) { return 1ull << k; }
constexpr uint64_t kMatrix = (1ull << kNumKeys) - 1;

// Union-find over the 7 scan rows (0..6) and 8 scan columns (7..14).
struct Dsu {
  uint8_t p[kScanRows + kScanCols];
  Dsu() {
    for (int i = 0; i < kScanRows + kScanCols; ++i) p[i] = uint8_t(i);
  }
  int find(int x) {
    while (p[x] != x) x = p[x] = p[p[x]];
    return x;
  }
  void join(int a, int b) { p[find(a)] = uint8_t(find(b)); }
  void add(uint64_t keys) {
    keys &= kMatrix;
    while (keys) {
      const int k = __builtin_ctzll(keys);
      keys &= keys - 1;
      join(scanRow(k), kScanRows + scanCol(k));
    }
  }
  bool linked(int k) { return find(scanRow(k)) == find(kScanRows + scanCol(k)); }
};

// ---- known gestures with three keys down (docs/ghosting-audit.md) -------------
// A token is a key id (< kNumKeys) or a class. `late` = the token positions
// (bit 0..2) the most recent key may fill: a gesture is matched in the order it
// is played (holding a note, then ALT+pad, is not the same as ALT+pad, then a note).
enum : uint8_t { tStep = 200, tPad, tNote, tPlay, tVal, tUpDn };
struct Gesture { uint8_t t[3]; uint8_t late; };
constexpr uint8_t L0 = 1, L1 = 2, L2 = 4;
constexpr Gesture kGestures[] = {
    {{kKCtrl, kKShift, kKNote0 + 1}, L2},     // CTRL+SHIFT+X  cut pattern
    {{kKCtrl, kKShift, kKNote0 + 2}, L2},     // CTRL+SHIFT+C  copy pattern
    {{kKCtrl, kKShift, tStep}, L2},           // CTRL+SHIFT+step  track length
    {{kKOpt, kKShift, tStep}, L2},            // OPT+SHIFT+step  copy pattern there
    {{kKShift, kKLBracket, kKRBracket}, L1 | L2},   // SHIFT+[+]  bass cutoff back
    {{tStep, kKShift, tVal}, L1 | L2},        // step held + SHIFT+-/=  x8 (SHIFT may come last)
    {{tStep, kKCtrl, tUpDn}, L2},             // step held + CTRL+;/.  note +-12
    {{tStep, kKShift, kKL}, L2},              // step held + SHIFT+l  previous chord
    {{tStep, kKMinus, kKEquals}, L1 | L2},    // step held + - and =  default
    {{tStep, tPad, tVal}, L2},                // step held, field pad still down, -/=
    {{tStep, tNote, tVal}, L2},               // lock mode, column key still down, -/=
    {{kKTab, kKShift, tVal}, L1 | L2},        // TAB+SHIFT+-/=  tempo +-10
    {{kKTab, kKMinus, kKEquals}, L1 | L2},    // TAB + - and =  default tempo
    {{tStep, kKTab, tVal}, L2},               // step held + TAB+-/=  tempo
    {{tPlay, kKShift, tPlay}, L1 | L2},       // holding a pad/note, SHIFT+note accent / slide
    {{tPlay, kKFn, tPlay}, L2},               // holding a pad/note, FN+pad mute / FN+note solo
    {{tPlay, kKAlt, tPad}, L2},               // holding a pad/note (or rolling), ALT+pad roll
    {{kKAlt, tPad, tUpDn}, L2},               // rolling, ALT+;/.  roll rate
    {{tPlay, kKOpt, tStep}, L2},              // holding a pad/note, OPT+step  queue pattern
    {{tPlay, tPlay, tPlay}, L2},              // three pads / notes (drumming, legato runs)
};

bool tokenMatches(uint8_t t, int k) {
  switch (t) {
    case tStep: return stepKeyIndex(k) >= 0;
    case tPad: return padKeyIndex(k) >= 0;
    case tNote: return noteKeyIndex(k) >= 0;
    case tPlay: return padKeyIndex(k) >= 0 || noteKeyIndex(k) >= 0;
    case tVal: return k == kKMinus || k == kKEquals;
    case tUpDn: return k == kKSemicolon || k == kKDot;
    default: return t == k;
  }
}

// Two distinct keys of `held` match tokens a and b.
bool pairMatches(uint64_t held, uint8_t a, uint8_t b) {
  for (uint64_t m = held; m; m &= m - 1) {
    const int x = __builtin_ctzll(m);
    if (!tokenMatches(a, x)) continue;
    for (uint64_t n = held & ~bit(x); n; n &= n - 1)
      if (tokenMatches(b, __builtin_ctzll(n))) return true;
  }
  return false;
}

}  // namespace

uint64_t ghostClosure(uint64_t pressed) {
  Dsu d;
  d.add(pressed);
  uint64_t out = 0;
  for (int k = 0; k < kNumKeys; ++k)
    if (d.linked(k)) out |= bit(k);
  return out;
}

bool ghostCompletes(int k, uint64_t closed) {
  if (k < 0 || k >= kNumKeys) return false;
  Dsu d;
  d.add(closed & ~bit(k));
  return d.linked(k);
}

bool knownGesture(uint64_t held, int k) {
  held &= ~bit(k);
  for (const Gesture& g : kGestures)
    for (int i = 0; i < 3; ++i) {
      if (!((g.late >> i) & 1) || !tokenMatches(g.t[i], k)) continue;
      if (pairMatches(held, g.t[(i + 1) % 3], g.t[(i + 2) % 3])) return true;
    }
  return false;
}

// ---- GhostFilter ----------------------------------------------------------------

void GhostFilter::reset() { *this = GhostFilter(); }

uint64_t GhostFilter::queueMask() const {
  uint64_t m = 0;
  for (int i = 0; i < qn_; ++i) m |= bit(q_[i].ev.key);
  return m;
}

uint64_t GhostFilter::heldForMatch(int exceptA, int exceptB) const {
  uint64_t m = real_;
  for (int i = 0; i < qn_; ++i)
    if (q_[i].state == kReal) m |= bit(q_[i].ev.key);
  return m & ~bit(exceptA) & ~bit(exceptB);
}

void GhostFilter::drop(int k) {
  phantom_ |= bit(k);
  ++dropped_;
}

int GhostFilter::release(Out* out) {
  for (int i = 0; i < qn_; ++i)
    if (q_[i].state == kUndecided) return 0;
  int n = 0;
  for (int i = 0; i < qn_; ++i) {
    const Entry& e = q_[i];
    if (e.state == kReal) {
      real_ |= bit(e.ev.key);
      out[n++] = Out{e.ev, e.ms};
    } else {
      drop(e.ev.key);
    }
  }
  qn_ = 0;
  return n;
}

int GhostFilter::flushAll(Out* out) {
  for (int i = 0; i < qn_; ++i)
    if (q_[i].state == kUndecided) q_[i].state = kReal;
  return release(out);
}

int GhostFilter::tick(uint32_t nowMs, Out* out) {
  if (!qn_ || nowMs - q_[0].ms < kFlushMs) return 0;
  return flushAll(out);
}

int GhostFilter::push(const KeyEvent& e, uint32_t nowMs, Out* out) {
  const int k = e.key;
  if (k >= kNumKeys) {                       // G0 (or junk): not part of the matrix
    out[0] = Out{e, nowMs};
    return 1;
  }
  const uint64_t b = bit(k);
  int n = 0;
  if (!e.down) {
    if (qn_) n = flushAll(out);              // its downs are older than this up
    if (phantom_ & b) {
      phantom_ &= ~b;
      ++swallowed_;
      return n;
    }
    real_ &= ~b;
    out[n++] = Out{e, nowMs};
    return n;
  }

  if (real_ & b) return 0;                    // duplicate down
  for (int i = 0; i < qn_; ++i)
    if (q_[i].ev.key == k) return 0;
  phantom_ &= ~b;                             // a down for a "phantom": its up was lost, start afresh
  if (qn_) {
    const int32_t dt = int32_t(e.timeUs - q_[0].ev.timeUs);
    if (dt > int32_t(kWindowUs) || dt < -int32_t(kWindowUs)) n = flushAll(out);
  }

  const uint64_t settled = real_ | phantom_;
  if (ghostCompletes(k, settled)) {           // 1. lone completing key: a phantom
    drop(k);
    return n;
  }
  const uint64_t queued = queueMask();
  if (qn_ && ghostCompletes(k, settled | queued)) {
    // 2b. the partner of a held-back down: one of the two is a phantom.
    int idx = -1, cnt = 0;
    for (int i = 0; i < qn_; ++i)
      if (q_[i].state == kUndecided && (q_[i].partners & b)) { idx = i; ++cnt; }
    bool keep = false;
    if (cnt == 1) {
      const int x = q_[idx].ev.key;
      const uint64_t held = heldForMatch(x, k);
      const bool mx = knownGesture(held, x), mk = knownGesture(held, k);
      q_[idx].state = (mx && !mk) ? kReal : kDrop;
      keep = mk && !mx;
    } else {
      for (int i = 0; i < qn_; ++i)
        if (q_[i].state == kUndecided && (q_[i].partners & b)) q_[i].state = kDrop;
    }
    if (!keep) {
      drop(k);
    } else if (qn_ < kQueue) {
      q_[qn_++] = Entry{e, nowMs, 0, kReal};
    } else {                                  // cannot happen with kQueue >= 2; stay safe
      n += release(out + n);
      real_ |= b;
      out[n++] = Out{e, nowMs};
      return n;
    }
    return n + release(out + n);
  }

  // 2a. would this down make some open key read as pressed?
  const uint64_t closed = settled | queued;
  const uint64_t ghosts = ghostClosure(closed | b) & ~ghostClosure(closed) & ~b;
  if (ghosts || qn_) {
    if (qn_ == kQueue) n += flushAll(out + n);
    q_[qn_++] = Entry{e, nowMs, ghosts, ghosts ? kUndecided : kReal};
    return n + release(out + n);
  }
  real_ |= b;                                 // 3. an ordinary key
  out[n++] = Out{e, nowMs};
  return n;
}

}  // namespace ui
}  // namespace pg
