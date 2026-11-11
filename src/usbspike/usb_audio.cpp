// USB audio spike -- see usb_audio.h.
//
// Checked against the installed packages (pioarduino 55.03.312-1):
//  * framework-arduinoespressif32-libs/esp32s3/include/arduino_tinyusb/include/
//    tusb_config.h: CFG_TUD_AUDIO = CONFIG_TINYUSB_AUDIO_ENABLED (= 1 in every
//    esp32s3 sdkconfig.h variant), EP IN + OUT enabled, flow control on
//    (audio_device.h default), no interrupt / feedback EP, EP IN max 776 B,
//    IN FIFO 4 x 776 = 3104 B (static .bss in libarduino_tinyusb.a).
//  * TinyUSB 88c7f0b81 (versions.txt), audio_device.c: the IN path runs in
//    the ISO completion ISR: audiod_xfer_isr -> audiod_tx_xfer_isr picks the
//    packet size from the FIFO level (audiod_tx_packet_size) and calls
//    tud_audio_tx_done_isr() after queueing the next packet.
//  * cores/esp32/esp32-hal-tinyusb.c: tinyusb_enable_interface() must run
//    before tinyusb_init(); the descriptor callback gets the next free
//    interface number and allocates endpoints with tinyusb_get_free_in_endpoint().
#include "usb_audio.h"

#include <Arduino.h>
#include <esp_attr.h>
#include <esp_system.h>
#include <soc/rtc_cntl_reg.h>

#include <atomic>
#include <cstring>

#include "uac1_desc.h"

#ifndef USBSPIKE_RATE
#define USBSPIKE_RATE 44100
#endif

#if !USBSPIKE_NO_USB
#if ARDUINO_USB_MODE
#error "env:usb-audio-spike needs ARDUINO_USB_MODE=0 (TinyUSB on the USB-OTG controller)"
#endif
#include "USB.h"
#include "esp32-hal-tinyusb.h"
#endif

namespace usbaudio {
namespace {

// ---- rate choice, kept across esp_restart() --------------------------------
constexpr uint32_t kRtcMagic = 0x55415331u;   // "UAS1"
RTC_NOINIT_ATTR uint32_t s_rtcMagic;
RTC_NOINIT_ATTR uint32_t s_rtcRate;

uint32_t bootRate() {
  if (s_rtcMagic == kRtcMagic && (s_rtcRate == 44100u || s_rtcRate == 32000u)) return s_rtcRate;
  return USBSPIKE_RATE;
}

uint32_t s_rate = 0;   // set by the static constructor below

}  // namespace

uint32_t sampleRate() { return s_rate ? s_rate : bootRate(); }

void requestRateAndRestart(uint32_t hz) {
  s_rtcMagic = kRtcMagic;
  s_rtcRate = (hz == 32000u) ? 32000u : 44100u;
  delay(50);
  esp_restart();
}

#if USBSPIKE_NO_USB
// ============================================================================
// Baseline: no TinyUSB. Same interface, nothing behind it.
// ============================================================================
namespace {
struct RateInit { RateInit() { s_rate = bootRate(); } } s_rateInit;
}  // namespace

void rebootToDownloadMode() {
  REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
  esp_restart();
}
void begin() {}
bool push(const int16_t*, int) { return false; }
Stats stats() {
  Stats s{};
  s.usbCompiled = false;
  s.rate = sampleRate();
  return s;
}
const uint8_t* deviceDescriptor(size_t* len) { if (len) *len = 0; return nullptr; }
const uint8_t* configDescriptor(size_t* len) { if (len) *len = 0; return nullptr; }

#else
// ============================================================================
// TinyUSB build
// ============================================================================
namespace {

// Written by the descriptor callback (main task, before setup()).
uint8_t  s_itfAc = 0xFF, s_itfAs = 0xFF, s_epIn = 0;
uint16_t s_maxPacket = 0;
uint16_t s_nominalFrames = 0;  // floor(rate / 1000)

// Stream state. s_alt: tud task; s_mounted / s_suspended: Arduino USB event
// task; s_on: the AND of the three, read by the audio task.
volatile uint8_t s_alt = 0;
volatile bool    s_mounted = false, s_suspended = false;
std::atomic<bool> s_on{false};
tu_fifo_t* volatile s_ff = nullptr;
volatile uint32_t s_hostSetRate = 0, s_opens = 0;

// ISR counters (single writer: the USB ISR). DRAM only.
DRAM_ATTR volatile uint32_t s_packets = 0, s_framesSent = 0, s_pktZero = 0, s_pktSmall = 0,
                            s_pktNominal = 0, s_pktLarge = 0, s_pktOther = 0;
DRAM_ATTR volatile bool s_hadData = false;
DRAM_ATTR volatile uint16_t s_nominalIsr = 0;

// Producer counters (single writer: the audio task).
volatile uint32_t s_framesPushed = 0, s_overrunFrames = 0;

// Prefill: when a stream opens, the first push() first puts this many bytes
// of silence in the FIFO, i.e. starts it at the flow-control target (the
// driver's threshold). Without it the FIFO starts empty and, at 44.1 kHz,
// only climbs to the target by the 0.1 frame/ms surplus (~4 s), with less
// than one block of margin meanwhile: host/usbspike/check_usb_descriptors.cpp
// shows the resulting empty packet(s) right after stream start.
std::atomic<uint32_t> s_prefillPending{0};   // 4-byte: exchange() is a call to newlib's IRAM __atomic_exchange_4 (S32C1I); a bool needs the slower 1-byte emulation
volatile uint16_t s_prefillBytes = 0;
const int16_t kSilence[128 * 2] = {};

void updateOn() {
  const bool on = (s_alt != 0) && s_mounted && !s_suspended && s_ff != nullptr;
  s_on.store(on, std::memory_order_release);
}

// Descriptor callback: runs inside tinyusb_init() (USB.begin() in main.cpp,
// before setup()). *itf is the next free interface number (Arduino's CDC took
// 0 and 1). Returns the bytes written; 0 makes tinyusb_init() fail.
uint16_t loadDescriptor(uint8_t* dst, uint8_t* itf) {
  const uint8_t ep = tinyusb_get_free_in_endpoint();
  if (ep == 0) return 0;
  usbspike::Uac1Params p;
  p.itfAc = *itf;
  p.epIn = static_cast<uint8_t>(0x80 | ep);
  p.strIdx = tinyusb_add_string_descriptor("Pocket Groove");
  p.sampleRate = s_rate;
  const size_t n = usbspike::buildUac1(dst, usbspike::kUac1DescLen, p);
  if (n != usbspike::kUac1DescLen) return 0;
  s_itfAc = p.itfAc;
  s_itfAs = static_cast<uint8_t>(p.itfAc + 1);
  s_epIn = p.epIn;
  s_maxPacket = usbspike::maxPacketBytes(s_rate);
  s_nominalFrames = static_cast<uint16_t>(s_rate / 1000u);
  s_nominalIsr = s_nominalFrames;
  *itf = static_cast<uint8_t>(*itf + 2);
  return static_cast<uint16_t>(n);
}

// Static registration, like USBCDC's constructor. Only touches the core's
// zero-initialised C statics, so constructor order does not matter.
struct Registrar {
  Registrar() {
    s_rate = bootRate();
    tinyusb_enable_interface(USB_INTERFACE_AUDIO, static_cast<uint16_t>(usbspike::kUac1DescLen), loadDescriptor);
  }
} s_registrar;

void onUsbEvent(void*, esp_event_base_t base, int32_t id, void*) {
  if (base != ARDUINO_USB_EVENTS) return;
  switch (id) {
    case ARDUINO_USB_STARTED_EVENT: s_mounted = true; s_suspended = false; s_alt = 0; break;   // (re)configured: stream closed
    case ARDUINO_USB_STOPPED_EVENT: s_mounted = false; s_alt = 0; break;
    case ARDUINO_USB_SUSPEND_EVENT: s_suspended = true; break;
    case ARDUINO_USB_RESUME_EVENT:  s_suspended = false; break;
    default: break;
  }
  updateOn();
}

}  // namespace

// ---- TinyUSB audio class callbacks (strong versions of the weak stubs) ----
extern "C" {

// SET_INTERFACE on any audio interface, after the driver opened the new
// alternate's endpoint (and, for alt != 0, queued a zero-length packet).
bool tud_audio_set_itf_cb(uint8_t rhport, tusb_control_request_t const* p_request) {
  (void)rhport;
  const uint8_t itf = tu_u16_low(tu_le16toh(p_request->wIndex));
  const uint8_t alt = tu_u16_low(tu_le16toh(p_request->wValue));
  if (itf == s_itfAs) {
    if (alt != 0) {
      s_ff = tud_audio_get_ep_in_ff();   // cleared by the driver just before this call
      s_hadData = false;
      s_opens = s_opens + 1;
      s_prefillBytes = static_cast<uint16_t>(tud_audio_get_ep_in_fifo_threshold() / usbspike::kFrameBytes * usbspike::kFrameBytes);
      s_prefillPending.store(1u, std::memory_order_release);
    }
    s_alt = alt;
    updateOn();
  }
  return true;
}

// The driver closed the streaming endpoint (alt -> 0 or switch); FIFO cleared.
bool tud_audio_set_itf_close_ep_cb(uint8_t rhport, tusb_control_request_t const* p_request) {
  (void)rhport;
  const uint8_t itf = tu_u16_low(tu_le16toh(p_request->wIndex));
  if (itf == s_itfAs) {
    s_alt = 0;
    updateOn();
    s_hadData = false;
  }
  return true;
}

// Endpoint SET requests: UAC1 Sampling Frequency Control (audio10 5.2.3.2.3.1).
// Only one rate is advertised, so the host can only set that one; remember it.
bool tud_audio_set_req_ep_cb(uint8_t rhport, tusb_control_request_t const* p_request, uint8_t* pBuff) {
  (void)rhport;
  const uint8_t ctrlSel = TU_U16_HIGH(p_request->wValue);
  if (ctrlSel == AUDIO10_EP_CTRL_SAMPLING_FREQ && p_request->bRequest == AUDIO10_CS_REQ_SET_CUR && p_request->wLength == 3) {
    s_hostSetRate = tu_unaligned_read32(pBuff) & 0x00FFFFFFu;
    return true;
  }
  return false;   // stall anything else
}

// Endpoint GET requests: current / min / max sampling frequency = our rate.
bool tud_audio_get_req_ep_cb(uint8_t rhport, tusb_control_request_t const* p_request) {
  const uint8_t ctrlSel = TU_U16_HIGH(p_request->wValue);
  if (ctrlSel == AUDIO10_EP_CTRL_SAMPLING_FREQ &&
      (p_request->bRequest == AUDIO10_CS_REQ_GET_CUR || p_request->bRequest == AUDIO10_CS_REQ_GET_MIN ||
       p_request->bRequest == AUDIO10_CS_REQ_GET_MAX)) {
    uint8_t freq[3] = {static_cast<uint8_t>(s_rate & 0xFF), static_cast<uint8_t>((s_rate >> 8) & 0xFF),
                       static_cast<uint8_t>((s_rate >> 16) & 0xFF)};
    return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, freq, sizeof(freq));
  }
  return false;
}

// ISR context, once per completed ISO IN packet (every 1 ms frame while the
// stream is open), right after the driver queued the next packet from the
// FIFO. n_bytes_sent is the size of the packet that just went out. In IRAM:
// it runs 1000 times a second and must not cost instruction-cache misses.
bool IRAM_ATTR tud_audio_tx_done_isr(uint8_t rhport, uint16_t n_bytes_sent, uint8_t func_id, uint8_t ep_in,
                                     uint8_t cur_alt_setting) {
  (void)rhport; (void)func_id; (void)ep_in; (void)cur_alt_setting;
  s_packets = s_packets + 1;
  const uint16_t frames = static_cast<uint16_t>(n_bytes_sent / usbspike::kFrameBytes);
  s_framesSent = s_framesSent + frames;
  const uint16_t nom = s_nominalIsr;
  if (frames == 0) {
    if (s_hadData) s_pktZero = s_pktZero + 1;
  } else {
    s_hadData = true;
    if (frames == nom) s_pktNominal = s_pktNominal + 1;
    else if (frames == nom + 1) s_pktLarge = s_pktLarge + 1;
    else if (frames + 1 == nom) s_pktSmall = s_pktSmall + 1;
    else s_pktOther = s_pktOther + 1;
  }
  return true;
}

}  // extern "C"

void begin() {
  USB.onEvent(onUsbEvent);
  // The device may already be mounted by now (enumeration starts before setup()).
  s_mounted = tud_mounted();
  s_suspended = tud_suspended();
  updateOn();
}

void rebootToDownloadMode() { usb_persist_restart(RESTART_BOOTLOADER); }

// Audio task. The FIFO is TinyUSB's own (single producer = us, single
// consumer = the ISO ISR; tu_fifo's indices are volatile and lock-free, and
// no mutex is configured for it). The driver configured it "overwritable",
// so we must never write more than tu_fifo_remaining(): a write that wraps
// would move the read side under the ISR's feet. Frames that do not fit are
// dropped and counted.
bool IRAM_ATTR push(const int16_t* lr, int frames) {
  if (!s_on.load(std::memory_order_acquire)) return false;
  tu_fifo_t* ff = s_ff;
  if (!ff || frames <= 0) return false;
  if (s_prefillPending.exchange(0u, std::memory_order_acq_rel)) {
    uint16_t left = s_prefillBytes;
    while (left) {
      const uint16_t room = static_cast<uint16_t>(tu_fifo_remaining(ff) / usbspike::kFrameBytes * usbspike::kFrameBytes);
      uint16_t n = left < sizeof(kSilence) ? left : static_cast<uint16_t>(sizeof(kSilence));
      if (n > room) n = room;
      if (!n) break;
      tu_fifo_write_n(ff, kSilence, n);
      left = static_cast<uint16_t>(left - n);
    }
  }
  const uint16_t room = static_cast<uint16_t>(tu_fifo_remaining(ff) / usbspike::kFrameBytes);
  uint16_t n = static_cast<uint16_t>(frames);
  if (n > room) {
    s_overrunFrames = s_overrunFrames + (n - room);
    n = room;
  }
  if (n) {
    tu_fifo_write_n(ff, lr, static_cast<uint16_t>(n * usbspike::kFrameBytes));
    s_framesPushed = s_framesPushed + n;
  }
  return true;
}

Stats stats() {
  Stats s{};
  s.usbCompiled = true;
  s.mounted = tud_mounted();
  s.suspended = tud_suspended();
  s.streaming = s_on.load(std::memory_order_acquire);
  s.rate = s_rate;
  s.hostSetRate = s_hostSetRate;
  s.streamOpens = s_opens;
  s.itfAc = s_itfAc;
  s.epIn = s_epIn;
  s.maxPacket = s_maxPacket;
  s.packets = s_packets;
  s.framesSent = s_framesSent;
  s.pktZero = s_pktZero;
  s.pktSmall = s_pktSmall;
  s.pktNominal = s_pktNominal;
  s.pktLarge = s_pktLarge;
  s.pktOther = s_pktOther;
  s.framesPushed = s_framesPushed;
  s.overrunFrames = s_overrunFrames;
  tu_fifo_t* ff = s_ff;
  if (ff) {
    s.fifoLevel = tu_fifo_count(ff);
    s.fifoDepth = tu_fifo_depth(ff);
    s.fifoTarget = tud_audio_get_ep_in_fifo_threshold();
  }
  return s;
}

const uint8_t* deviceDescriptor(size_t* len) {
  const uint8_t* d = tud_descriptor_device_cb();
  if (len) *len = d ? d[0] : 0;
  return d;
}

const uint8_t* configDescriptor(size_t* len) {
  const uint8_t* d = tud_descriptor_configuration_cb(0);
  if (len) *len = d ? static_cast<size_t>(d[2] | (d[3] << 8)) : 0;
  return d;
}

#endif  // USBSPIKE_NO_USB

}  // namespace usbaudio
