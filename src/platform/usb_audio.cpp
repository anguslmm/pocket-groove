// Platform layer: USB composite device (CDC + UAC1 stereo input). See usb_audio.h.
//
// Checked against the installed packages (pioarduino 55.03.312-1):
//  * framework-arduinoespressif32-libs/esp32s3/include/arduino_tinyusb/include/
//    tusb_config.h: CFG_TUD_AUDIO = CONFIG_TINYUSB_AUDIO_ENABLED (= 1 in every
//    esp32s3 sdkconfig.h variant), EP IN + OUT enabled, flow control on
//    (audio_device.h default), no interrupt / feedback EP, EP IN max 776 B,
//    IN FIFO 4 x 776 = 3104 B (static .bss in libarduino_tinyusb.a).
//  * TinyUSB 88c7f0b81 (versions.txt), audio_device.c: audiod_set_interface()
//    clears the IN FIFO when it CLOSES the streaming endpoint (alternate
//    change), opens the new one and queues a zero-length packet WITHOUT
//    calling tud_audio_tx_done_isr(); from then on every ISO IN completion
//    runs audiod_tx_xfer_isr(), which sizes the next packet from the FIFO
//    level (audiod_tx_packet_size) and then calls tud_audio_tx_done_isr().
//  * dcd_dwc2.c handle_incomplete_iso_in(): an ISO IN packet the host did not
//    poll is retried once (iso_retry = bInterval = 1) and then completed with
//    0 bytes and XFER_RESULT_FAILED, which the audio driver treats like any
//    completion: it dequeues the next packet. Before the host starts polling,
//    the FIFO therefore drains at about one packet every 2 ms.
//  * cores/esp32/esp32-hal-tinyusb.c: tinyusb_enable_interface() must run
//    before tinyusb_init() (USB.begin()); the descriptor callback gets the next
//    free interface number and allocates endpoints with
//    tinyusb_get_free_in_endpoint(). tinyusb_init() installs the DWC2
//    interrupt on the calling core.
//  * cores/esp32/USBCDC.cpp: _onLineState() reboots into the bootloader on an
//    RTS / RTS+DTR / DTR / none sequence and _onLineCoding() on 1200 baud, both
//    only while reboot_enable is set. We clear it and handle 1200 baud
//    ourselves (ARDUINO_USB_CDC_LINE_CODING_EVENT), so only that one, explicit
//    request can restart the instrument.
#include "usb_audio.h"

#if PG_USB_AUDIO

#include <Arduino.h>
#include <esp_attr.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <cstring>

#if ARDUINO_USB_MODE
#error "PG_USB_AUDIO needs ARDUINO_USB_MODE=0 (TinyUSB on the USB-OTG controller)"
#endif
#if ARDUINO_USB_CDC_ON_BOOT
#error "PG_USB_AUDIO needs ARDUINO_USB_CDC_ON_BOOT=0: usbaudio::begin() starts the stack once the rate is known"
#endif
#include "USB.h"
#include "USBCDC.h"
#include "esp32-hal-tinyusb.h"

#include "uac1_desc.h"
#include "usb_cdc.h"

USBCDC PgSerial(0);   // usb_cdc.h

namespace plat {
namespace usbaudio {
namespace {

constexpr uint32_t kFrameBytes = pgusb::kFrameBytes;
constexpr int kBlockFrames = 128;   // plat::audio::kBlockFrames (not included: keeps this module standalone)

uint32_t s_rate = 0;
bool s_started = false;

// Written by the descriptor callback (inside USB.begin()).
uint8_t  s_itfAc = 0xFF, s_itfAs = 0xFF, s_epIn = 0;
uint16_t s_maxPacket = 0;

// Stream state. s_alt: tud task; s_mounted / s_suspended: Arduino USB event
// task; s_on: the AND of the three, read by the audio task.
volatile uint8_t s_alt = 0;
volatile bool    s_mounted = false, s_suspended = false;
std::atomic<bool> s_on{false};
tu_fifo_t* volatile s_ff = nullptr;
volatile uint32_t s_hostSetRate = 0, s_opens = 0;
volatile uint16_t s_target = 0;          // FIFO target in bytes (driver threshold, whole frames)
// Phase: Closed / Opening by the tud task, Opening -> Running by the ISR.
std::atomic<uint32_t> s_phase{0};

// ISR counters (single writer: the USB ISR). DRAM only.
DRAM_ATTR volatile uint32_t s_packets = 0, s_framesSent = 0, s_pktZero = 0, s_pktSmall = 0,
                            s_pktNominal = 0, s_pktLarge = 0, s_pktOther = 0;
DRAM_ATTR volatile uint16_t s_nominalIsr = 0;

// Producer counters (single writer: the audio task).
volatile uint32_t s_framesPushed = 0, s_overrunFrames = 0, s_openingDiscards = 0;
const int16_t kSilence[kBlockFrames * 2] = {};   // flash: read by tu_fifo_write_n on the audio task only

// 1200-baud touch.
void (*volatile s_dlFn)(void*) = nullptr;
void* volatile s_dlUser = nullptr;

void updateOn() {
  const bool on = (s_alt != 0) && s_mounted && !s_suspended && s_ff != nullptr;
  s_on.store(on, std::memory_order_release);
}

// Descriptor callback: runs inside tinyusb_init() (USB.begin()). *itf is the
// next free interface number (Arduino's CDC took 0 and 1). Returns the bytes
// written; 0 makes tinyusb_init() fail.
uint16_t loadDescriptor(uint8_t* dst, uint8_t* itf) {
  const uint8_t ep = tinyusb_get_free_in_endpoint();
  if (ep == 0) return 0;
  pgusb::Uac1Params p;
  p.itfAc = *itf;
  p.epIn = static_cast<uint8_t>(0x80 | ep);
  p.strIdx = tinyusb_add_string_descriptor("Pocket Groove");
  p.sampleRate = s_rate;
  const size_t n = pgusb::buildUac1(dst, pgusb::kUac1DescLen, p);
  if (n != pgusb::kUac1DescLen) return 0;
  s_itfAc = p.itfAc;
  s_itfAs = static_cast<uint8_t>(p.itfAc + 1);
  s_epIn = p.epIn;
  s_maxPacket = pgusb::maxPacketBytes(s_rate);
  s_nominalIsr = static_cast<uint16_t>(s_rate / 1000u);
  *itf = static_cast<uint8_t>(*itf + 2);
  return static_cast<uint16_t>(n);
}

void onUsbEvent(void*, esp_event_base_t base, int32_t id, void*) {
  if (base != ARDUINO_USB_EVENTS) return;
  switch (id) {
    case ARDUINO_USB_STARTED_EVENT: s_mounted = true; s_suspended = false; s_alt = 0; break;   // (re)configured: stream closed
    case ARDUINO_USB_STOPPED_EVENT: s_mounted = false; s_alt = 0; break;
    case ARDUINO_USB_SUSPEND_EVENT: s_suspended = true; break;
    case ARDUINO_USB_RESUME_EVENT:  s_suspended = false; break;
    default: break;
  }
  if (s_alt == 0) s_phase.store(static_cast<uint32_t>(Phase::Closed), std::memory_order_release);
  updateOn();
}

// Arduino USB event task. 1200 baud = "reboot into the bootloader, please".
void onCdcEvent(void*, esp_event_base_t base, int32_t id, void* data) {
  if (base != ARDUINO_USB_CDC_EVENTS || id != ARDUINO_USB_CDC_LINE_CODING_EVENT || !data) return;
  const auto* d = static_cast<const arduino_usb_cdc_event_data_t*>(data);
  if (d->line_coding.bit_rate != 1200) return;
  void (*fn)(void*) = s_dlFn;
  if (fn) {
    fn(s_dlUser);
    vTaskDelay(pdMS_TO_TICKS(400));   // the UI thread mutes the codec and reboots meanwhile
  }
  rebootToDownloadMode();
}

}  // namespace

// ---- TinyUSB audio class callbacks (strong versions of the weak stubs) ----
extern "C" {

// SET_INTERFACE on an audio interface, after the driver opened the new
// alternate's endpoint and queued a zero-length packet. The FIFO was cleared
// when the previous alternate's endpoint was closed.
bool tud_audio_set_itf_cb(uint8_t rhport, tusb_control_request_t const* p_request) {
  (void)rhport;
  const uint8_t itf = tu_u16_low(tu_le16toh(p_request->wIndex));
  const uint8_t alt = tu_u16_low(tu_le16toh(p_request->wValue));
  if (itf == s_itfAs) {
    if (alt != 0) {
      s_ff = tud_audio_get_ep_in_ff();
      s_opens = s_opens + 1;
      s_target = static_cast<uint16_t>(tud_audio_get_ep_in_fifo_threshold() / kFrameBytes * kFrameBytes);
      s_phase.store(static_cast<uint32_t>(Phase::Opening), std::memory_order_release);
    } else {
      s_phase.store(static_cast<uint32_t>(Phase::Closed), std::memory_order_release);
    }
    s_alt = alt;
    updateOn();
  }
  return true;
}

// The driver closed the streaming endpoint (alternate -> 0 or a switch) and
// cleared the FIFO.
bool tud_audio_set_itf_close_ep_cb(uint8_t rhport, tusb_control_request_t const* p_request) {
  (void)rhport;
  const uint8_t itf = tu_u16_low(tu_le16toh(p_request->wIndex));
  if (itf == s_itfAs) {
    s_alt = 0;
    s_phase.store(static_cast<uint32_t>(Phase::Closed), std::memory_order_release);
    updateOn();
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

// ISR context, once per completed ISO IN transfer, right after the driver
// queued the next packet from the FIFO. n_bytes_sent is the size of the
// packet that just completed: 0 for a zero-length packet AND for a packet the
// host never polled (dcd_dwc2 completes it as failed, 0 bytes). So the first
// completion with data is the proof that the host is receiving. In IRAM: it
// runs 1000 times a second.
bool IRAM_ATTR tud_audio_tx_done_isr(uint8_t rhport, uint16_t n_bytes_sent, uint8_t func_id, uint8_t ep_in,
                                     uint8_t cur_alt_setting) {
  (void)rhport; (void)func_id; (void)ep_in; (void)cur_alt_setting;
  s_packets = s_packets + 1;
  const uint16_t frames = static_cast<uint16_t>(n_bytes_sent / kFrameBytes);
  s_framesSent = s_framesSent + frames;
  const uint32_t ph = s_phase.load(std::memory_order_relaxed);
  if (frames == 0) {
    if (ph == static_cast<uint32_t>(Phase::Running)) s_pktZero = s_pktZero + 1;
    return true;
  }
  if (ph == static_cast<uint32_t>(Phase::Opening)) s_phase.store(static_cast<uint32_t>(Phase::Running), std::memory_order_release);
  const uint16_t nom = s_nominalIsr;
  if (frames == nom) s_pktNominal = s_pktNominal + 1;
  else if (frames == nom + 1) s_pktLarge = s_pktLarge + 1;
  else if (frames + 1 == nom) s_pktSmall = s_pktSmall + 1;
  else s_pktOther = s_pktOther + 1;
  return true;
}

}  // extern "C"

bool begin(uint32_t rate) {
  if (s_started) return true;
  s_rate = (rate == 32000u) ? 32000u : 44100u;
  tinyusb_enable_interface(USB_INTERFACE_AUDIO, static_cast<uint16_t>(pgusb::kUac1DescLen), loadDescriptor);
  // The serial port. 256 B receive queue: a ~PGX request line is at most
  // 241 bytes and the host sends the next one only after the reply.
  PgSerial.setRxBufferSize(256);
  PgSerial.enableReboot(false);
  PgSerial.onEvent(ARDUINO_USB_CDC_LINE_CODING_EVENT, onCdcEvent);
  PgSerial.begin();
  USB.onEvent(onUsbEvent);
  s_started = USB.begin();
  // The device may already be mounted by the time the event handler exists.
  s_mounted = tud_mounted();
  s_suspended = tud_suspended();
  updateOn();
  return s_started;
}

uint32_t sampleRate() { return s_rate; }

void onDownloadRequest(void (*fn)(void*), void* user) {
  s_dlFn = nullptr;
  s_dlUser = user;
  s_dlFn = fn;
}

void rebootToDownloadMode() {
  usb_persist_restart(RESTART_BOOTLOADER);
  for (;;) vTaskDelay(1);
}

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
  uint32_t room = tu_fifo_remaining(ff) / kFrameBytes;   // frames
  if (s_phase.load(std::memory_order_acquire) != static_cast<uint32_t>(Phase::Running)) {
    // OPENING: the host has not taken a packet yet. Hold the FIFO at the
    // flow-control target (silence below it, at most one block above it), so
    // that whenever the host starts, flow control starts on target, and
    // nothing piles up (or is counted as an overrun) while it has not.
    const uint32_t target = s_target;
    uint32_t level = tu_fifo_count(ff);
    while (level + kFrameBytes <= target && room) {
      uint32_t n = (target - level) / kFrameBytes;
      if (n > static_cast<uint32_t>(kBlockFrames)) n = kBlockFrames;
      if (n > room) n = room;
      tu_fifo_write_n(ff, kSilence, static_cast<uint16_t>(n * kFrameBytes));
      level += n * kFrameBytes;
      room -= n;
    }
    const uint32_t cap = target + static_cast<uint32_t>(frames) * kFrameBytes;
    const uint32_t allow = level < cap ? (cap - level) / kFrameBytes : 0;
    if (allow < room) room = allow;
    uint32_t n = static_cast<uint32_t>(frames);
    if (n > room) {
      s_openingDiscards = s_openingDiscards + (n - room);
      n = room;
    }
    if (n) {
      tu_fifo_write_n(ff, lr, static_cast<uint16_t>(n * kFrameBytes));
      s_framesPushed = s_framesPushed + n;
    }
    return true;
  }
  uint32_t n = static_cast<uint32_t>(frames);
  if (n > room) {
    s_overrunFrames = s_overrunFrames + (n - room);
    n = room;
  }
  if (n) {
    tu_fifo_write_n(ff, lr, static_cast<uint16_t>(n * kFrameBytes));
    s_framesPushed = s_framesPushed + n;
  }
  return true;
}

void IRAM_ATTR pushAndFoldToMono(int16_t* lr, int frames) {
  push(lr, frames);
  for (int i = 0; i < frames; ++i) {
    const int32_t m = (static_cast<int32_t>(lr[2 * i]) + lr[2 * i + 1]) / 2;
    lr[2 * i] = lr[2 * i + 1] = static_cast<int16_t>(m);
  }
}

Stats stats() {
  Stats s{};
  s.usbCompiled = true;
  s.started = s_started;
  s.rate = s_rate;
  if (!s_started) return s;
  s.mounted = tud_mounted();
  s.suspended = tud_suspended();
  s.streaming = s_on.load(std::memory_order_acquire);
  s.phase = static_cast<Phase>(s_phase.load(std::memory_order_acquire));
  s.cdcOpen = tud_cdc_connected();
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
  s.openingDiscards = s_openingDiscards;
  tu_fifo_t* ff = s_ff;
  if (ff) {
    s.fifoLevel = tu_fifo_count(ff);
    s.fifoDepth = tu_fifo_depth(ff);
    s.fifoTarget = s_target;
  }
  return s;
}

const uint8_t* deviceDescriptor(size_t* len) {
  const uint8_t* d = s_started ? tud_descriptor_device_cb() : nullptr;
  if (len) *len = d ? d[0] : 0;
  return d;
}

const uint8_t* configDescriptor(size_t* len) {
  const uint8_t* d = s_started ? tud_descriptor_configuration_cb(0) : nullptr;
  if (len) *len = d ? static_cast<size_t>(d[2] | (d[3] << 8)) : 0;
  return d;
}

}  // namespace usbaudio
}  // namespace plat

#elif USBSPIKE_NO_USB
// ============================================================================
// Stub build (env:usb-audio-spike-nousb): same interface, no TinyUSB.
// ============================================================================
#include <esp_system.h>
#include <soc/rtc_cntl_reg.h>
#include <soc/soc.h>

namespace plat {
namespace usbaudio {
namespace {
uint32_t s_rate = 44100;
}
bool begin(uint32_t rate) { s_rate = rate; return false; }
uint32_t sampleRate() { return s_rate; }
void onDownloadRequest(void (*)(void*), void*) {}
void rebootToDownloadMode() {
  REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
  esp_restart();
}
bool push(const int16_t*, int) { return false; }
void pushAndFoldToMono(int16_t* lr, int frames) {
  for (int i = 0; i < frames; ++i) {
    const int32_t m = (static_cast<int32_t>(lr[2 * i]) + lr[2 * i + 1]) / 2;
    lr[2 * i] = lr[2 * i + 1] = static_cast<int16_t>(m);
  }
}
Stats stats() {
  Stats s{};
  s.rate = s_rate;
  return s;
}
const uint8_t* deviceDescriptor(size_t* len) { if (len) *len = 0; return nullptr; }
const uint8_t* configDescriptor(size_t* len) { if (len) *len = 0; return nullptr; }
}  // namespace usbaudio
}  // namespace plat

#endif  // PG_USB_AUDIO / USBSPIKE_NO_USB
