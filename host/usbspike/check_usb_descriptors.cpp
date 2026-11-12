// Desktop check of the USB audio function's descriptors (src/platform/
// usb_audio.cpp, used by env:cardputer-adv-usb and env:usb-audio-spike) and of
// TinyUSB's IN flow control, without hardware.
//
// Build and run (from the repository root; uses the TinyUSB headers that
// PlatformIO installed for the firmware build, for Arduino's CDC block):
//
//   sh host/usbspike/run_checks.sh                  # builds this, runs all offline checks
//   sh host/usbspike/run_checks.sh --dump LOG       # also checks a 'd' dump from the device
//
// (The script is one c++ command against the TinyUSB headers in
// ~/.platformio/packages/framework-arduinoespressif32-libs; see it for flags.)
//
// What it does:
//  1. Rebuilds the configuration descriptor exactly as the firmware assembles
//    it at run time (esp32-hal-tinyusb.c tinyusb_load_enabled_interfaces():
//    9-byte config header + Arduino's CDC block from TinyUSB's
//    TUD_CDC_DESCRIPTOR with USBCDC.cpp's arguments + our UAC1 block from
//    src/platform/uac1_desc.h with the endpoint the core's allocator hands out),
//    for 44100 and 32000 Hz, prints it decoded field by field, and runs the
//    validator (src/platform/usb_desc_check.h) over it.
//  2. Mutation tests: breaks one field at a time and requires the validator to
//    catch each one (so a PASS means something).
//  3. Simulates TinyUSB's ISO-IN flow control (audiod_calc_tx_packet_sz +
//    audiod_tx_packet_size, copied from audio_device.c @ 88c7f0b81, MIT) with
//    the driver's FIFO (3104 B, threshold depth/2), a producer that delivers
//    128-frame blocks at the I2S rate (with clock error and scheduling jitter),
//    and a consumer that takes one packet per 1 ms USB frame. Checks packet
//    sizes (44/45 frames at 44.1 kHz), that no empty packet or FIFO overflow
//    happens once running, and that the long-run rate equals the I2S rate.
//  4. Stream start: the host polls the endpoint some time after it selected
//    the streaming alternate setting, and until then the DWC2 port retires an
//    unpolled packet every 2 ms. Shows that the spike's open sequence
//    overflows the FIFO once (its measured "148 frames dropped") and that the
//    fixed one (usb_audio.cpp, OPENING phase) never does, for any delay.
//  5. --dump FILE: parses "[usb] cfg xxxx: .." lines printed by the 'd'
//    (spike) or 'u' (instrument) serial command and validates the bytes the
//    device really sends.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "tusb_option.h"
#include "common/tusb_common.h"
#include "common/tusb_types.h"
#include "class/cdc/cdc.h"
#include "device/usbd.h"

#include "uac1_desc.h"
#include "usb_desc_check.h"

namespace {

int g_fail = 0;
void expect(bool ok, const char* what) {
  std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++g_fail;
}

void printLine(const char* line, void*) { std::printf("    %s\n", line); }
void quiet(const char*, void*) {}

// ---------------------------------------------------------------------------
// 1. Reconstruct the configuration descriptor
// ---------------------------------------------------------------------------
// esp32-hal-tinyusb.c tinyusb_get_free_in_endpoint(), with the endpoints
// USBCDC's tinyusb_enable_interface() reserved (OUT 3, IN 4, IN 5).
uint8_t freeInEndpoint(uint16_t& inMask, uint16_t outMask) {
  const int kNumInEps = 5;   // CFG_TUD_NUM_IN_EPS (esp32-hal-tinyusb.h)
  for (int i = 1; i <= kNumInEps; ++i)
    if (!(inMask & (1u << i)) && (outMask & (1u << i))) { inMask |= static_cast<uint16_t>(1u << i); return static_cast<uint8_t>(i); }
  for (int i = 1; i <= kNumInEps; ++i)
    if (!(inMask & (1u << i))) { inMask |= static_cast<uint16_t>(1u << i); return static_cast<uint8_t>(i); }
  return 0;
}

std::vector<uint8_t> buildConfiguration(uint32_t rate) {
  // String indices as the core hands them out: 0 language, 1 manufacturer,
  // 2 product, 3 serial (tinyusb_string_descriptor_len starts at 4), then in
  // interface-enum order: CDC "TinyUSB CDC" = 4, our "Pocket Groove" = 5,
  // the configuration's "TinyUSB Device" = 6.
  const uint8_t cdc[] = {TUD_CDC_DESCRIPTOR(0, 4, 0x85, 64, 0x03, 0x84, 64)};   // USBCDC.cpp load_cdc_descriptor
  uint16_t inMask = (1u << 4) | (1u << 5), outMask = (1u << 3);
  const uint8_t ep = freeInEndpoint(inMask, outMask);
  pgusb::Uac1Params p;
  p.itfAc = 2;
  p.epIn = static_cast<uint8_t>(0x80 | ep);
  p.strIdx = 5;
  p.sampleRate = rate;
  uint8_t audio[pgusb::kUac1DescLen];
  const size_t an = pgusb::buildUac1(audio, sizeof(audio), p);
  const uint16_t total = static_cast<uint16_t>(9 + sizeof(cdc) + an);
  // TINYUSB_CONFIG_DEFAULT / ESPUSB: self-powered attribute, 500 mA.
  const uint8_t hdr[] = {TUD_CONFIG_DESCRIPTOR(1, 4, 6, total, TUSB_DESC_CONFIG_ATT_SELF_POWERED, 500)};
  std::vector<uint8_t> v(hdr, hdr + sizeof(hdr));
  v.insert(v.end(), cdc, cdc + sizeof(cdc));
  v.insert(v.end(), audio, audio + an);
  return v;
}

// ---------------------------------------------------------------------------
// Decoder (lsusb -v style), independent of the builder
// ---------------------------------------------------------------------------
unsigned u16(const uint8_t* p) { return static_cast<unsigned>(p[0] | (p[1] << 8)); }
unsigned u24(const uint8_t* p) { return static_cast<unsigned>(p[0] | (p[1] << 8) | (p[2] << 16)); }

void decode(const std::vector<uint8_t>& d) {
  uint8_t cls = 0, sub = 0;
  for (size_t o = 0; o + 2 <= d.size() && d[o] >= 2; o += d[o]) {
    const uint8_t* p = &d[o];
    std::printf("  %04zx  ", o);
    for (int i = 0; i < p[0]; ++i) std::printf("%02x ", p[i]);
    std::printf("\n        ");
    switch (p[1]) {
      case 0x02: std::printf("CONFIGURATION wTotalLength %u bNumInterfaces %u bConfigurationValue %u iConfiguration %u bmAttributes 0x%02x bMaxPower %u mA", u16(p + 2), p[4], p[5], p[6], p[7], p[8] * 2); break;
      case 0x0B: std::printf("INTERFACE ASSOCIATION first %u count %u class %02x/%02x/%02x", p[2], p[3], p[4], p[5], p[6]); break;
      case 0x04:
        cls = p[5]; sub = p[6];
        std::printf("INTERFACE %u alt %u endpoints %u class %02x/%02x/%02x iInterface %u%s", p[2], p[3], p[4], p[5], p[6], p[7], p[8],
                    cls == 1 && sub == 1 ? "  (AudioControl)" : cls == 1 && sub == 2 ? "  (AudioStreaming)" : cls == 2 ? "  (CDC control)" : cls == 0x0A ? "  (CDC data)" : "");
        break;
      case 0x05: {
        static const char* xt[] = {"control", "isochronous", "bulk", "interrupt"};
        static const char* st[] = {"none", "asynchronous", "adaptive", "synchronous"};
        std::printf("ENDPOINT 0x%02x %s %s, sync %s, wMaxPacketSize %u, bInterval %u", p[2], (p[2] & 0x80) ? "IN" : "OUT", xt[p[3] & 3], st[(p[3] >> 2) & 3], u16(p + 4), p[6]);
        if (p[0] == 9) std::printf(", bRefresh %u bSynchAddress 0x%02x", p[7], p[8]);
        break;
      }
      case 0x24:
        if (cls == 1 && sub == 1) {
          if (p[2] == 1) std::printf("AC HEADER bcdADC %x.%02x wTotalLength %u bInCollection %u baInterfaceNr %u", p[4], p[3], u16(p + 5), p[7], p[8]);
          else if (p[2] == 2) std::printf("INPUT TERMINAL id %u type 0x%04x assoc %u channels %u config 0x%04x", p[3], u16(p + 4), p[6], p[7], u16(p + 8));
          else if (p[2] == 3) std::printf("OUTPUT TERMINAL id %u type 0x%04x assoc %u source %u", p[3], u16(p + 4), p[6], p[7]);
          else std::printf("AC subtype 0x%02x", p[2]);
        } else if (cls == 1 && sub == 2) {
          if (p[2] == 1) std::printf("AS GENERAL terminal link %u delay %u format tag 0x%04x", p[3], p[4], u16(p + 5));
          else if (p[2] == 2) {
            std::printf("FORMAT TYPE %u channels %u subframe %u B bits %u rates %u:", p[3], p[4], p[5], p[6], p[7]);
            for (int k = 0; k < p[7]; ++k) std::printf(" %u Hz", u24(p + 8 + 3 * k));
          } else std::printf("AS subtype 0x%02x", p[2]);
        } else {
          std::printf("CS_INTERFACE (CDC functional) subtype 0x%02x", p[2]);
        }
        break;
      case 0x25: std::printf("CS ENDPOINT subtype %u bmAttributes 0x%02x lock delay units %u delay %u", p[2], p[3], p[4], u16(p + 5)); break;
      default: std::printf("type 0x%02x", p[1]);
    }
    std::printf("\n");
  }
}

// ---------------------------------------------------------------------------
// 2. Mutation tests
// ---------------------------------------------------------------------------
size_t findAudioEp(const std::vector<uint8_t>& d) {
  for (size_t o = 9; o < d.size(); o += d[o])
    if (d[o + 1] == 0x05 && (d[o + 3] & 3) == 1) return o;
  return 0;
}
size_t findAcInterface(const std::vector<uint8_t>& d) {
  for (size_t o = 9; o < d.size(); o += d[o])
    if (d[o + 1] == 0x04 && d[o + 5] == 1 && d[o + 6] == 1) return o;
  return 0;
}
// nth class-specific descriptor of `type`/`subtype` inside the audio function
// (CDC's functional descriptors are CS_INTERFACE too, with overlapping subtypes)
size_t findCs(const std::vector<uint8_t>& d, uint8_t type, uint8_t subtype, int nth = 0) {
  for (size_t o = findAcInterface(d); o < d.size(); o += d[o])
    if (d[o + 1] == type && d[o + 2] == subtype && nth-- == 0) return o;
  return 0;
}

void mutationTests(uint32_t rate) {
  struct M { const char* what; void (*apply)(std::vector<uint8_t>&); };
  static const M ms[] = {
      {"wTotalLength off by one", [](std::vector<uint8_t>& d) { d[2]++; }},
      {"bNumInterfaces 3", [](std::vector<uint8_t>& d) { d[4] = 3; }},
      {"data EP sync type NONE (driver would treat it as feedback EP)", [](std::vector<uint8_t>& d) { d[findAudioEp(d) + 3] = 0x01; }},
      {"data EP OUT instead of IN", [](std::vector<uint8_t>& d) { d[findAudioEp(d) + 2] &= 0x7F; }},
      {"wMaxPacketSize one frame below TinyUSB's flow-control maximum", [](std::vector<uint8_t>& d) {
         const size_t e = findAudioEp(d), f = findCs(d, 0x24, 0x02, 1);
         const unsigned rate = static_cast<unsigned>(d[f + 8] | (d[f + 9] << 8) | (d[f + 10] << 16));
         const unsigned bad = (rate / 1000 + 1) * 4 - 4;
         d[e + 4] = static_cast<uint8_t>(bad);
         d[e + 5] = static_cast<uint8_t>(bad >> 8); }},
      {"bInterval 4", [](std::vector<uint8_t>& d) { d[findAudioEp(d) + 6] = 4; }},
      {"AC interface with one endpoint", [](std::vector<uint8_t>& d) { d[findAcInterface(d) + 4] = 1; }},
      {"AC header wTotalLength wrong", [](std::vector<uint8_t>& d) { d[findCs(d, 0x24, 0x01) + 5]++; }},
      {"AS_GENERAL links the input terminal", [](std::vector<uint8_t>& d) { size_t o = findCs(d, 0x24, 0x01, 1); d[o + 3] = pgusb::kEntityIt; }},
      {"format: 1 channel vs 2-channel terminal", [](std::vector<uint8_t>& d) { d[findCs(d, 0x24, 0x02, 1) + 4] = 1; }},
      {"format: 24-bit resolution in a 2-byte subframe", [](std::vector<uint8_t>& d) { d[findCs(d, 0x24, 0x02, 1) + 6] = 24; }},
      {"format: tSamFreq 48000 (not the running rate)", [](std::vector<uint8_t>& d) { size_t f = findCs(d, 0x24, 0x02, 1); d[f + 8] = 0x80; d[f + 9] = 0xBB; d[f + 10] = 0; }},
      {"input terminal is USB streaming (a speaker path)", [](std::vector<uint8_t>& d) { size_t o = findCs(d, 0x24, 0x02, 0); d[o + 4] = 0x01; d[o + 5] = 0x01; }},
      {"AC baInterfaceNr points at the CDC data interface", [](std::vector<uint8_t>& d) { d[findCs(d, 0x24, 0x01, 0) + 8] = 1; }},
      {"CS endpoint descriptor replaced by another type", [](std::vector<uint8_t>& d) { size_t e = findAudioEp(d); d[e + d[e] + 1] = 0x24; }},
      {"output terminal source does not exist", [](std::vector<uint8_t>& d) { d[findCs(d, 0x24, 0x03) + 7] = 9; }},
      {"output terminal not USB streaming", [](std::vector<uint8_t>& d) { size_t o = findCs(d, 0x24, 0x03); d[o + 4] = 0x01; d[o + 5] = 0x03; }},
      {"AS alt 0 with an endpoint", [](std::vector<uint8_t>& d) { size_t a = findAcInterface(d); for (size_t o = a; o < d.size(); o += d[o]) if (d[o + 1] == 0x04 && d[o + 5] == 1 && d[o + 6] == 2 && d[o + 3] == 0) { d[o + 4] = 1; break; } }},
      {"duplicate entity ID", [](std::vector<uint8_t>& d) { d[findCs(d, 0x24, 0x03) + 3] = pgusb::kEntityIt; }},
  };
  for (const M& m : ms) {
    std::vector<uint8_t> d = buildConfiguration(rate);
    m.apply(d);
    const int e = pgusb::checkConfiguration(d.data(), d.size(), rate, nullptr, quiet, nullptr);
    char what[160];
    std::snprintf(what, sizeof(what), "mutation caught (%u Hz): %s", static_cast<unsigned>(rate), m.what);
    expect(e > 0, what);
  }
}

// ---------------------------------------------------------------------------
// 3. Flow-control simulation (TinyUSB audio_device.c @ 88c7f0b81, MIT)
// ---------------------------------------------------------------------------
struct FlowControl {
  uint16_t packet_sz_tx[3] = {0, 0, 0};
  int ctrl_blackout = 0;   // a function-static in the driver
  void calc(uint32_t sample_rate_tx, uint8_t n_channels_tx, uint8_t n_bytes_per_sample_tx, uint8_t interval_tx) {
    const uint8_t interval = interval_tx;   // full speed
    const uint16_t sample_normimal = static_cast<uint16_t>(sample_rate_tx * interval / 1000);
    const uint16_t sample_reminder = static_cast<uint16_t>(sample_rate_tx * interval % 1000);
    const uint16_t min = static_cast<uint16_t>((sample_normimal - 1) * n_channels_tx * n_bytes_per_sample_tx);
    const uint16_t norm = static_cast<uint16_t>(sample_normimal * n_channels_tx * n_bytes_per_sample_tx);
    const uint16_t max = static_cast<uint16_t>((sample_normimal + 1) * n_channels_tx * n_bytes_per_sample_tx);
    if (sample_reminder) { packet_sz_tx[0] = norm; packet_sz_tx[1] = norm; packet_sz_tx[2] = max; }
    else { packet_sz_tx[0] = min; packet_sz_tx[1] = norm; packet_sz_tx[2] = max; }
  }
  uint16_t packetSize(uint16_t data_count, uint16_t fifo_depth, uint16_t fifo_threshold, uint16_t max_depth) {
    const uint16_t* nominal_size = packet_sz_tx;
    if (nominal_size[1] && nominal_size[1] * 4 <= fifo_depth) {
      uint16_t packet_size;
      uint16_t slot_size = static_cast<uint16_t>(nominal_size[2] - nominal_size[1]);
      if (data_count < nominal_size[0]) {
        packet_size = 0;
      } else if (data_count < (fifo_threshold - slot_size) && !ctrl_blackout) {
        packet_size = nominal_size[0];
        ctrl_blackout = 10;
      } else if (data_count > (fifo_threshold + slot_size) && !ctrl_blackout) {
        packet_size = nominal_size[2];
        ctrl_blackout = (nominal_size[0] == nominal_size[1]) ? 0 : 10;
      } else {
        packet_size = nominal_size[1];
        if (ctrl_blackout) ctrl_blackout--;
      }
      return packet_size < max_depth ? packet_size : max_depth;
    }
    return data_count < max_depth ? data_count : max_depth;
  }
};

struct SimResult {
  long packets = 0, zeroAfterStart = 0, overflowFrames = 0, other = 0;
  long zeroFirst10s = 0;
  long openingDiscards = 0;        // frames the producer did not queue while OPENING (fixed producer only)
  long bySize[4] = {0, 0, 0, 0};   // nominal-1, nominal, nominal+1, other (steady state, t >= 10 s)
  long steadyPackets = 0;
  double steadyFrames = 0, framesIn = 0;
  int minLevel = 1 << 30, maxLevel = 0;   // FIFO level in steady state
  int levelAtHostStart = -1;              // FIFO level when the first packet reached the host
};

// Producer behaviour at stream start.
enum class Producer {
  Spike,   // the spike: at the first block after the open, prefill silence to the target, then push
           // every block; a full FIFO is an overrun from the open on
  Fixed,   // src/platform/usb_audio.cpp: OPENING until the first packet WITH DATA completes (the host
           // is receiving), meanwhile hold the FIFO at the target (silence below it, at most one
           // block above it, the excess discarded silently); RUNNING after that
};

struct SimOpts {
  uint32_t rate = 44100;
  double ppm = 0;            // I2S clock error vs. the host's USB frame clock
  double jitterMs = 0;       // uniform random extra delay of each block delivery (audio task scheduling)
  double seconds = 120;
  double hostStartMs = 0;    // SET_INTERFACE at t = 0; the host polls the endpoint from this time on
  Producer producer = Producer::Fixed;
  unsigned seed = 1234u;
};

// The consumer side, as the driver and the DWC2 port do it (audio_device.c,
// dcd_dwc2.c @ 88c7f0b81):
//  * SET_INTERFACE (t = 0) clears the FIFO, opens the endpoint and queues a
//    zero-length packet; it does not call tud_audio_tx_done_isr.
//  * Each ISO IN transfer that COMPLETES runs audiod_tx_xfer_isr: the next
//    packet is sized by flow control and taken out of the FIFO, then
//    tud_audio_tx_done_isr(n_bytes_sent of the completed packet) runs.
//  * Before the host polls, a queued packet is retried once (iso_retry =
//    bInterval = 1) and then completed as failed with 0 bytes: one packet is
//    dequeued (and lost) every 2 ms. From hostStartMs on, every queued packet
//    goes out in the next frame: one completion per 1 ms with its real size.
SimResult simulate(const SimOpts& o, uint16_t mps) {
  const uint16_t kDepth = 3104, kThreshold = kDepth / 2;   // CFG_TUD_AUDIO_FUNC_1_EP_IN_SW_BUF_SZ, default threshold
  const int kFrameBytes = 4, kBlock = 128;
  const int target = kThreshold / kFrameBytes * kFrameBytes;   // usb_audio.cpp s_target
  FlowControl fc;
  fc.calc(o.rate, 2, 2, 1);
  std::mt19937 rng(o.seed);
  std::uniform_real_distribution<double> jit(0.0, o.jitterMs);
  std::uniform_real_distribution<double> phase(0.0, kBlock * 1000.0 / o.rate);
  const double blockMs = kBlock * 1000.0 / (o.rate * (1.0 + o.ppm * 1e-6));
  double nextBlockIdeal = phase(rng);       // the audio task's block grid is unrelated to the open
  double nextBlockAt = nextBlockIdeal + jit(rng);
  int level = 0;                            // bytes in the FIFO
  bool started = false;                     // a packet with data has reached the host
  bool running = false;                     // Fixed: the producer's phase
  bool spikePrefillDone = false;
  int queued = 0;                           // bytes of the packet the hardware holds (the ZLP at open)
  int retry = 1;                            // iso_retry of the queued packet
  SimResult r;
  const uint16_t nom = static_cast<uint16_t>(o.rate / 1000);
  for (long ms = 0; ms < static_cast<long>(o.seconds * 1000.0); ++ms) {
    // deliver every block whose time has come before this USB frame
    while (nextBlockAt <= static_cast<double>(ms)) {
      int n = kBlock;
      if (o.producer == Producer::Spike) {
        if (!spikePrefillDone) {
          spikePrefillDone = true;
          const int fill = std::min(target, (kDepth - level) / kFrameBytes * kFrameBytes);
          level += fill;
        }
        const int room = (kDepth - level) / kFrameBytes;
        n = std::min(kBlock, room);
        r.overflowFrames += kBlock - n;
      } else if (!running) {
        int room = (kDepth - level) / kFrameBytes;
        while (level + kFrameBytes <= target && room) {   // silence up to the target
          int s = std::min((target - level) / kFrameBytes, std::min(kBlock, room));
          level += s * kFrameBytes;
          room -= s;
        }
        const int cap = target + kBlock * kFrameBytes;
        const int allow = level < cap ? (cap - level) / kFrameBytes : 0;
        n = std::min(kBlock, std::min(room, allow));
        r.openingDiscards += kBlock - n;
      } else {
        const int room = (kDepth - level) / kFrameBytes;
        n = std::min(kBlock, room);
        r.overflowFrames += kBlock - n;
      }
      level += n * kFrameBytes;
      r.framesIn += n;
      nextBlockIdeal += blockMs;
      nextBlockAt = nextBlockIdeal + jit(rng);
    }
    // end of USB frame `ms`: does the queued packet complete, and with what?
    int sent = -1;   // -1: nothing completes in this frame
    if (static_cast<double>(ms) >= o.hostStartMs) {
      sent = queued;                       // the host took it
    } else if (retry > 0) {
      --retry;                             // missed: retried in the next frame
    } else {
      sent = 0;                            // missed twice: completed as failed, 0 bytes (data lost)
    }
    if (sent < 0) continue;
    // audiod_tx_xfer_isr: size and dequeue the next packet, then tx_done_isr(sent)
    const uint16_t sz = fc.packetSize(static_cast<uint16_t>(level), kDepth, kThreshold, mps);
    level -= sz;
    queued = sz;
    retry = 1;
    const int frames = sent / kFrameBytes;
    if (frames && !running) running = true;            // tud_audio_tx_done_isr: OPENING -> RUNNING
    if (static_cast<double>(ms) < o.hostStartMs) continue;   // a failed completion: nothing reached the host
    ++r.packets;
    if (frames) {
      if (!started) r.levelAtHostStart = level + sz;
      started = true;
    } else if (started) {
      ++r.zeroAfterStart;
      if (ms < 10000) ++r.zeroFirst10s;
    }
    if (ms >= 10000) {   // steady state: the FIFO has settled at the flow-control target
      ++r.steadyPackets;
      r.steadyFrames += frames;
      if (frames == nom - 1) ++r.bySize[0];
      else if (frames == nom) ++r.bySize[1];
      else if (frames == nom + 1) ++r.bySize[2];
      else ++r.bySize[3];
      if (level < r.minLevel) r.minLevel = level;
      if (level > r.maxLevel) r.maxLevel = level;
    }
  }
  return r;
}

void flowControlTests() {
  struct Case { uint32_t rate; double ppm; double jitter; };
  const Case cases[] = {
      {44100, 0, 0}, {44100, +100, 1.0}, {44100, -100, 1.0}, {44100, +1000, 2.0}, {44100, -1000, 2.0},
      {32000, 0, 0}, {32000, +250, 1.0}, {32000, -250, 1.0},
  };
  for (const Case& c : cases) {
    const uint16_t mps = pgusb::maxPacketBytes(c.rate);
    SimOpts o;
    o.rate = c.rate; o.ppm = c.ppm; o.jitterMs = c.jitter;
    SimResult r = simulate(o, mps);
    const double measured = r.steadyFrames / static_cast<double>(r.steadyPackets) * 1000.0;   // frames per host second
    const double expectRate = c.rate * (1.0 + c.ppm * 1e-6);
    const double rateErrPpm = (measured / expectRate - 1.0) * 1e6;
    std::printf("  %5u Hz %+6.0f ppm jitter %.1f ms: steady sizes %u:%ld %u:%ld %u:%ld other:%ld, empty %ld, "
                "overflow %ld frames, FIFO %d..%d B, delivered %.2f f/s (%+.1f ppm vs I2S)\n",
                static_cast<unsigned>(c.rate), c.ppm, c.jitter, c.rate / 1000 - 1, r.bySize[0], c.rate / 1000, r.bySize[1],
                c.rate / 1000 + 1, r.bySize[2], r.bySize[3], r.zeroAfterStart, r.overflowFrames, r.minLevel, r.maxLevel,
                measured, rateErrPpm);
    char what[200];
    std::snprintf(what, sizeof(what), "flow control %u Hz %+.0f ppm, jitter %.1f ms: no empty packet, no overflow, only N-1/N/N+1 sizes, "
                  "long-run rate = I2S rate within 10 ppm", static_cast<unsigned>(c.rate), c.ppm, c.jitter);
    expect(r.zeroAfterStart == 0 && r.overflowFrames == 0 && r.bySize[3] == 0 && std::fabs(rateErrPpm) < 10.0, what);
  }
  // How late may a block be (audio task preempted / stalled) before the host
  // gets an empty packet? Informational: a uniform random delay of 0..J ms on
  // every block, 60 s each.
  std::printf("  producer jitter tolerance at 44.1 kHz: ");
  for (double j : {2.0, 4.0, 6.0, 8.0, 10.0, 12.0}) {
    SimOpts o;
    o.jitterMs = j; o.seconds = 60; o.seed = 77u;
    SimResult s = simulate(o, pgusb::maxPacketBytes(44100));
    std::printf("%.0f ms: %ld empty  ", j, s.zeroAfterStart);
  }
  std::printf("\n");
  // The 44.1 kHz cadence the host sees with a perfect clock: 9 x 44 + 1 x 45 per 10 ms.
  {
    SimOpts o;
    o.seconds = 60; o.seed = 1u;
    SimResult r = simulate(o, pgusb::maxPacketBytes(44100));
    const double share45 = static_cast<double>(r.bySize[2]) / static_cast<double>(r.bySize[1] + r.bySize[2]);
    std::printf("  44.1 kHz ideal clock: %.3f %% of packets carry 45 frames (expected 10 %%), %ld carry 43\n", share45 * 100.0, r.bySize[0]);
    expect(std::fabs(share45 - 0.10) < 0.0005 && r.bySize[0] == 0, "44.1 kHz cadence is 44/45 frames with 10 % of 45-frame packets");
  }
}

// Stream start: the spike measured "Overruns 148 frames dropped" once per
// stream open, never growing afterwards. The host selects the streaming
// alternate setting some ms before it polls the endpoint; until then the
// DWC2 port retires one unpolled packet every 2 ms (above), so the FIFO
// fills at about half the rate the audio task writes. The spike prefilled to
// the target at the open and pushed everything: the FIFO ran full ~12 ms
// later. The fixed producer waits for the first packet the host received.
void streamStartTests() {
  std::printf("  host start delay after SET_INTERFACE (ms):   spike producer: overrun frames    fixed producer: overrun / empty / "
              "start discards / FIFO at host start\n");
  double reproduces = -1;
  long spikeAt30 = 0;
  for (uint32_t rate : {44100u, 32000u}) {
    const uint16_t mps = pgusb::maxPacketBytes(rate);
    bool allClean = true;
    for (double d : {0.0, 1.0, 3.0, 6.0, 10.0, 12.0, 15.0, 18.0, 20.0, 25.0, 30.0, 50.0, 100.0, 250.0, 1000.0, 5000.0}) {
      long spikeOver = 0, fixedOver = 0, fixedZero = 0, fixedDisc = 0;
      int lvl = 0;
      // several phases of the audio task's block grid against the open
      for (unsigned seed = 1; seed <= 8; ++seed) {
        SimOpts o;
        o.rate = rate; o.seconds = 20; o.hostStartMs = d; o.seed = seed; o.jitterMs = 0.5;
        o.producer = Producer::Spike;
        const SimResult s = simulate(o, mps);
        o.producer = Producer::Fixed;
        const SimResult f = simulate(o, mps);
        spikeOver = std::max(spikeOver, s.overflowFrames);
        fixedOver = std::max(fixedOver, f.overflowFrames);
        fixedZero = std::max(fixedZero, f.zeroAfterStart);
        fixedDisc = std::max(fixedDisc, f.openingDiscards);
        lvl = std::max(lvl, f.levelAtHostStart);
      }
      std::printf("  %5u Hz %6.0f ms:%34ld%25ld / %ld / %ld / %d B\n", static_cast<unsigned>(rate), d, spikeOver, fixedOver,
                  fixedZero, fixedDisc, lvl);
      if (fixedOver || fixedZero) allClean = false;
      if (rate == 44100u && reproduces < 0 && spikeOver >= 148) reproduces = d;
      if (rate == 44100u && d == 30.0) spikeAt30 = spikeOver;
    }
    char what[200];
    std::snprintf(what, sizeof(what), "%u Hz stream start, host polling 0..5000 ms after SET_INTERFACE: fixed producer has no overrun "
                  "and no empty packet", static_cast<unsigned>(rate));
    expect(allClean, what);
  }
  std::printf("  the spike's sequence drops >= 148 frames once the host starts %.0f ms after the open\n", reproduces);
  expect(spikeAt30 > 0 && reproduces > 0, "the spike's start-of-stream overrun reproduces in the model (one-off, at the open)");
}

// ---------------------------------------------------------------------------
// 4. Device dump
// ---------------------------------------------------------------------------
bool readDump(const char* path, std::vector<uint8_t>& out, uint32_t& rate) {
  FILE* f = std::fopen(path, "r");
  if (!f) { std::perror(path); return false; }
  char line[512];
  rate = 0;
  while (std::fgets(line, sizeof(line), f)) {
    const char* p = std::strstr(line, "[usb] cfg ");
    if (p) {
      p = std::strchr(p + 10, ':');
      if (!p) continue;
      ++p;
      unsigned v;
      int used;
      while (std::sscanf(p, " %2x%n", &v, &used) == 1) { out.push_back(static_cast<uint8_t>(v)); p += used; }
    }
    const char* r = std::strstr(line, " rate ");
    unsigned hz;
    if (r && std::strstr(line, "USB audio spike") && std::sscanf(r, " rate %u", &hz) == 1) rate = hz;
    const char* u = std::strstr(line, "[usb] rate ");   // the instrument's 'u' dump
    if (u && std::sscanf(u, "[usb] rate %u", &hz) == 1) rate = hz;
  }
  std::fclose(f);
  return !out.empty();
}

}  // namespace

int main(int argc, char** argv) {
  const char* dump = nullptr;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--dump") && i + 1 < argc) dump = argv[++i];
    else { std::fprintf(stderr, "usage: %s [--dump device-log.txt]\n", argv[0]); return 2; }
  }

  for (uint32_t rate : {44100u, 32000u}) {
    std::vector<uint8_t> cfg = buildConfiguration(rate);
    std::printf("== configuration descriptor as the device assembles it, %u Hz (%zu bytes)\n", static_cast<unsigned>(rate), cfg.size());
    decode(cfg);
    std::printf("  validator:\n");
    pgusb::CheckResult r;
    const int e = pgusb::checkConfiguration(cfg.data(), cfg.size(), rate, &r, printLine, nullptr, true);
    char what[200];
    std::snprintf(what, sizeof(what), "%u Hz configuration: 0 errors (%d warnings), audio EP 0x%02x max packet %u B, %d IN EPs, DWC2 FIFO %d/256 words",
                  static_cast<unsigned>(rate), r.warnings, r.audioEpIn, r.audioMaxPacket, r.inEndpoints, r.fifoWordsUsed);
    expect(e == 0 && r.warnings == 0, what);
    expect(cfg.size() == 9 + TUD_CDC_DESC_LEN + pgusb::kUac1DescLen, "total length = 9 + 66 (CDC) + 91 (UAC1) = 166 bytes");
    expect(r.audioEpIn == 0x83, "audio endpoint is 0x83 (first IN slot whose OUT twin the CDC uses, as tinyusb_get_free_in_endpoint picks)");
  }

  std::printf("== mutation tests\n");
  mutationTests(44100);
  mutationTests(32000);

  std::printf("== TinyUSB ISO-IN flow control simulation (FIFO 3104 B, threshold 1552 B, 128-frame producer)\n");
  flowControlTests();

  std::printf("== stream start: host polling late after SET_INTERFACE (the spike's one-off overrun)\n");
  streamStartTests();

  if (dump) {
    std::vector<uint8_t> cfg;
    uint32_t rate = 0;
    std::printf("== device dump %s\n", dump);
    if (!readDump(dump, cfg, rate)) { expect(false, "dump contains '[usb] cfg' lines"); }
    else {
      decode(cfg);
      pgusb::CheckResult r;
      const int e = pgusb::checkConfiguration(cfg.data(), cfg.size(), rate, &r, printLine, nullptr, true);
      expect(e == 0, "configuration descriptor from the device validates");
      if (rate) {
        const std::vector<uint8_t> ref = buildConfiguration(rate);
        expect(ref == cfg, "device bytes are identical to the offline reconstruction");
      }
    }
  }

  std::printf("\n%s: %d failure(s)\n", g_fail ? "FAILED" : "ALL CHECKS PASSED", g_fail);
  return g_fail ? 1 : 0;
}
