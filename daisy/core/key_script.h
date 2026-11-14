// Keys from the computer (stage 0: the Daisy has no keyboard yet). The device's USB
// serial port takes the desktop app's key-script format (host/app/script.h, which is
// host/tools/ui_shot's), one command per line, each line prefixed with '>' so it can
// share the port with the transfer protocol (lines starting "~PGX ") and the
// single-letter commands (r, h, ...):
//   >down <key>        a Cardputer key by legend: ` 1 .. = del tab q .. \ fn sft a .. ent
//   >up <key>          ctl opt alt z .. / spc G0 (pg::ui::keyFromName: aliases too)
//   >tap <key> [ms]    down, hold ms (default 40), up
//                      one key more than the Cardputer's: padjob (or F4, the desktop's
//                      key for it), the pads' job, drums / notes (kKeyPadJob)
//   >wait <ms>         the commands after it wait (the queue pauses)
//   >release           every key this script holds goes up now
// The ui_shot setup lines (battery, sd, boot, demo, empty) are skipped as the
// desktop app skips them, so the same script files drive all three. Anything else
// is handed back to the caller (shot, expect-released, ...) or reported as an error.
//
// Commands queue up in arrival order and are executed by step() from the main loop,
// so a whole script can be streamed at once and still plays with its timing.
// Portable (host-tested: host/daisy/test_daisy_core.cpp).
#pragma once
#include <cstdint>

#include "pg/platform.h"

namespace pgd {

class KeyScript {
 public:
  static constexpr int kQueue = 128;
  static constexpr uint32_t kDefaultTapMs = 40;
  // Not a Cardputer key: the desktop's padjob binding (F4, host/app/keymap.h). Its
  // down event toggles the pads' job (App::handleKey); bit 60 of held().
  static constexpr uint8_t kKeyPadJob = 60;

  enum class Parse : uint8_t {
    Queued,      // key or wait command queued
    Skipped,     // a comment, an empty line or a ui_shot setup line
    Other,       // not a key command: the caller handles it (verb / arg below)
    UnknownKey,  // a key command naming no key
    BadNumber,   // wait / tap with a malformed time
    Full,        // the queue is full: the command was dropped
  };
  // One line, without the leading '>' and the newline. verb / arg (for Other):
  // pointers into a copy kept until the next parse() call.
  Parse parse(const char* line);
  const char* verb() const { return verb_; }
  const char* arg() const { return arg_; }

  // Executes due commands. For each key event it calls emit(user, event); events
  // carry timeUs = nowMs * 1000. Returns the number of events emitted.
  using Emit = void (*)(void* user, const pg::KeyEvent& e);
  int step(uint32_t nowMs, Emit emit, void* user);
  // Drops the queue and releases every key the script holds (emit is called).
  void releaseAll(uint32_t nowMs, Emit emit, void* user);

  int queued() const { return count_; }
  bool idle() const { return count_ == 0; }
  uint64_t held() const { return held_; }   // bit = key id (kKeyG0 = bit 56, kKeyPadJob 60)
  uint32_t eventsEmitted() const { return events_; }
  uint32_t dropped() const { return dropped_; }

 private:
  enum Op : uint8_t { kDown, kUp, kWait };
  struct Cmd {
    Op op;
    uint8_t key;
    uint32_t ms;
  };
  bool pushCmd(Op op, uint8_t key, uint32_t ms);
  Cmd q_[kQueue];
  int head_ = 0, count_ = 0;
  bool waiting_ = false;
  uint32_t until_ = 0;
  uint64_t held_ = 0;
  uint32_t events_ = 0, dropped_ = 0;
  char buf_[96] = {};
  const char* verb_ = "";
  const char* arg_ = "";
};

}  // namespace pgd
