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
constexpr uint32_t kTaskStack = 4096;   // bytes; I2C via M5GFX + queue + listener

// TCA8418 registers (datasheet names)
constexpr uint8_t REG_CFG = 0x01, REG_INT_STAT = 0x02, REG_KEY_LCK_EC = 0x03, REG_KEY_EVENT_A = 0x04;
constexpr uint8_t REG_GPIO_INT_STAT_1 = 0x11, REG_GPIO_INT_EN_1 = 0x1A, REG_KP_GPIO_1 = 0x1D;
constexpr uint8_t REG_GPI_EM_1 = 0x20, REG_GPIO_DIR_1 = 0x23, REG_GPIO_INT_LVL_1 = 0x26;
constexpr uint8_t CFG_KE_IEN = 0x01, CFG_GPI_IEN = 0x02, CFG_OVR_FLOW_IEN = 0x08;
constexpr uint8_t STAT_OVR_FLOW = 0x08;

QueueHandle_t s_queue = nullptr;
TaskHandle_t  s_task = nullptr;
bool          s_present = false;

volatile uint32_t s_intTimeUs = 0;
volatile bool     s_intPending = false;

volatile uint32_t s_heldLo = 0, s_heldHi = 0;   // bits 0..31 / 32..56
volatile uint32_t s_events = 0, s_drops = 0, s_unknown = 0, s_ovf = 0, s_i2cErr = 0;
volatile uint32_t s_lastLat = 0, s_maxLat = 0;
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

void emit(uint8_t key, bool down, uint8_t raw, uint32_t tInt, uint32_t tRead) {
  if (isHeld(key) == down) return;           // duplicate (e.g. release after an overflow flush)
  setHeld(key, down);
  Event e;
  e.ev.key = key;
  e.ev.down = down;
  e.raw = raw;
  e.tIntUs = tInt;
  e.tReadUs = tRead;
  s_events = s_events + 1;
  auto fn = s_listener;
  if (fn) fn(e, s_listenerUser);
  if (s_queue && xQueueSend(s_queue, &e, 0) != pdTRUE) s_drops = s_drops + 1;
}

void releaseAll(uint32_t now) {
  for (uint8_t k = 0; k < pg::kNumKeys; ++k) {
    if (isHeld(k)) emit(k, false, 0, now, now);
  }
}

// Reads every pending event. Returns false on an I2C error.
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
  for (;;) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5));
    pollG0();
    if (!s_present) continue;
    if (digitalRead(kPinInt) != LOW) continue;
    const uint32_t now = millis();
    if (backoffUntil && static_cast<int32_t>(now - backoffUntil) < 0) continue;
    backoffUntil = 0;
    if (!drain()) backoffUntil = (now + 100) | 1u;   // bus trouble: do not hammer it
  }
}

bool setupController() {
  // Same writes as Adafruit_TCA8418::begin(), matrix(7, 8), flush(),
  // enableInterrupts() in M5Cardputer 1.1.1 -- plus the overflow interrupt.
  bool ok = true;
  for (uint8_t i = 0; i < 3; ++i) ok = wr(REG_GPIO_DIR_1 + i, 0x00) && ok;       // all inputs
  for (uint8_t i = 0; i < 3; ++i) ok = wr(REG_GPI_EM_1 + i, 0xFF) && ok;         // GPI event mode
  for (uint8_t i = 0; i < 3; ++i) ok = wr(REG_GPIO_INT_LVL_1 + i, 0x00) && ok;   // falling
  for (uint8_t i = 0; i < 3; ++i) ok = wr(REG_GPIO_INT_EN_1 + i, 0xFF) && ok;
  ok = wr(REG_KP_GPIO_1, 0x7F) && ok;                                            // rows 0..6 in the matrix
  ok = wr(REG_KP_GPIO_1 + 1, 0xFF) && ok;                                        // cols 0..7 in the matrix
  if (!ok) return false;
  uint8_t v = 0;
  for (int n = 0; n < 16 && rd(REG_KEY_EVENT_A, &v) && v != 0; ++n) {}           // flush key events
  for (uint8_t i = 0; i < 3; ++i) rd(REG_GPIO_INT_STAT_1 + i, &v);               // flush GPI status
  wr(REG_INT_STAT, 0x1F);
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
  return st;
}

}  // namespace keys
}  // namespace plat
