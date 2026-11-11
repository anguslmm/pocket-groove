// USB audio spike -- stereo 16-bit UAC1 input function on top of the TinyUSB
// stack that arduino-esp32 3.3.12 ships prebuilt (CFG_TUD_AUDIO = 1), next to
// Arduino's own CDC serial port (composite device).
//
// How it hooks in (no IDF rebuild, no framework patch):
//  * A static constructor registers the descriptor block with
//    tinyusb_enable_interface(USB_INTERFACE_AUDIO, ...), the same way the core's
//    USBCDC / the USB library's USBAudioCard do. The core assembles the
//    configuration descriptor from all registered blocks in tinyusb_init(),
//    which main.cpp runs (USB.begin()) before setup().
//  * The class-specific callbacks (tud_audio_set_itf_cb, ...) are TU_ATTR_WEAK
//    in the prebuilt audio_device.c; this file provides the strong ones. The
//    USB *library* (USBAudioCard.cpp, which defines the same callbacks) is not
//    linked: nothing here includes its headers.
//
// Threads:
//  * push() is for the audio task only (one producer). It never blocks, never
//    allocates, and with no stream open it is one atomic load.
//  * The consumer is TinyUSB's ISO-IN completion ISR (audiod_xfer_isr), which
//    takes 1 ms worth of frames out of the driver's FIFO per USB frame, sized
//    by the driver's flow control (44 or 45 frames at 44.1 kHz).
//  * Everything else: UI/loop task.
//
// Build switch: -DUSBSPIKE_NO_USB=1 compiles a stub (Serial on the hardware
// USB-Serial/JTAG port, no TinyUSB) -- the RAM/flash baseline.
//
// No Arduino / TinyUSB types in this header.
#pragma once
#include <cstddef>
#include <cstdint>

namespace usbaudio {

struct Stats {
  bool     usbCompiled;     // false in the no-USB baseline build
  bool     mounted;         // host configured the device (SET_CONFIGURATION)
  bool     suspended;       // bus suspended (also what an unplug looks like: no VBUS sensing)
  bool     streaming;       // host selected the streaming alternate setting and the bus is up
  uint32_t rate;            // the one rate in the descriptor (= the I2S rate)
  uint32_t hostSetRate;     // last SET_CUR sampling frequency from the host (0 = never sent)
  uint32_t streamOpens;     // alt 0 -> 1 transitions
  uint8_t  itfAc, epIn;     // where the core put us
  uint16_t maxPacket;       // wMaxPacketSize in the descriptor
  // ISO IN packets as they complete (counted in the ISR):
  uint32_t packets;         // all completed IN packets (one per 1 ms frame while streaming)
  uint32_t framesSent;      // audio frames in them
  uint32_t pktZero;         // empty packets after data had started = USB-side underruns
  uint32_t pktSmall;        // floor(rate/1000) - 1 frames (flow control slowing down; 48k-type rates)
  uint32_t pktNominal;      // floor(rate/1000) frames (44 at 44.1 kHz, 32 at 32 kHz)
  uint32_t pktLarge;        // floor(rate/1000) + 1 frames (45 at 44.1 kHz)
  uint32_t pktOther;        // any other size (should stay 0)
  // Producer side (audio task):
  uint32_t framesPushed;    // frames accepted into the FIFO
  uint32_t overrunFrames;   // frames dropped because the FIFO was full (host not reading)
  uint16_t fifoLevel;       // bytes in the driver FIFO now
  uint16_t fifoDepth;       // its size (CFG_TUD_AUDIO_FUNC_1_EP_IN_SW_BUF_SZ)
  uint16_t fifoTarget;      // flow-control target level (driver default: depth / 2)
};

// Sample rate for this boot: chosen before the descriptor is built (static
// init), from a value kept in RTC memory across esp_restart() (requestRate),
// else the build default (-DUSBSPIKE_RATE, 44100).
uint32_t sampleRate();
// Stores `hz` (44100 or 32000) for the next boot and restarts the chip. The
// host sees a disconnect and a new device with the new descriptor.
void requestRateAndRestart(uint32_t hz);
// Restart into the ROM serial download mode (no G0 button needed). USB mode:
// the core switches the PHY back to the USB-Serial/JTAG controller first
// (usb_persist_restart(RESTART_BOOTLOADER)), so the ROM appears as 303a:1001.
void rebootToDownloadMode();

// UI thread, once after boot: event hooks (suspend / resume / unmount).
void begin();
// Audio task: hand over interleaved stereo frames (L,R,...). Returns false
// (and costs one atomic load) when no stream is open. Drops what does not fit.
bool push(const int16_t* interleavedStereo, int frames);

Stats stats();
// The bytes the device answers GET_DESCRIPTOR with (nullptr in the baseline).
const uint8_t* deviceDescriptor(size_t* len);
const uint8_t* configDescriptor(size_t* len);

}  // namespace usbaudio
