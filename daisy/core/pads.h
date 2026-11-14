// Velocity pads from force sensors (stage 1: four Adafruit 1075 squares, Alpha
// MF02A-N-221-A01; docs/DAISY.md "Force-sensor pads"). Pure and portable: ADC
// readings in, events out (host/daisy/test_daisy_pads.cpp, synthetic force curves).
//
// THE SENSOR. Each pad is a divider: 3V3A - FSR - ADC pin - 10 k - AGND. The ADC
// reads x = V / 3.3 = 10k / (10k + R_fsr). An FSR's conductance is roughly
// proportional to the force on it, so everything here works in conductance:
//     G = (1 / 10 k) * x / (1 - x)     (microsiemens: G = 100 uS * x / (1 - x))
// which is linear in force where the divider voltage is not (it bends over towards
// 3.3 V). Idle (no touch, > 1 M): G < 1 uS. The calibration's numbers are in uS.
//
// THE SCAN. scan() takes one reading per pad and is called at a fixed rate: on the
// Daisy once per audio block (96 frames at 48 kHz = 2.0 ms, 500 Hz), from the audio
// interrupt, so the scan interval never jitters with the main loop. The events go
// into a single-producer single-consumer ring; the main loop pops them (pop()).
//
// A HIT, per pad:
//   Idle      G < threshold. The first scan at or above it is the ONSET scan.
//   Onset     the velocity window: the onset scan and the next window-1 scans
//             (default window 2: 2.0 ms after the onset scan, 4 ms of force
//             history at most). Velocity comes from the fastest rise seen in it
//             (the onset scan's rise counted from the threshold): a rise of the
//             whole span (threshold -> ceiling) within the window is velocity 1.
//             The slope, not the level, so where the crossing fell between two
//             scans does not matter for a steady rise. Reaching the ceiling ends
//             the window at once (velocity 1). Falling below the release level
//             inside the window: a blip, no note.
//   Held      Down(pad, velocity) was sent; Pressure(pad, 0..1) follows while held,
//             (G - threshold) / (ceiling - threshold), when it moved by 1/128.
//   release   below release level = threshold * releaseRatio (hysteresis) for
//             releaseScans scans in a row (default 2: 4 ms): Up(pad). A bounce
//             that dips for one scan does not end the note: no double triggers.
//   Suppressed a crosstalk ghost (below): no events until it is released.
// CROSSTALK: when a pad's window ends, any other pad whose onset was within
// xtalkScans (default 5: 10 ms) and whose velocity (sent, or estimated so far) is
// more than its own / xtalkRatio (default 0.4) makes it a ghost: a hard hit next door
// leaks a fraction of its force into its neighbours (through the board, and through
// the ADC sampling one channel after another), and that fraction is dropped. Two
// pads hit about as hard (a flam across two pads) both play.
// The velocity curve: out = lin ^ curve (1 = linear; 0.5 lifts soft hits).
//
// LATENCY (at the Daisy's 2 ms scan): the crossing is seen 0-2 ms after it happens,
// the note is decided window-1 scans after that (2 ms), then the main loop posts it
// and the engine plays it in the next block, which the codec outputs one block later
// (4 ms): 6-8 ms from the force crossing the threshold to sound, if the main loop
// passes within 2 ms (docs/DAISY.md has the budget and the seam that saves 2 ms).
#pragma once
#include <atomic>
#include <cstdint>

namespace pgd {

struct PadCal {
  float threshold = 10.0f;   // uS: where a touch starts (the FSR at 100 k)
  float ceiling = 1000.0f;   // uS: full force (1 k): pressure 1, the velocity span
  float curve = 1.0f;        // velocity exponent, 0.25..4
  bool on = true;            // off: the pad is ignored (not wired)
};

struct PadGlobal {
  uint8_t windowScans = 2;     // the velocity window, 1..4 scans (the onset scan included)
  uint8_t releaseScans = 2;    // scans below the release level before Up, 1..4
  uint8_t xtalkScans = 5;      // onsets this close are compared, 0..20 (0: off)
  float xtalkRatio = 0.4f;     // a pad below this fraction of a neighbour's velocity is a ghost
  float releaseRatio = 0.6f;   // release level = threshold * this (hysteresis), 0.1..0.95
};

struct PadEvent {
  enum Type : uint8_t { Down = 0, Pressure = 1, Up = 2 };
  uint8_t type;
  uint8_t pad;       // 0..kPads-1
  float value;       // Down: velocity 0..1 (after the curve); Pressure: 0..1; Up: 0
  uint32_t scan;     // the scan it was decided in
};

class PadScanner {
 public:
  static constexpr int kPads = 4;
  static constexpr float kDividerUs = 100.0f;   // 1 / 10 k, in uS
  static constexpr float kPressureStep = 1.0f / 128.0f;
  static constexpr int kRing = 64;               // events (a power of two)

  // x (0..1: the ADC's reading) -> G in uS (clamped: x at most 0.9995).
  static float conductance(float x);
  // The inverse (the tests' synthetic sensors).
  static float readingFor(float uS);
  // A calibration made valid (ranges clamped, ceiling above the threshold).
  static PadCal sanitize(const PadCal& c);
  static PadGlobal sanitize(const PadGlobal& g);

  // Main loop. Taken over by scan() at its next call (a copy, so a half-made change
  // is never seen; see pads.cpp).
  void setCal(int pad, const PadCal& c);
  void setGlobal(const PadGlobal& g);
  const PadCal& cal(int pad) const { return want_[pad & (kPads - 1)]; }
  const PadGlobal& global() const { return wantG_; }
  // scans per second (the report's milliseconds)
  void setScanHz(float hz) { scanHz_ = hz > 1.0f ? hz : 1.0f; }
  float scanHz() const { return scanHz_; }

  // The scan context (the audio interrupt): one reading per pad, 0..1.
  void scan(const float* x);
  // The main loop: the next event, false when none.
  bool pop(PadEvent* e);
  // The main loop, while nothing scans (tests, boot): back to idle, events dropped.
  void reset();

  // For the report (written by scan(), read anywhere).
  enum class State : uint8_t { Idle, Onset, Held, Suppressed };
  State state(int pad) const { return pad_[pad].state; }
  static const char* stateName(State s);
  float lastReading(int pad) const { return pad_[pad].x; }
  float lastUs(int pad) const { return pad_[pad].g; }
  float pressure(int pad) const;          // 0..1 from the last reading
  float lastVelocity(int pad) const { return pad_[pad].lastVel; }
  float peakUs(int pad) const { return pad_[pad].maxG; }   // since the last report
  void clearPeaks();
  uint32_t hits(int pad) const { return pad_[pad].hits; }
  uint32_t ghosts(int pad) const { return pad_[pad].ghosts; }
  uint32_t blips(int pad) const { return pad_[pad].blips; }
  uint32_t scans() const { return scan_; }
  uint32_t dropped() const { return dropped_; }

 private:
  struct Pad {
    State state = State::Idle;
    uint32_t onset = 0;        // the onset scan
    uint8_t seen = 0;          // scans of the window so far
    uint8_t below = 0;         // scans in a row below the release level
    float prev = 0.0f;         // G at the previous scan
    float maxRise = 0.0f;      // in the window, uS per scan
    float estimate = 0.0f;     // linear velocity so far (window) or sent (held)
    float sentP = -1.0f;       // the last pressure sent
    float x = 0.0f, g = 0.0f;  // the last reading, its conductance
    float maxG = 0.0f;
    float lastVel = 0.0f;
    uint32_t hits = 0, ghosts = 0, blips = 0;
  };
  void push(uint8_t type, int pad, float value);
  float linear(const Pad& p, const PadCal& c) const;
  bool ghost(int pad, float lin) const;
  void takeOver();

  Pad pad_[kPads];
  PadCal cal_[kPads];       // what scan() uses
  PadGlobal g_{};
  PadCal want_[kPads];      // what the main loop set
  PadGlobal wantG_{};
  std::atomic<uint32_t> wantSeq_{1};
  uint32_t usedSeq_ = 0;
  float scanHz_ = 500.0f;
  uint32_t scan_ = 0;
  PadEvent ring_[kRing];
  std::atomic<uint32_t> head_{0}, tail_{0};
  uint32_t dropped_ = 0;
};

}  // namespace pgd
