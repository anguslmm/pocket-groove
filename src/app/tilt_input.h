// The tilt gesture's sensor side on the device (docs/UX.md 10.3 "Tilt"): the
// BMI270 reader (plat::imu) feeding pg::tilt::Tracker on the IMU task, and the
// UI thread's view of its output. src/app/main.cpp calls these around G0.
//
// Threads: press / release / read run on the UI thread; the tracker runs on the
// IMU task (core 1). They meet in three atomics: a press generation with the two
// responses (UI -> IMU task), and the latest axes with a sequence number (IMU
// task -> UI). No locks.
#pragma once
#include <cstdint>

namespace app {
namespace tilt {

// Starts the IMU reader (asleep). False when there is no IMU (tilt then does nothing).
bool begin();
// G0 down with a tilt to drive: zero pose = the next reading, per-axis responses
// (pg::tilt::Response). Starts the readings.
void press(int responseX, int responseY);
// G0 up, or the hold cancelled: stops the readings.
void release();
// The axes (-1..1) if a reading arrived since the last call.
bool read(float* x, float* y);

}  // namespace tilt
}  // namespace app
