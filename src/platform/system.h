// Pocket Groove platform layer -- system services (board bring-up, time, serial,
// memory, chip facts, battery, internal I2C bus).
//
// PLATFORM LAYER RULE: headers under src/platform/ contain no Arduino / M5 /
// ESP-IDF types, so the code that uses them (src/app, src/smoke) stays portable
// and can be linked against a desktop mock. Only the .cpp files here include
// <M5Unified.h>, Arduino or ESP-IDF headers.
#pragma once
#include <cstddef>
#include <cstdint>

namespace plat {
namespace sys {

struct BootInfo {
  bool        displayOk;    // an LCD panel was detected and initialised
  bool        isCardputerAdv;
  int         boardId;      // m5::board_t value (24 = M5CardputerADV in M5GFX 0.2.36)
  const char* boardName;
};

// First call of every firmware: brings up M5Unified (board autodetect, LCD,
// power/ADC, internal I2C on GPIO 8/9) and the USB serial port. Does NOT start
// audio, keys or storage. Never blocks for long and never aborts.
BootInfo begin(uint32_t serialBaud = 115200);
const BootInfo& bootInfo();

// ---- time ------------------------------------------------------------------
uint32_t millis();
uint64_t micros();
void     delayMs(uint32_t ms);
uint32_t cycles();          // CPU cycle counter of the calling core (wraps every ~17.9 s at 240 MHz)
uint32_t cyclesPerUs();     // 240 at the default clock
int      currentCore();     // core the caller runs on

// ---- tasks -----------------------------------------------------------------
// Runs fn(user) once on a new FreeRTOS task, which deletes itself afterwards.
// For blocking work (file tests, bus scans) that must not freeze the UI loop.
// Defaults: core 1 (with the UI, away from audio), priority 1 (= UI loop).
// Returns false if the task could not be created (fn is then not called).
bool runTask(void (*fn)(void*), void* user, const char* name,
             uint32_t stackBytes = 6144, int priority = 1, int core = 1);

// ---- USB serial ---------------------------------------------------------------
// The chip's USB-Serial/JTAG CDC port, or in the USB audio build (PG_USB_AUDIO)
// the TinyUSB CDC port of plat::usbaudio (which starts it).
// write() never stalls the caller for long: with no host listening the data is
// dropped (and writes are skipped for a while) instead of blocking.
void serialWrite(const char* text);
void serialWrite(const char* data, size_t len);
int  serialRead();          // -1 when nothing is waiting
bool serialHostListening();

// ---- USB audio build only (PG_USB_AUDIO; not defined in the other envs) ------
// The TinyUSB port disappears at every reset and the host re-enumerates it, so
// a terminal always connects after the boot log was printed. Everything
// serialWrite() sends from begin() until bootLogEnd() is kept (up to 3 KB, in
// RTC slow memory: no heap) and bootLogReplay() prints it again.
void bootLogEnd();
void bootLogReplay();
// A crash backtrace goes to UART0 / the USB-Serial/JTAG console, neither of
// which reaches the computer in this build, but the core dump is saved to
// flash. Writes a one-line summary of it ("[crash] ...": task, PC, cause,
// backtrace for addr2line) into buf; returns 0 if there is none.
int crashSummary(char* buf, size_t cap);
// Restart and open the boot menu (the serial 'm' of the other builds only
// works in the first 300 ms after a reset, before a host can reconnect).
void restartIntoBootMenu();
bool bootMenuRequested();   // true once after such a restart

// ---- chip / memory ---------------------------------------------------------
struct ChipInfo {
  char     model[20];
  uint8_t  revMajor, revMinor, cores;
  uint32_t cpuMhz;
  uint32_t flashBytes;
  bool     psramFound;      // the Arduino core probes for PSRAM at every boot
  uint32_t psramBytes;
  char     idfVersion[24];
  char     coreVersion[16];
  char     resetReason[16];
};
void chipInfo(ChipInfo* out);

struct HeapInfo {
  uint32_t freeBytes;       // internal RAM, byte-addressable
  uint32_t largestBlock;
  uint32_t minEverFree;
  uint32_t totalBytes;
};
HeapInfo heap();
// Static internal RAM of this image (.data + .bss + .noinit in DRAM), from
// the linker symbols, and the heap the allocator was given at boot.
uint32_t staticDramBytes();
// Unused stack of the calling task, in bytes, since it started.
uint32_t stackFreeMinSelf();

// ---- tasks and CPU load (FreeRTOS run-time stats, esp_timer microseconds) ----
struct TaskRow {
  char     name[20];
  int8_t   core;            // pinned core, -1 = either
  uint8_t  prio;
  uint32_t stackFree;       // bytes never used since the task started (high-water mark)
  uint32_t stackSize;       // bytes, 0 = not known here (the caller knows its own tasks')
  uint32_t runUs;           // run time since the previous taskTable() call (first call: since boot)
};
// Every task that exists now, sorted by name; returns how many were written (at
// most cap). Each call measures run times from the previous call (*elapsedUs =
// the wall time between the two). Not real-time: walks the scheduler's lists.
int taskTable(TaskRow* rows, int cap, uint32_t* elapsedUs);
// Time the idle task of `core` has run since boot (microseconds): the share of a
// stretch NOT spent in it is that core's load.
uint64_t idleRunUs(int core);

// ---- reset cause -------------------------------------------------------------
// true if the previous run ended in a crash, a watchdog reset or a brownout
// (anything the firmware did not ask for): panic, int/task/other WDT, brownout.
bool resetWasAbnormal();
void restart();
void* allocInternal(size_t bytes);   // internal RAM, nullptr on failure (never aborts)
void  freeInternal(void* p);

// ---- battery ---------------------------------------------------------------
struct Battery {
  int percent;      // 0..100, -1 if unknown
  int millivolts;   // -1 if unknown
};
Battery battery();

// ---- internal I2C bus (codec 0x18, keyboard 0x34, IMU) ----------------------
// Thread-safe per register access (M5GFX's i2c layer takes a per-port mutex
// around each transaction). Never call these from the audio task.
bool i2cProbe(uint8_t addr);
int  i2cScan(uint8_t* found, int cap);          // returns the number of devices found
bool i2cRead8(uint8_t addr, uint8_t reg, uint8_t* value, uint32_t hz = 100000);
bool i2cWrite8(uint8_t addr, uint8_t reg, uint8_t value, uint32_t hz = 100000);
const char* imuName();                           // what M5Unified detected at boot

}  // namespace sys
}  // namespace plat
