// Platform layer: keyboard reader. See keys.h.
//
// Why not M5Cardputer's Keyboard class: it reports state, not events, uses
// std::vector on every update and reads at most ONE controller event per
// update() call. We talk to the TCA8418 directly with the same register setup
// the library uses (Adafruit_TCA8418::begin + matrix(7, 8) + enableInterrupts).
//
// Hardware-only pitfalls handled here:
//  * INT is level-held low while events are pending (CFG.INT_CFG = 0), so an
//    edge interrupt alone can miss work. After every drain the line is
//    re-checked, and the task also wakes every 5 ms to look at the level.
//  * All I2C happens on this task or the UI thread, never in the ISR and never
//    on the audio task. M5GFX's i2c layer locks each transaction per port, so
//    the codec (UI thread) and this task can share the bus.
//  * The INT line itself is unverified on hardware (pull-up, routing to
//    GPIO11). Every 20 ms the task also reads the FIFO event count while INT
//    is high; events found that way are counted (Stats::polledEvents), so the
//    keyboard works either way and the smoke report says which case is true.
//  * Rows/columns that are not part of the 7x8 matrix stay in "GPI event" mode
//    like in the library; their codes (>= 97) are counted and dropped instead
//    of indexing past the 4x14 map.
#include "keys.h"

#include <Arduino.h>
#include <M5Unified.h>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

namespace plat {
namespace keys {

namespace {

constexpr uint8_t  kAddr     = 0x34;
constexpr uint32_t kI2cHz    = 400000;
constexpr int      kPinInt   = 11;
constexpr int      kPinG0    = 0;
constexpr int      kTaskPrio = 10;      // above the UI loop (1), far below audio (20)
constexpr int      kTaskCore = 1;       // with the UI; core 0 is left to audio
constexpr uint32_t kTaskStack = kTaskStackBytes;   // I2C via M5GFX + queue + listener

// TCA8418 registers (datasheet names)
constexpr uint8_t REG_CFG = 0x01, REG_INT_STAT = 0x02, REG_KEY_LCK_EC = 0x03, REG_KEY_EVENT_A = 0x04;
constexpr uint8_t REG_GPIO_INT_STAT_1 = 0x11, REG_GPIO_INT_EN_1 = 0x1A, REG_KP_GPIO_1 = 0x1D;
constexpr uint8_t REG_GPI_EM_1 = 0x20, REG_GPIO_DIR_1 = 0x23, REG_GPIO_INT_LVL_1 = 0x26;
constexpr uint8_t CFG_KE_IEN = 0x01, CFG_GPI_IEN = 0x02, CFG_OVR_FLOW_IEN = 0x08;
constexpr uint8_t STAT_GPI_INT = 0x02, STAT_OVR_FLOW = 0x08;

QueueHandle_t s_queue = nullptr;
TaskHandle_t  s_task = nullptr;
bool          s_present = false;

volatile uint32_t s_intTimeUs = 0;
volatile bool     s_intPending = false;

volatile uint32_t s_heldLo = 0, s_heldHi = 0;   // bits 0..31 / 32..56
volatile uint32_t s_events = 0, s_drops = 0, s_unknown = 0, s_ovf = 0, s_i2cErr = 0;
volatile uint32_t s_lastLat = 0, s_maxLat = 0;
volatile uint32_t s_intLowDrains = 0, s_polledEvents = 0;
volatile uint8_t  s_lastRaw = 0;

void (*volatile s_listener)(const Event&, void*) = nullptr;
void* volatile s_listenerUser = nullptr;

bool rd(uint8_t reg, uint8_t* v) { return M5.In_I2C.readRegister(kAddr, reg, v, 1, kI2cHz); }
bool wr(uint8_t reg, uint8_t v)  { return M5.In_I2C.writeRegister(kAddr, reg, &v, 1, kI2cHz); }

void IRAM_ATTR onInt(void*) {
  if (!s_intPending) {
    s_intTimeUs = static_cast<uint32_t>(esp_timer_get_time());
    s_intPending = true;
  }
  BaseType_t woken = pdFALSE;
  if (s_task) vTaskNotifyGiveFromISR(s_task, &woken);
  if (woken) portYIELD_FROM_ISR();
}

bool isHeld(uint8_t key) {
  return key < 32 ? ((s_heldLo >> key) & 1u) : ((s_heldHi >> (key - 32)) & 1u);
}
void setHeld(uint8_t key, bool down) {
  if (key < 32) { if (down) s_heldLo = s_heldLo | (1u << key); else s_heldLo = s_heldLo & ~(1u << key); }
  else { const uint32_t b = 1u << (key - 32); if (down) s_heldHi = s_heldHi | b; else s_heldHi = s_heldHi & ~b; }
}

// Key-ups that did not fit into the queue (this task only). A lost key-up
// would leave the UI with a key held forever (stuck note), so it is retried
// until there is room; a lost key-down is simply forgotten (held state
// reverted, so its key-up is dropped as a duplicate too).
uint64_t s_pendUp = 0;

void flushPendingUps() {
  if (!s_pendUp || !s_queue) return;
  for (uint8_t k = 0; k < kNumIds; ++k) {
    if (!((s_pendUp >> k) & 1u)) continue;
    Event e;
    e.ev.key = k;
    e.ev.down = false;
    e.ev.timeUs = 0;
    e.raw = 0;
    e.tIntUs = e.tReadUs = static_cast<uint32_t>(esp_timer_get_time());
    if (xQueueSend(s_queue, &e, 0) != pdTRUE) return;   // still full: next wake-up
    s_pendUp &= ~(1ull << k);
  }
}

void emit(uint8_t key, bool down, uint8_t raw, uint32_t tInt, uint32_t tRead) {
  if (isHeld(key) == down) return;           // duplicate (e.g. release after an overflow flush)
  flushPendingUps();                         // keep older releases ahead of this event
  setHeld(key, down);
  Event e;
  e.ev.key = key;
  e.ev.down = down;
  e.ev.timeUs = 0;
  e.raw = raw;
  e.tIntUs = tInt;
  e.tReadUs = tRead;
  s_events = s_events + 1;
  auto fn = s_listener;
  if (fn) fn(e, s_listenerUser);
  const uint64_t bit = 1ull << key;
  if (down && (s_pendUp & bit)) {            // its previous release is still pending: drop this press
    setHeld(key, false);
    s_drops = s_drops + 1;
    return;
  }
  if (s_queue && xQueueSend(s_queue, &e, 0) != pdTRUE) {
    s_drops = s_drops + 1;
    if (down) setHeld(key, false);           // forget the press: its release is then a duplicate
    else s_pendUp |= bit;                    // the UI saw the press: the release must arrive
  }
}

void releaseAll(uint32_t now) {
  for (uint8_t k = 0; k < pg::kNumKeys; ++k) {
    if (isHeld(k)) emit(k, false, 0, now, now);
  }
}

// Reads every pending event. Returns false on an I2C error.
// Ends after at most 4 passes x 16 FIFO reads, whatever the INT line does.
bool drain() {
  for (int pass = 0; pass < 4; ++pass) {
    const uint32_t tInt = s_intPending ? s_intTimeUs : static_cast<uint32_t>(esp_timer_get_time());
    s_intPending = false;
    for (int n = 0; n < 16; ++n) {
      uint8_t code = 0;
      if (!rd(REG_KEY_EVENT_A, &code)) { s_i2cErr = s_i2cErr + 1; return false; }
      if (code == 0) break;                  // FIFO empty
      const uint32_t tRead = static_cast<uint32_t>(esp_timer_get_time());
      s_lastRaw = code;
      const uint32_t lat = tRead - tInt;
      s_lastLat = lat;
      if (lat > s_maxLat) s_maxLat = lat;
      const uint8_t k = static_cast<uint8_t>((code & 0x7F) - 1);
      const uint8_t tr = k / 10, tc = k % 10;
      if ((code & 0x7F) == 0 || tr > 6 || tc > 7) { s_unknown = s_unknown + 1; continue; }
      emit(keyFromTca(tr, tc), (code & 0x80) != 0, code, tInt, tRead);
    }
    uint8_t stat = 0;
    if (!rd(REG_INT_STAT, &stat)) { s_i2cErr = s_i2cErr + 1; return false; }
    if (stat & STAT_GPI_INT) {
      // GPI_INT can only be cleared after the GPIO_INT_STAT registers are read.
      uint8_t v = 0;
      for (uint8_t i = 0; i < 3; ++i) rd(REG_GPIO_INT_STAT_1 + i, &v);
    }
    if (stat & STAT_OVR_FLOW) {
      // Events were lost inside the chip: held-state can no longer be trusted.
      s_ovf = s_ovf + 1;
      releaseAll(static_cast<uint32_t>(esp_timer_get_time()));
    }
    if (!wr(REG_INT_STAT, 0x1F)) { s_i2cErr = s_i2cErr + 1; return false; }   // write 1 to clear
    if (digitalRead(kPinInt) != LOW) return true;                             // line released: done
  }
  return true;
}

void pollG0() {
  static uint8_t stable = 0;     // consecutive samples that differ from the reported state
  const bool down = (digitalRead(kPinG0) == LOW);
  if (down != isHeld(kKeyG0)) {
    if (++stable >= 3) {         // 3 x 5 ms debounce
      stable = 0;
      const uint32_t now = static_cast<uint32_t>(esp_timer_get_time());
      emit(kKeyG0, down, 0, now, now);
    }
  } else {
    stable = 0;
  }
}

void keyTask(void*) {
  uint32_t backoffUntil = 0;
  uint32_t tick = 0;
  for (;;) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5));
    flushPendingUps();
    pollG0();
    if (!s_present) continue;
    const uint32_t now = millis();
    if (backoffUntil && static_cast<int32_t>(now - backoffUntil) < 0) continue;
    backoffUntil = 0;
    if (digitalRead(kPinInt) == LOW) {
      s_intLowDrains = s_intLowDrains + 1;
      if (!drain()) backoffUntil = (now + 100) | 1u;   // bus trouble: do not hammer it
    } else if ((++tick & 3) == 0) {
      // INT high: normally nothing is pending. If the FIFO still holds events
      // the INT line is not reaching us; serve them anyway (20 ms latency).
      uint8_t ec = 0;
      if (!rd(REG_KEY_LCK_EC, &ec)) { s_i2cErr = s_i2cErr + 1; backoffUntil = (now + 100) | 1u; continue; }
      if ((ec & 0x0F) != 0 && digitalRead(kPinInt) != LOW) {
        const uint32_t before = s_events;
        if (!drain()) backoffUntil = (now + 100) | 1u;
        s_polledEvents = s_polledEvents + (s_events - before);
      }
    }
  }
}

bool setupController() {
  // Same writes as Adafruit_TCA8418::begin(), matrix(7, 8), flush(),
  // enableInterrupts() in M5Cardputer 1.1.1 -- plus the overflow interrupt.
  // Order differs from the library on purpose: the matrix is switched off
  // first, the FIFO flushed, and the matrix switched on LAST. A key already
  // held at power-on (the boot-menu gesture) is then reported as a fresh
  // press after the flush instead of possibly being flushed with the stale
  // events, and after a warm reset (the chip keeps its state, it is not reset
  // with the ESP32) leaving keypad mode makes it scan the held keys again.
  bool ok = true;
  ok = wr(REG_KP_GPIO_1, 0x00) && ok;                                            // matrix off (all GPIO)
  ok = wr(REG_KP_GPIO_1 + 1, 0x00) && ok;
  for (uint8_t i = 0; i < 3; ++i) ok = wr(REG_GPIO_DIR_1 + i, 0x00) && ok;       // all inputs
  for (uint8_t i = 0; i < 3; ++i) ok = wr(REG_GPI_EM_1 + i, 0xFF) && ok;         // GPI event mode
  for (uint8_t i = 0; i < 3; ++i) ok = wr(REG_GPIO_INT_LVL_1 + i, 0x00) && ok;   // falling
  for (uint8_t i = 0; i < 3; ++i) ok = wr(REG_GPIO_INT_EN_1 + i, 0xFF) && ok;
  if (!ok) return false;
  uint8_t v = 0;
  for (int n = 0; n < 16 && rd(REG_KEY_EVENT_A, &v) && v != 0; ++n) {}           // flush key events
  for (uint8_t i = 0; i < 3; ++i) rd(REG_GPIO_INT_STAT_1 + i, &v);               // flush GPI status
  wr(REG_INT_STAT, 0x1F);
  ok = wr(REG_KP_GPIO_1, 0x7F) && ok;                                            // rows 0..6 in the matrix
  ok = wr(REG_KP_GPIO_1 + 1, 0xFF) && ok;                                        // cols 0..7 in the matrix
  if (!ok) return false;
  uint8_t cfg = 0;
  if (!rd(REG_CFG, &cfg)) return false;
  cfg |= (CFG_KE_IEN | CFG_GPI_IEN | CFG_OVR_FLOW_IEN);
  return wr(REG_CFG, cfg);
}

}  // namespace

bool begin() {
  if (s_task) return s_present;
  s_queue = xQueueCreate(64, sizeof(Event));
  pinMode(kPinG0, INPUT_PULLUP);
  pinMode(kPinInt, INPUT_PULLUP);          // INT is open-drain; an external pull-up is not confirmed

  s_present = M5.In_I2C.scanID(kAddr, kI2cHz) && setupController();

  if (xTaskCreatePinnedToCore(keyTask, "pg_keys", kTaskStack, nullptr, kTaskPrio, &s_task, kTaskCore) != pdPASS) {
    s_task = nullptr;
    return false;
  }
  if (s_present) {
    attachInterruptArg(digitalPinToInterrupt(kPinInt), onInt, nullptr, FALLING);
  }
  return s_present;
}

bool poll(Event* out) {
  if (!s_queue || !out) return false;
  return xQueueReceive(s_queue, out, 0) == pdTRUE;
}

bool waitEvent(Event* out, uint32_t timeoutMs) {
  if (!s_queue || !out) return false;
  return xQueueReceive(s_queue, out, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}

uint32_t taskStackFree() { return s_task ? static_cast<uint32_t>(uxTaskGetStackHighWaterMark(s_task)) : 0; }

uint64_t heldMask() {
  return (static_cast<uint64_t>(s_heldHi) << 32) | s_heldLo;
}

void setListener(void (*fn)(const Event&, void*), void* user) {
  s_listener = nullptr;
  s_listenerUser = user;
  s_listener = fn;
}

Stats stats() {
  Stats st;
  st.controllerPresent = s_present;
  st.events = s_events;
  st.queueDrops = s_drops;
  st.unknownCodes = s_unknown;
  st.fifoOverflows = s_ovf;
  st.i2cErrors = s_i2cErr;
  st.lastIntToReadUs = s_lastLat;
  st.maxIntToReadUs = s_maxLat;
  st.lastRaw = s_lastRaw;
  st.intLowDrains = s_intLowDrains;
  st.polledEvents = s_polledEvents;
  return st;
}

}  // namespace keys
}  // namespace plat
