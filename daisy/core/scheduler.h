// The Daisy firmware's main loop: a cooperative scheduler. There is no operating
// system: the audio callback (an interrupt) renders, and everything else (keys,
// USB serial, the UI, the display, flash writes, the IMU) is a task here. A task
// is a function run every `periodMs` (0 = every pass of the loop); it must return
// quickly (a few ms at most: a long step delays every other task, never the audio)
// and keep its own state between calls, i.e. be a state machine.
//
// Order: tasks run in the order they were added, each at most once per pass. A task
// that is late by more than a period is not run twice to catch up: its next time is
// set from now (a stalled loop must not turn into a burst). Per task the scheduler
// keeps run counts and the run time (average and longest, over the report window),
// so the serial report shows where the main loop's time goes, as the Cardputer's
// task table does.
//
// Portable (host-tested: host/daisy/test_daisy_core.cpp). Times are passed in, so
// the tests drive a fake clock. Every comparison and difference of times is modular
// (unsigned subtraction, or its signed reading for "is it due"), so the 32-bit clocks'
// wraps are ordinary steps (host/daisy/test_daisy_clock.cpp runs across them).
#pragma once
#include <cstdint>

namespace pgd {

class Scheduler {
 public:
  using Fn = void (*)(void* user, uint32_t nowMs);
  static constexpr int kMaxTasks = 16;

  struct TaskStats {
    const char* name;
    uint32_t periodMs;
    uint32_t runs;        // since boot
    uint32_t windowRuns;  // since the last takeWindow()
    uint32_t maxUs;       // longest run in the window
    uint64_t totalUs;     // run time in the window
  };

  // Returns the task's index, or -1 when full.
  int add(const char* name, uint32_t periodMs, Fn fn, void* user);
  // One pass: runs every task that is due. nowMs and nowUs are read per task
  // through the clock functions (the tests pass fakes).
  using MsClock = uint32_t (*)(void*);
  using UsClock = uint32_t (*)(void*);
  void runOnce(MsClock ms, UsClock us, void* clockUser);
  // Lets a task ask to be run on the next pass regardless of its period.
  void poke(int task);
  void setPeriod(int task, uint32_t periodMs);

  int count() const { return n_; }
  const TaskStats& stats(int i) const { return t_[i].st; }
  // Starts a new report window (the max / total figures restart).
  void takeWindow();
  uint32_t passes() const { return passes_; }
  uint32_t maxPassUs() const { return maxPassUs_; }   // longest whole pass in the window
  // The task running now, -1 between tasks: read by the audio interrupt for the load
  // meter's records (which main-loop work an over-long block interrupted).
  int current() const { return current_; }

 private:
  struct Task {
    Fn fn = nullptr;
    void* user = nullptr;
    uint32_t next = 0;
    bool primed = false;
    bool poked = false;
    TaskStats st{};
  };
  Task t_[kMaxTasks];
  int n_ = 0;
  uint32_t passes_ = 0, maxPassUs_ = 0;
  volatile int8_t current_ = -1;
};

}  // namespace pgd
