#include "serial_link.h"

namespace pgd {

SerialLink::Kind SerialLink::feed(uint8_t b, uint32_t nowMs, pg::io::Xfer& xfer) {
  // Xfer sees every byte (it tracks line starts itself) and claims its own lines.
  if (!inScript_ && xfer.rx(b, nowMs)) {
    atLineStart_ = (b == '\n');
    return Kind::Xfer;
  }
  if (inScript_) {
    if (b == '\n') {
      inScript_ = false;
      atLineStart_ = true;
      if (overflow_) {
        overflow_ = false;
        ++overlong_;
        return Kind::None;
      }
      if (len_ > 0 && line_[len_ - 1] == '\r') --len_;
      line_[len_] = 0;
      ++lines_;
      return Kind::ScriptLine;
    }
    // Keep Xfer's view of line starts right: it saw none of this line.
    if (len_ < kMaxLine) line_[len_++] = static_cast<char>(b);
    else overflow_ = true;
    return Kind::None;
  }
  if (b == '\n' || b == '\r') {
    atLineStart_ = true;
    return Kind::None;
  }
  if (b == '>') {
    inScript_ = true;
    overflow_ = false;
    len_ = 0;
    atLineStart_ = false;
    return Kind::None;
  }
  atLineStart_ = false;
  letter_ = static_cast<char>(b);
  return Kind::Letter;
}

}  // namespace pgd
