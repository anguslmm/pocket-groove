// The Seed's USB port as a serial port (libDaisy's CDC device on the internal USB FS
// PHY: VID 0x0483, PID 0x5740, "Daisy Seed Built In"; on a Mac /dev/cu.usbmodem*).
// Receive: libDaisy calls back from the USB interrupt with each packet; the bytes go
// into a ring the main loop drains. Transmit: a ring the main loop fills; flush()
// hands it to the USB stack in packets of up to kChunk bytes, double-buffered (the
// stack sends from the buffer it was given until the transfer completes, so a
// buffer is only refilled once the next transmit has been accepted, i.e. the
// previous one is done). Nothing ever waits for the computer: with no terminal
// open the ring fills and further output is dropped (counted).
// Main loop only (receive callback: interrupt).
#pragma once
#include <cstddef>
#include <cstdint>

namespace pgdev {
namespace usbserial {

constexpr uint32_t kChunk = 512;

// rx / tx: ring memory (powers of two).
void begin(uint8_t* rxMem, uint32_t rxBytes, uint8_t* txMem, uint32_t txBytes);
int read();                       // a byte, or -1
void write(const char* p, size_t n);
size_t room();
void flush();
bool configured();                // a computer has set the device up (enumerated)
uint32_t rxCount();               // bytes received since boot (the USB interrupt counts them)

struct Stats {
  uint32_t rxBytes, rxDropped, txBytes, txDropped, txPackets, txBusy;
};
Stats stats();

}  // namespace usbserial
}  // namespace pgdev
