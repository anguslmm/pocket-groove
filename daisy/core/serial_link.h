// The USB serial port's input, split three ways (one port, as on the Cardputer):
//   "~PGX ..." lines     the transfer protocol (pg::io::Xfer, tools/pg): bytes are
//                        offered to Xfer first, and it claims its own lines
//   ">..." lines         key-script commands (key_script.h: keys from the computer)
//   anything else        single-letter commands (r = report, h = help, ...), one per
//                        byte, as the Cardputer's serial port takes them
// A '>' starts a script line wherever it comes (it is no command letter), a newline
// ends it; one longer than kMaxLine is dropped whole (counted). Portable (host/daisy/test_daisy_core.cpp).
#pragma once
#include <cstdint>

#include "pg/io/xfer.h"

namespace pgd {

class SerialLink {
 public:
  static constexpr int kMaxLine = 95;
  enum class Kind : uint8_t { None, Xfer, Letter, ScriptLine };

  // Feeds one byte. Xfer: the byte was the transfer protocol's (call
  // xfer.service() when xfer.lineReady()). Letter: letter() is the command.
  // ScriptLine: line() is a complete script line (without '>' and newline).
  Kind feed(uint8_t b, uint32_t nowMs, pg::io::Xfer& xfer);
  char letter() const { return letter_; }
  const char* line() const { return line_; }
  uint32_t overlongLines() const { return overlong_; }
  uint32_t scriptLines() const { return lines_; }

 private:
  bool atLineStart_ = true;
  bool inScript_ = false;
  bool overflow_ = false;
  int len_ = 0;
  char line_[kMaxLine + 1] = {};
  char letter_ = 0;
  uint32_t overlong_ = 0, lines_ = 0;
};

}  // namespace pgd
