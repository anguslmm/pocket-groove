// The desktop's microphone. See mic.h.
#include "mic.h"

#include <cctype>
#include <cstdio>

#include "input_names.h"
#include "samples.h"

namespace desk {

using pg::sampler::RecState;

MicChoice chooseMicInput(const std::vector<std::string>& inputs, const std::string* defaultName,
                         const std::string& wanted) {
  MicChoice c;
  auto take = [&](const std::string& n, const char* why) {
    c.ok = true;
    c.name = c.open = n;
    c.why = why;
    return c;
  };
  if (!wanted.empty()) {
    const int at = findInput(inputs, wanted);
    if (at >= 0) return take(inputs[size_t(at)], "--mic-device");
    c.why = "no input matches --mic-device \"" + wanted + "\"";
    return c;
  }
  const bool knowDefault = defaultName && !defaultName->empty();
  if (knowDefault && !isDeviceStream(*defaultName)) {
    take(*defaultName, "the system default");
    bool listed = false;
    for (const std::string& n : inputs) listed = listed || n == *defaultName;
    if (!listed) c.open.clear();   // not by that name in the list: open SDL's default itself
    return c;
  }
  const char* after = knowDefault ? "the system default is the device's own stream" : "the system default is not known";
  for (const std::string& n : inputs)
    if (!isDeviceStream(n) && (containsNoCase(n.c_str(), "microphone") || containsNoCase(n.c_str(), "built-in")))
      return take(n, knowDefault ? "a microphone; the system default is the device's own stream"
                                 : "a microphone by name; the system default is not known");
  for (const std::string& n : inputs)
    if (!isDeviceStream(n)) {
      take(n, "the first other input; ");
      c.why += after;
      return c;
    }
  if (inputs.empty() && !knowDefault) {   // SDL lists nothing: try its default
    c.ok = true;
    c.name = "the default input";
    c.why = "SDL lists no inputs";
    return c;
  }
  c.why = std::string("no input other than the device's own stream (\"") + kListenDeviceName + "\")";
  return c;
}

std::string micInputsLine(const std::vector<std::string>& inputs, const MicChoice& c) {
  std::string s = "[mic] inputs: ";
  for (size_t i = 0; i < inputs.size(); ++i) s += (i ? ", \"" : "\"") + inputs[i] + "\"";
  if (inputs.empty()) s += "(none)";
  if (c.ok) s += "; using \"" + c.name + "\" (" + c.why + ")";
  else s += "; none usable: " + c.why;
  return s;
}

void Mic::onAudio(const int16_t* mono, int n, void* user) {
  Mic* m = static_cast<Mic*>(user);
  const uint32_t head = m->head_.load(std::memory_order_relaxed);
  const uint32_t tail = m->tail_.load(std::memory_order_acquire);
  const uint32_t space = kRing - (head - tail);
  const uint32_t k = uint32_t(n) < space ? uint32_t(n) : space;
  for (uint32_t i = 0; i < k; ++i) m->ring_[(head + i) & (kRing - 1)] = mono[i];
  m->head_.store(head + k, std::memory_order_release);
  if (k < uint32_t(n)) m->dropped_.fetch_add(uint32_t(n) - k, std::memory_order_relaxed);
}

bool Mic::ready(const char** why) {
  if (!samples_ || !src_) {
    *why = "NO MIC";
    return false;
  }
  *why = "";
  return true;
}

bool Mic::erase(int slot) {
  std::string msg;
  return samples_ && samples_->clearSlot(slot, &msg);
}

bool Mic::start(int slot) {
  why_ = "";
  if (armed_ || !samples_ || !src_) {
    why_ = "NO MIC";
    return false;
  }
  head_.store(0);
  tail_.store(0);
  dropped_.store(0);
  sink_.v.clear();
  sink_.v.reserve(pg::sampler::kSlotMaxSamples + 1024);
  rec_.start(&sink_);
  err_.clear();
  zeroRun_ = 0;
  heard_ = 0;
  inPeak_ = 0;
  silentShown_ = false;
  silentNotice_.clear();
  if (!src_->open(&Mic::onAudio, this, &err_)) {
    std::printf("[mic] cannot open %s: %s\n", src_->name().c_str(), err_.c_str());
    why_ = "CANNOT OPEN";
    return false;
  }
  slot_ = slot;
  armed_ = true;
  std::printf("[mic] recording into slot %d from %s (hold L; it starts at the sound)\n", slot + 1,
              src_->name().c_str());
  return true;
}

void Mic::release() {
  if (armed_) rec_.release();
}

void Mic::drain() {
  int16_t buf[512];
  for (;;) {
    const uint32_t head = head_.load(std::memory_order_acquire);
    const uint32_t tail = tail_.load(std::memory_order_relaxed);
    uint32_t n = head - tail;
    if (n == 0) return;
    if (n > 512) n = 512;
    for (uint32_t i = 0; i < n; ++i) {
      buf[i] = ring_[(tail + i) & (kRing - 1)];
      zeroRun_ = buf[i] == 0 ? zeroRun_ + 1 : 0;
      const int a = buf[i] < 0 ? -int(buf[i]) : int(buf[i]);
      if (a > inPeak_) inPeak_ = a;
    }
    tail_.store(tail + n, std::memory_order_release);
    heard_ += n;
    rec_.feed(buf, int(n));
  }
}

bool Mic::poll(pg::sampler::RecView* v) {
  *v = pg::sampler::RecView{};
  v->why = why_;
  if (!armed_) return false;
  const bool rel = rec_.released();
  drain();
  checkSilent();
  if (rel && !rec_.ended()) rec_.finish();
  v->state = rec_.state();
  v->slot = slot_;
  v->samples = rec_.ended() ? rec_.length() : rec_.recorded();
  v->level = rec_.level();
  v->threshold = rec_.threshold();
  v->peak = rec_.peak();
  v->cols = rec_.cols();
  v->colsUsed = rec_.colsUsed();
  if (rec_.ended()) {
    finishSession();
    v->state = why_[0] ? RecState::Failed : rec_.state();
    v->why = why_;
  }
  return true;
}

// A second of exact zeros while WAITING: not a quiet room (a real microphone's noise
// is never all 0) but a dead input. Said once per hold.
void Mic::checkSilent() {
  const RecState s = rec_.state();
  if (silentShown_ || zeroRun_ < kSilentSamples || (s != RecState::Settling && s != RecState::Waiting)) return;
  silentShown_ = true;
  ++silentCount_;
  const std::string name = src_->name();
  std::printf("[mic] \"%s\" gives digital silence (every sample 0 for 1 s). Either it is the wrong input "
              "(e.g. the Pocket Groove's own USB stream): choose another with --mic-device NAME "
              "(--list-inputs lists them) or in System Settings > Sound > Input; or macOS is not letting this "
              "app hear the microphone: allow it for the app the desktop was started from (Terminal, iTerm, "
              "VS Code) in System Settings > Privacy & Security > Microphone, then quit and restart that app.\n",
              name.c_str());
  std::fflush(stdout);
  // The toast: short, upper case, ASCII (a non-ASCII character becomes one '?'), fits the screen.
  std::string t = "MIC SILENT: ";
  const size_t maxLen = 36;
  for (size_t i = 0; i < name.size() && t.size() < maxLen; ++i) {
    const unsigned char ch = static_cast<unsigned char>(name[i]);
    if (ch >= 0x80 && ch < 0xC0) continue;   // a UTF-8 continuation byte
    t += ch >= 0x20 && ch < 0x7F ? char(std::toupper(ch)) : '?';
  }
  silentNotice_ = t;
}

void Mic::finishSession() {
  src_->close();
  armed_ = false;
  const RecState s = rec_.state();
  const uint32_t drops = dropped_.load();
  if (s == RecState::Done || s == RecState::Full) {
    std::string msg;
    if (!samples_->putRecording(slot_, sink_.v.data(), rec_.length(), &msg)) {
      why_ = "CANNOT WRITE";
      std::printf("[mic] slot %d: %s\n", slot_ + 1, msg.c_str());
      return;
    }
    lastFile_ = samples_->fileOf(slot_);
    std::printf("[mic] slot %d: %s, %.2f s recorded, peak %d, threshold %d, room rms %d, %u samples dropped\n",
                slot_ + 1, lastFile_.c_str(), double(rec_.length()) / pg::sampler::kPoolRate, int(rec_.peak()),
                int(rec_.threshold()), int(rec_.floorRms()), unsigned(drops));
  } else {
    std::printf("[mic] slot %d: %s (nothing kept; the input gave %.2f s, peak %d)\n", slot_ + 1,
                pg::sampler::recStateName(s), double(heard_) / 44100.0, inPeak_);
  }
}

}  // namespace desk
