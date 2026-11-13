// See tilt_input.h.
#include "tilt_input.h"

#include <atomic>

#include "../platform/imu.h"
#include "pg/tilt/tilt.h"

namespace app {
namespace tilt {

namespace {

// How the BMI270 sits in the case: which chip axis is the device's roll axis and
// the signs that make X positive for "right side down" and Y positive for "far
// edge down" (away). NOT KNOWN YET (docs/hw-spike.md: the spike's report prints
// the gravity vector at rest, which answers it): this assumes the chip's x runs
// along the keyboard to the right and its y toward the screen's far edge, z out
// of the keys. A device check (docs/STATE.md): roll right should move the X
// macro up, tilt away the Y macro up; if not, flip the sign or set the swap.
constexpr bool kSwapXY = false;
constexpr float kSignX = -1.0f;
constexpr float kSignY = -1.0f;

constexpr float kDegToRad = 0.0174532925f;
constexpr float kAccScale = 1.0f / plat::imu::kAccLsbPerG;
constexpr float kGyrScale = kDegToRad / plat::imu::kGyrLsbPerDps;

// UI -> IMU task: (generation << 4) | responseX << 2 | responseY
std::atomic<uint32_t> g_press{0};
// IMU task -> UI: the axes as two int16 (x low, y high, full scale 32767), and a counter
std::atomic<uint32_t> g_axes{0};
std::atomic<uint32_t> g_seq{0};
uint32_t g_seen = 0;       // IMU task: the press generation the tracker runs
uint32_t g_readSeq = 0;    // UI thread: the last sequence read
pg::tilt::Tracker g_tracker;   // IMU task only

inline int16_t q15(float v) { return static_cast<int16_t>(v * 32767.0f + (v < 0.0f ? -0.5f : 0.5f)); }

void onReading(const plat::imu::Raw& r, void*) {
  const uint32_t p = g_press.load(std::memory_order_acquire);
  if ((p >> 4) != g_seen) {   // a new press: its zero pose is this reading
    g_seen = p >> 4;
    g_tracker.press(static_cast<int>((p >> 2) & 3u), static_cast<int>(p & 3u));
  }
  const pg::tilt::Vec3 acc = {r.acc[0] * kAccScale, r.acc[1] * kAccScale, r.acc[2] * kAccScale};
  const pg::tilt::Vec3 gyr = {r.gyr[0] * kGyrScale, r.gyr[1] * kGyrScale, r.gyr[2] * kGyrScale};
  g_tracker.sample(acc, gyr);
  const uint32_t w = static_cast<uint16_t>(q15(g_tracker.x())) |
                     (static_cast<uint32_t>(static_cast<uint16_t>(q15(g_tracker.y()))) << 16);
  g_axes.store(w, std::memory_order_relaxed);
  g_seq.fetch_add(1, std::memory_order_release);
}

}  // namespace

bool begin() {
  g_tracker.setRate(static_cast<float>(plat::imu::kPollHz));
  g_tracker.setOrientation(kSwapXY, kSignX, kSignY);
  return plat::imu::begin(onReading, nullptr);
}

void press(int responseX, int responseY) {
  if (!plat::imu::present()) return;
  const uint32_t gen = (g_press.load(std::memory_order_relaxed) >> 4) + 1;
  g_press.store((gen << 4) | ((static_cast<uint32_t>(responseX) & 3u) << 2) | (static_cast<uint32_t>(responseY) & 3u),
                std::memory_order_release);
  g_readSeq = g_seq.load(std::memory_order_acquire);   // nothing from an earlier hold
  plat::imu::start();
}

void release() { plat::imu::stop(); }

bool read(float* x, float* y) {
  const uint32_t s = g_seq.load(std::memory_order_acquire);
  if (s == g_readSeq) return false;
  g_readSeq = s;
  const uint32_t w = g_axes.load(std::memory_order_relaxed);
  *x = static_cast<float>(static_cast<int16_t>(w & 0xFFFFu)) * (1.0f / 32767.0f);
  *y = static_cast<float>(static_cast<int16_t>(w >> 16)) * (1.0f / 32767.0f);
  return true;
}

}  // namespace tilt
}  // namespace app
