// Pocket Groove platform layer -- the USB port as a composite device: a CDC
// serial port (the console and the ~PGX project transfer) plus a stereo
// 16-bit UAC1 audio INPUT called "Pocket Groove" (env:cardputer-adv-usb and
// env:usb-audio-spike; docs/usb-audio.md).
//
// Built on the TinyUSB that arduino-esp32 3.3.12 ships prebuilt (the audio
// class is compiled in): the UAC1 descriptor block is registered with the
// core's composite-descriptor builder the way its USBCDC does, and the TinyUSB
// audio callbacks (weak in the library) are defined here. Needs
// ARDUINO_USB_MODE=0 (the PHY goes to the USB-OTG controller, not to the
// USB-Serial/JTAG one) and ARDUINO_USB_CDC_ON_BOOT=0: begin() starts the USB
// stack itself, once the sample rate is known (it is in the descriptor).
//
// Threads:
//  * begin(), stats(), descriptors: UI thread.
//  * push() / pushAndFoldToMono(): the audio task only (the one producer).
//    They never block or allocate; with no stream open they cost one atomic
//    load.
//  * The consumer is TinyUSB's ISO-IN completion ISR (once per 1 ms USB
//    frame), which takes 44 or 45 frames (at 44.1 kHz) out of the driver's
//    own FIFO.
//
// Stream start (the spike's "148 frames dropped" one-off): after the host
// selects the streaming alternate setting it can take tens of ms before it
// polls the endpoint. Meanwhile the DWC2 driver retires each unpolled ISO
// packet as a failed transfer every second frame and dequeues the next one,
// i.e. the FIFO drains at about half the rate it fills. The spike prefilled
// the FIFO to its target at the open and then pushed everything, so after
// ~12-20 ms the FIFO was full and the rest of each block was dropped and
// counted as an overrun. Now the stream stays in an OPENING phase until the
// first packet with data has really gone out (tx_done_isr with
// n_bytes_sent > 0: a missed frame completes with 0 bytes): in that phase the
// FIFO is kept at the flow-control target (excess frames are discarded
// silently; nobody hears them), and only from the RUNNING phase on is a full
// FIFO an overrun. host/usbspike/check_usb_descriptors.cpp simulates both.
//
// Build switches: PG_USB_AUDIO=1 compiles the real thing; USBSPIKE_NO_USB=1
// compiles stubs (the spike's no-USB RAM baseline); otherwise this module is
// empty (all other envs). PG_USB_TRIM=0 turns off the linker diet in
// usb_trim.c.
//
// No Arduino / TinyUSB types in this header.
#pragma once
#include <cstddef>
#include <cstdint>

namespace plat {
namespace usbaudio {

enum class Phase : uint8_t {
  Closed = 0,    // no streaming alternate setting selected (or bus down)
  Opening = 1,   // alternate 1 selected, the host has not taken data yet
  Running = 2,   // the host is receiving packets
};

struct Stats {
  bool     usbCompiled;     // false in the stub build
  bool     started;         // begin() succeeded (USB stack installed)
  bool     mounted;         // host configured the device (SET_CONFIGURATION)
  bool     suspended;       // bus suspended (also what an unplug looks like: no VBUS sensing)
  bool     streaming;       // streaming alternate selected and the bus is up (push() is live)
  Phase    phase;
  bool     cdcOpen;         // a terminal holds DTR on the serial port
  uint32_t rate;            // the one rate in the descriptor (= the I2S rate)
  uint32_t hostSetRate;     // last SET_CUR sampling frequency from the host (0 = never sent)
  uint32_t streamOpens;     // alternate 0 -> 1 transitions
  uint8_t  itfAc, epIn;     // where the core put us
  uint16_t maxPacket;       // wMaxPacketSize in the descriptor
  // ISO IN packets as they complete (counted in the ISR):
  uint32_t packets;         // completed IN transfers (one per 1-2 ms frame while the alternate is open)
  uint32_t framesSent;      // audio frames in them
  uint32_t pktZero;         // empty packets while RUNNING = USB-side underruns (or a missed host poll)
  uint32_t pktSmall;        // floor(rate/1000) - 1 frames
  uint32_t pktNominal;      // floor(rate/1000) frames (44 at 44.1 kHz, 32 at 32 kHz)
  uint32_t pktLarge;        // floor(rate/1000) + 1 frames (45 at 44.1 kHz)
  uint32_t pktOther;        // any other size (should stay 0)
  // Producer side (audio task):
  uint32_t framesPushed;    // frames accepted into the FIFO
  uint32_t overrunFrames;   // frames dropped while RUNNING because the FIFO was full
  uint32_t openingDiscards; // frames not queued while OPENING (above the target, nobody listening yet)
  uint16_t fifoLevel;       // bytes in the driver FIFO now
  uint16_t fifoDepth;       // its size (CFG_TUD_AUDIO_FUNC_1_EP_IN_SW_BUF_SZ)
  uint16_t fifoTarget;      // flow-control target level (driver default: depth / 2)
};

// UI thread, once: registers the audio function (descriptor for `sampleRate`,
// 44100 or 32000), starts the CDC port and the USB stack (USB.begin()). The
// USB interrupt is allocated on the calling core (core 1 from setup(): away
// from the audio task on core 0). Returns false if the stack did not start.
// Turns off the core's DTR/RTS "reset to bootloader" sequence on the CDC port
// (a terminal must never reboot the instrument) and handles the 1200-baud
// touch that `pio run -t upload` uses itself: see onDownloadRequest().
bool begin(uint32_t sampleRate);
uint32_t sampleRate();

// The host opened the CDC port at 1200 baud (PlatformIO's upload "touch"):
// `fn(user)` runs on the USB event task and should arrange
// rebootToDownloadMode() soon (e.g. on the UI thread, after muting the
// codec). If it has not happened 400 ms later, the event task reboots by
// itself. Without a handler the reboot is immediate.
void onDownloadRequest(void (*fn)(void*), void* user);
// Restart into the ROM serial download mode (no button needed): the core
// switches the PHY back to the USB-Serial/JTAG controller first, so the ROM
// appears as 303a:1001 and esptool can flash. Does not return.
void rebootToDownloadMode();

// Audio task: hand over interleaved stereo frames (L,R,...). Returns false
// (one atomic load) when no stream is open. Drops what does not fit.
bool push(const int16_t* interleavedStereo, int frames);
// Audio task: push(), then replace every frame with the mono sum (L+R)/2 in
// both slots (what the mono codec plays). |L|,|R| <= 32767 so the sum cannot
// clip. In IRAM.
void pushAndFoldToMono(int16_t* interleavedStereo, int frames);

Stats stats();
// The bytes the device answers GET_DESCRIPTOR with (nullptr in the stub build
// or before begin()).
const uint8_t* deviceDescriptor(size_t* len);
const uint8_t* configDescriptor(size_t* len);

}  // namespace usbaudio
}  // namespace plat
