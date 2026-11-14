#include "key_script.h"

#include <cstring>

#include "pg/ui/keys.h"

namespace pgd {

namespace {

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

// Decimal milliseconds, 0..600000; false if malformed.
bool parseMs(const char* s, uint32_t* out) {
  if (!s || !*s) return false;
  uint32_t v = 0;
  for (; *s; ++s) {
    if (*s < '0' || *s > '9') return false;
    v = v * 10u + static_cast<uint32_t>(*s - '0');
    if (v > 600000u) return false;
  }
  *out = v;
  return true;
}

// The key id for a legend, or -1. "G0" is accepted in any case.
int keyId(const char* name) {
  if (!name || !*name) return -1;
  if ((name[0] == 'G' || name[0] == 'g') && name[1] == '0' && name[2] == 0) return pg::kKeyG0;
  const int k = pg::ui::keyFromName(name);
  return (k >= 0 && k <= pg::kKeyG0) ? k : -1;
}

}  // namespace

bool KeyScript::pushCmd(Op op, uint8_t key, uint32_t ms) {
  if (count_ >= kQueue) {
    ++dropped_;
    return false;
  }
  q_[(head_ + count_) % kQueue] = Cmd{op, key, ms};
  ++count_;
  return true;
}

KeyScript::Parse KeyScript::parse(const char* line) {
  // Copy and split into up to three tokens.
  size_t n = 0;
  while (line && line[n] && n + 1 < sizeof(buf_)) {
    buf_[n] = line[n];
    ++n;
  }
  buf_[n] = 0;
  char* tok[3] = {nullptr, nullptr, nullptr};
  int nt = 0;
  char* p = buf_;
  while (*p && nt < 3) {
    while (isSpace(*p)) ++p;
    if (!*p || *p == '#') break;
    tok[nt++] = p;
    while (*p && !isSpace(*p)) ++p;
    if (*p) *p++ = 0;
  }
  verb_ = tok[0] ? tok[0] : "";
  arg_ = tok[1] ? tok[1] : "";
  if (nt == 0) return Parse::Skipped;
  const char* v = tok[0];
  static const char* const kSetup[] = {"battery", "sd", "boot", "demo", "empty"};
  for (const char* s : kSetup)
    if (std::strcmp(v, s) == 0) return Parse::Skipped;

  if (std::strcmp(v, "down") == 0 || std::strcmp(v, "up") == 0 || std::strcmp(v, "tap") == 0) {
    const int k = keyId(tok[1]);
    if (k < 0) return Parse::UnknownKey;
    if (v[0] == 't') {
      uint32_t ms = kDefaultTapMs;
      if (tok[2] && !parseMs(tok[2], &ms)) return Parse::BadNumber;
      if (count_ + 3 > kQueue) {
        ++dropped_;
        return Parse::Full;
      }
      pushCmd(kDown, static_cast<uint8_t>(k), 0);
      pushCmd(kWait, 0, ms);
      pushCmd(kUp, static_cast<uint8_t>(k), 0);
      return Parse::Queued;
    }
    return pushCmd(v[0] == 'd' ? kDown : kUp, static_cast<uint8_t>(k), 0) ? Parse::Queued : Parse::Full;
  }
  if (std::strcmp(v, "wait") == 0) {
    uint32_t ms = 0;
    if (!parseMs(tok[1], &ms)) return Parse::BadNumber;
    return pushCmd(kWait, 0, ms) ? Parse::Queued : Parse::Full;
  }
  return Parse::Other;
}

int KeyScript::step(uint32_t nowMs, Emit emit, void* user) {
  int emitted = 0;
  while (count_ > 0) {
    if (waiting_) {
      if (static_cast<int32_t>(nowMs - until_) < 0) break;
      waiting_ = false;
    }
    const Cmd c = q_[head_];
    head_ = (head_ + 1) % kQueue;
    --count_;
    if (c.op == kWait) {
      if (c.ms == 0) continue;
      waiting_ = true;
      until_ = nowMs + c.ms;
      continue;
    }
    const uint64_t bit = 1ull << c.key;
    const bool down = c.op == kDown;
    if (down) held_ |= bit;
    else held_ &= ~bit;
    pg::KeyEvent e;
    e.key = c.key;
    e.down = down;
    e.timeUs = nowMs * 1000u;
    ++events_;
    ++emitted;
    if (emit) emit(user, e);
  }
  return emitted;
}

void KeyScript::releaseAll(uint32_t nowMs, Emit emit, void* user) {
  head_ = 0;
  count_ = 0;
  waiting_ = false;
  for (int k = 0; k <= pg::kKeyG0; ++k) {
    const uint64_t bit = 1ull << k;
    if (!(held_ & bit)) continue;
    held_ &= ~bit;
    pg::KeyEvent e;
    e.key = static_cast<uint8_t>(k);
    e.down = false;
    e.timeUs = nowMs * 1000u;
    ++events_;
    if (emit) emit(user, e);
  }
}

}  // namespace pgd
