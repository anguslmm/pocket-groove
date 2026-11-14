#include "scheduler.h"

namespace pgd {

int Scheduler::add(const char* name, uint32_t periodMs, Fn fn, void* user) {
  if (n_ >= kMaxTasks || !fn) return -1;
  Task& t = t_[n_];
  t.fn = fn;
  t.user = user;
  t.primed = false;
  t.poked = false;
  t.st = TaskStats{};
  t.st.name = name ? name : "?";
  t.st.periodMs = periodMs;
  return n_++;
}

void Scheduler::poke(int task) {
  if (task >= 0 && task < n_) t_[task].poked = true;
}

void Scheduler::setPeriod(int task, uint32_t periodMs) {
  if (task >= 0 && task < n_) t_[task].st.periodMs = periodMs;
}

void Scheduler::runOnce(MsClock ms, UsClock us, void* clockUser) {
  const uint32_t pass0 = us(clockUser);
  for (int i = 0; i < n_; ++i) {
    Task& t = t_[i];
    const uint32_t now = ms(clockUser);
    if (!t.primed) {
      t.primed = true;
      t.next = now;
    }
    const bool due = t.poked || t.st.periodMs == 0 || static_cast<int32_t>(now - t.next) >= 0;
    if (!due) continue;
    t.poked = false;
    if (t.st.periodMs) {
      t.next += t.st.periodMs;
      // Late by more than a period (a stalled loop): restart the rhythm from now.
      if (static_cast<int32_t>(now - t.next) >= 0) t.next = now + t.st.periodMs;
    }
    const uint32_t t0 = us(clockUser);
    t.fn(t.user, now);
    const uint32_t dt = us(clockUser) - t0;
    ++t.st.runs;
    ++t.st.windowRuns;
    t.st.totalUs += dt;
    if (dt > t.st.maxUs) t.st.maxUs = dt;
  }
  ++passes_;
  const uint32_t pass = us(clockUser) - pass0;
  if (pass > maxPassUs_) maxPassUs_ = pass;
}

void Scheduler::takeWindow() {
  for (int i = 0; i < n_; ++i) {
    t_[i].st.windowRuns = 0;
    t_[i].st.maxUs = 0;
    t_[i].st.totalUs = 0;
  }
  maxPassUs_ = 0;
}

}  // namespace pgd
