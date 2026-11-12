// USB audio spike -- byte-level validator for a full-speed configuration
// descriptor that carries the UAC1 stereo input function of uac1_desc.h
// (plus whatever else is in the configuration, e.g. Arduino's CDC).
//
// Portable (no Arduino / TinyUSB / ESP-IDF headers). Used twice:
//  * on the device, over the bytes TinyUSB really sends
//    (tud_descriptor_configuration_cb), at boot and on the 'd' command;
//  * on the desktop by host/usbspike/check_usb_descriptors.cpp, over a
//    reconstruction of the configuration and over a hex dump from the device.
//
// What it checks:
//  1. USB 2.0 chapter 9 framing: bLength/bDescriptorType of every descriptor,
//     wTotalLength, bNumInterfaces, contiguous interface numbers, alternate
//     settings 0..n, bNumEndpoints against the endpoint descriptors that
//     follow, unique endpoint addresses, IAD ranges, full-speed packet limits.
//  2. UAC1 (audio10.pdf 4.3-4.6, frmts10.pdf 2.2.5): AC header bcdADC and
//     wTotalLength, bInCollection / baInterfaceNr, unique entity IDs, source
//     IDs that exist, terminal types, channel cluster, AS alt 0 without
//     endpoints, AS_GENERAL terminal link and PCM tag, Type I format fields and
//     the sample-rate field, the isochronous endpoint (direction, async sync
//     type, interval, max packet vs. the rate), the CS endpoint descriptor.
//  3. What the prebuilt TinyUSB audio driver of arduino-esp32 3.3.12 needs
//     (audio_device.c @ 88c7f0b81): AC with 0 endpoints (no interrupt EP is
//     compiled in on the S3), the interface after AC is AudioStreaming,
//     UAC1 data EP sync type != NONE (the driver tells data from feedback EPs
//     by it), max packet >= its flow-control maximum (floor(rate/1000)+1 frames).
//  4. The ESP32-S3 DWC2 budget: <= 5 IN endpoints including EP0 and the
//     shared 256-word FIFO RAM (dcd_dwc2.c dfifo_alloc, dwc2_esp32.h).
#pragma once
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace pgusb {

struct CheckResult {
  int errors = 0;
  int warnings = 0;
  // Facts found (valid when errors == 0):
  uint8_t  audioItfAc = 0xFF, audioItfAs = 0xFF, audioEpIn = 0;
  uint16_t audioMaxPacket = 0;
  uint32_t audioRate = 0;
  uint8_t  audioChannels = 0, audioSubframe = 0, audioBits = 0;
  int      fifoWordsUsed = 0;   // DWC2 FIFO RAM words this configuration needs at most
  int      inEndpoints = 0;     // including EP0
};

using CheckOut = void (*)(const char* line, void* user);

namespace detail {

struct Ctx {
  CheckOut out;
  void* user;
  CheckResult* r;
  bool verbose;
  void emit(const char* kind, const char* fmt, va_list ap) {
    char buf[200];
    int n = snprintf(buf, sizeof(buf), "%s", kind);
    if (n < 0) n = 0;
    if (static_cast<size_t>(n) < sizeof(buf)) vsnprintf(buf + n, sizeof(buf) - static_cast<size_t>(n), fmt, ap);
    if (out) out(buf, user);
  }
  void err(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
    ++r->errors;
    va_list ap; va_start(ap, fmt); emit("ERROR ", fmt, ap); va_end(ap);
  }
  void warn(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
    ++r->warnings;
    va_list ap; va_start(ap, fmt); emit("warn  ", fmt, ap); va_end(ap);
  }
  void ok(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
    if (!verbose) return;
    va_list ap; va_start(ap, fmt); emit("ok    ", fmt, ap); va_end(ap);
  }
};

inline uint16_t rd16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
inline uint32_t rd24(const uint8_t* p) { return static_cast<uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16)); }
inline int popcount16(uint16_t v) { int c = 0; while (v) { c += v & 1u; v = static_cast<uint16_t>(v >> 1); } return c; }

}  // namespace detail

// Validates `len` bytes of a configuration descriptor. expectRate: the sample
// rate the firmware runs at (0 = do not check). Lines go to out(). Returns the
// number of errors (0 = pass).
inline int checkConfiguration(const uint8_t* d, size_t len, uint32_t expectRate, CheckResult* res,
                              CheckOut out, void* user, bool verbose = false) {
  using detail::rd16;
  using detail::rd24;
  CheckResult local;
  CheckResult& R = res ? *res : local;
  R = CheckResult{};
  detail::Ctx c{out, user, &R, verbose};

  if (!d || len < 9) { c.err("configuration shorter than its 9-byte header (%u B)", static_cast<unsigned>(len)); return R.errors; }
  if (d[0] != 9 || d[1] != 0x02) c.err("config header: bLength %u type 0x%02x (want 9, 0x02)", d[0], d[1]);
  const uint16_t total = rd16(d + 2);
  if (total != len) c.err("wTotalLength %u != bytes present %u", total, static_cast<unsigned>(len));
  else c.ok("wTotalLength %u", total);
  const uint8_t numItf = d[4];
  if (d[5] == 0) c.err("bConfigurationValue 0 is reserved");
  if (!(d[7] & 0x80)) c.err("bmAttributes bit 7 must be 1 (0x%02x)", d[7]);
  if (d[8] > 250) c.err("bMaxPower %u > 250 (500 mA)", d[8]);

  // ---- pass 1: framing, interfaces, endpoints ------------------------------
  constexpr int kMaxItf = 16;
  int  nextAlt[kMaxItf] = {};
  bool itfSeen[kMaxItf] = {};
  uint16_t inEpMax[16] = {};            // largest wMaxPacketSize per IN endpoint number
  uint16_t outEpMax[16] = {};
  int  epOwnerItf[2][16];
  for (int i = 0; i < 16; ++i) { epOwnerItf[0][i] = -1; epOwnerItf[1][i] = -1; }

  int curItf = -1, curAlt = -1, epsExpected = 0, epsFound = 0;
  uint8_t curClass = 0, curSub = 0;
  int iadFirst = -1, iadCount = 0;
  auto closeInterface = [&]() {
    if (curItf >= 0 && epsFound != epsExpected)
      c.err("interface %d alt %d: bNumEndpoints %d but %d endpoint descriptors follow", curItf, curAlt, epsExpected, epsFound);
  };

  size_t off = 9;
  while (off < len) {
    const uint8_t* p = d + off;
    const uint8_t bl = p[0];
    if (bl < 2) { c.err("@%u: bLength %u < 2, stop", static_cast<unsigned>(off), bl); return R.errors; }
    if (off + bl > len) { c.err("@%u: descriptor (len %u) runs past wTotalLength", static_cast<unsigned>(off), bl); return R.errors; }
    const uint8_t type = p[1];
    if (type == 0x0B) {  // IAD
      if (bl != 8) c.err("@%u: IAD bLength %u != 8", static_cast<unsigned>(off), bl);
      iadFirst = p[2]; iadCount = p[3];
      if (iadCount < 1) c.err("@%u: IAD bInterfaceCount 0", static_cast<unsigned>(off));
      if (iadFirst + iadCount > numItf) c.err("@%u: IAD covers interfaces %d..%d but bNumInterfaces is %u", static_cast<unsigned>(off), iadFirst, iadFirst + iadCount - 1, numItf);
    } else if (type == 0x04) {  // INTERFACE
      closeInterface();
      if (bl != 9) c.err("@%u: interface bLength %u != 9", static_cast<unsigned>(off), bl);
      curItf = p[2]; curAlt = p[3]; epsExpected = p[4]; epsFound = 0; curClass = p[5]; curSub = p[6];
      if (curItf >= kMaxItf) { c.err("@%u: interface number %d too large for this checker", static_cast<unsigned>(off), curItf); return R.errors; }
      if (curAlt != nextAlt[curItf]) c.err("interface %d: alternate %d out of order (expected %d)", curItf, curAlt, nextAlt[curItf]);
      nextAlt[curItf] = curAlt + 1;
      itfSeen[curItf] = true;
      if (iadFirst >= 0 && curItf >= iadFirst + iadCount) iadFirst = -1;   // left the IAD's range
    } else if (type == 0x05) {  // ENDPOINT
      if (curItf < 0) c.err("@%u: endpoint before any interface", static_cast<unsigned>(off));
      ++epsFound;
      const bool audioStd = (curClass == 0x01 && curSub == 0x02);
      if (bl != 7 && !(bl == 9 && audioStd)) c.err("@%u: endpoint bLength %u (7, or 9 for an audio-streaming EP)", static_cast<unsigned>(off), bl);
      const uint8_t addr = p[2], attr = p[3];
      const uint16_t mps = rd16(p + 4);
      const int num = addr & 0x0F, in = (addr & 0x80) ? 1 : 0;
      if (num == 0) c.err("@%u: endpoint 0 in a configuration descriptor", static_cast<unsigned>(off));
      if (addr & 0x70) c.err("@%u: endpoint address 0x%02x has reserved bits set", static_cast<unsigned>(off), addr);
      const int xfer = attr & 0x03;
      const uint16_t size = mps & 0x07FF;
      if ((mps >> 11) != 0) c.err("@%u: EP 0x%02x: additional transactions bits set at full speed", static_cast<unsigned>(off), addr);
      if (xfer == 1 && size > 1023) c.err("EP 0x%02x: isochronous max packet %u > 1023 (full speed)", addr, size);
      if ((xfer == 2) && !(size == 8 || size == 16 || size == 32 || size == 64)) c.err("EP 0x%02x: bulk max packet %u (8/16/32/64 at full speed)", addr, size);
      if (xfer == 3 && size > 64) c.err("EP 0x%02x: interrupt max packet %u > 64", addr, size);
      if (xfer != 1 && (attr & 0x3C)) c.err("EP 0x%02x: sync/usage bits set on a non-isochronous endpoint", addr);
      // the same endpoint may appear in several alternates of ONE interface only
      if (epOwnerItf[in][num] >= 0 && epOwnerItf[in][num] != curItf)
        c.err("EP 0x%02x used by interfaces %d and %d", addr, epOwnerItf[in][num], curItf);
      epOwnerItf[in][num] = curItf;
      uint16_t* mx = in ? &inEpMax[num] : &outEpMax[num];
      if (size > *mx) *mx = size;
    } else if (type == 0x24 || type == 0x25) {
      // class-specific: checked in pass 2
    } else if (type == 0x21 || type == 0x0F) {
      // HID / BOS-type descriptors: not expected here but legal framing
    } else {
      c.warn("@%u: unexpected descriptor type 0x%02x (len %u)", static_cast<unsigned>(off), type, bl);
    }
    off += bl;
  }
  closeInterface();
  int itfTotal = 0;
  for (int i = 0; i < kMaxItf; ++i) if (itfSeen[i]) ++itfTotal;
  if (itfTotal != numItf) c.err("bNumInterfaces %u but %d interface numbers present", numItf, itfTotal);
  for (int i = 0; i < itfTotal; ++i) if (!itfSeen[i]) c.err("interface numbers are not contiguous (missing %d)", i);
  if (R.errors == 0) c.ok("framing: %d interfaces, endpoint and alternate numbering consistent", itfTotal);

  // ---- DWC2 (ESP32-S3) endpoint / FIFO budget ------------------------------
  {
    int inCount = 1;                         // EP0 IN
    int txWords = 16;                        // EP0 IN: 64 bytes (dfifo_device_init)
    int largestOut = 64;                     // EP0 OUT
    for (int n = 1; n < 16; ++n) {
      if (inEpMax[n]) { ++inCount; txWords += (inEpMax[n] + 3) / 4; }
      if (outEpMax[n] > largestOut) largestOut = outEpMax[n];
      if ((inEpMax[n] || outEpMax[n]) && n >= 7) c.err("EP number %d >= 7 (DWC2 on the S3 has endpoints 0..6)", n);
    }
    const int rxWords = 13 + 1 + 2 * (largestOut / 4 + 1) + 2 * 7;   // calc_device_grxfsiz, ep_count 7
    R.inEndpoints = inCount;
    R.fifoWordsUsed = rxWords + txWords;
    if (inCount > 5) c.err("%d IN endpoints incl. EP0 > 5 (dwc2_esp32.h ep_in_count)", inCount);
    if (R.fifoWordsUsed > 256) c.err("DWC2 FIFO RAM: %d words needed > 256", R.fifoWordsUsed);
    else c.ok("DWC2 budget: %d IN endpoints incl. EP0, FIFO %d of 256 words (rx %d, tx %d)", inCount, R.fifoWordsUsed, rxWords, txWords);
  }

  // ---- pass 2: the audio function ------------------------------------------
  // Locate the AudioControl interface (class 1, subclass 1).
  size_t acOff = 0;
  for (off = 9; off < len; off += d[off]) {
    const uint8_t* p = d + off;
    if (p[1] == 0x04 && p[5] == 0x01 && p[6] == 0x01) { acOff = off; break; }
  }
  if (!acOff) { c.err("no AudioControl interface (class 0x01 subclass 0x01)"); return R.errors; }
  const uint8_t* ac = d + acOff;
  R.audioItfAc = ac[2];
  if (ac[3] != 0) c.err("AC interface: bAlternateSetting %u (TinyUSB audiod_open needs 0)", ac[3]);
  if (ac[4] != 0) c.err("AC interface: bNumEndpoints %u; the prebuilt driver has no interrupt EP on the S3 (CFG_TUD_AUDIO_ENABLE_INTERRUPT_EP 0) and rejects it", ac[4]);
  if (ac[7] != 0x00) c.err("AC interface: bInterfaceProtocol 0x%02x (UAC1 = 0x00)", ac[7]);

  // CS AC header right after.
  off = acOff + ac[0];
  if (off + 8 > len || d[off + 1] != 0x24 || d[off + 2] != 0x01) { c.err("CS AC header does not follow the AC interface"); return R.errors; }
  const uint8_t* hdr = d + off;
  if (rd16(hdr + 3) != 0x0100) c.err("AC header bcdADC 0x%04x != 0x0100", rd16(hdr + 3));
  const uint16_t csTotal = rd16(hdr + 5);
  const uint8_t inColl = hdr[7];
  if (hdr[0] != 8 + inColl) c.err("AC header bLength %u != 8 + bInCollection (%u)", hdr[0], inColl);
  if (inColl != 1) c.err("bInCollection %u (this function has exactly one AS interface)", inColl);
  const uint8_t asItf = (hdr[0] >= 9) ? hdr[8] : 0xFF;
  R.audioItfAs = asItf;

  // Walk the CS AC block: header + units/terminals until the next non-CS descriptor.
  struct Entity { uint8_t id, subtype, src, nch; uint16_t type, chcfg; };
  Entity ent[8];
  int nEnt = 0;
  size_t csSum = 0;
  size_t q = off;
  while (q + 2 < len && d[q + 1] == 0x24) {
    const uint8_t* p = d + q;
    csSum += p[0];
    const uint8_t st = p[2];
    if (st == 0x02) {        // INPUT_TERMINAL
      if (p[0] != 12) c.err("input terminal bLength %u != 12", p[0]);
      else if (nEnt < 8) ent[nEnt++] = Entity{p[3], st, 0, p[7], rd16(p + 4), rd16(p + 8)};
    } else if (st == 0x03) { // OUTPUT_TERMINAL
      if (p[0] != 9) c.err("output terminal bLength %u != 9", p[0]);
      else if (nEnt < 8) ent[nEnt++] = Entity{p[3], st, p[7], 0, rd16(p + 4), 0};
    } else if (st != 0x01) {
      c.warn("CS AC subtype 0x%02x not validated by this checker", st);
      if (nEnt < 8 && p[0] > 3) ent[nEnt++] = Entity{p[3], st, 0, 0, 0, 0};
    }
    q += p[0];
  }
  if (csSum != csTotal) c.err("AC header wTotalLength %u != %u bytes of CS AC descriptors", csTotal, static_cast<unsigned>(csSum));
  else c.ok("AC header: bcdADC 1.00, wTotalLength %u matches, AS interface %u", csTotal, asItf);

  int itIdx = -1, otIdx = -1;
  for (int i = 0; i < nEnt; ++i) {
    if (ent[i].id == 0) c.err("entity ID 0 is reserved");
    for (int j = 0; j < i; ++j) if (ent[j].id == ent[i].id) c.err("entity ID %u used twice", ent[i].id);
    if (ent[i].subtype == 0x03 && ent[i].type == 0x0101) otIdx = i;
  }
  if (otIdx < 0) c.err("no Output Terminal of type USB streaming (0x0101): the host would see no capture stream");
  else {
    for (int i = 0; i < nEnt; ++i) if (ent[i].id == ent[otIdx].src) itIdx = i;
    if (itIdx < 0) c.err("output terminal %u: bSourceID %u does not exist", ent[otIdx].id, ent[otIdx].src);
    else if (ent[itIdx].subtype != 0x02) c.warn("OT source %u is not an input terminal (units are not checked)", ent[otIdx].src);
    else {
      const Entity& it = ent[itIdx];
      if (it.type == 0x0101) c.err("input terminal %u is USB streaming: that is a host->device (speaker) path, not an input", it.id);
      if ((it.type >> 8) == 0x01 || it.type == 0x0100) c.err("input terminal type 0x%04x is a USB type", it.type);
      if (it.nch < 1) c.err("input terminal bNrChannels 0");
      if (detail::popcount16(it.chcfg) > it.nch) c.err("wChannelConfig 0x%04x names more channels than bNrChannels %u", it.chcfg, it.nch);
      if (it.chcfg & 0xF000) c.err("wChannelConfig 0x%04x uses reserved bits 12..15", it.chcfg);
      R.audioChannels = it.nch;
      c.ok("topology: IT %u (type 0x%04x, %u ch, config 0x%04x) -> OT %u (USB streaming)", it.id, it.type, it.nch, it.chcfg, ent[otIdx].id);
    }
  }

  // TinyUSB audiod_open: the next INTERFACE after AC must be AudioStreaming.
  size_t asOff = 0;
  for (off = q; off < len; off += d[off]) {
    if (d[off + 1] == 0x04) { asOff = off; break; }
  }
  if (!asOff || asOff + 9 > len || d[asOff + 5] != 0x01 || d[asOff + 6] != 0x02) { c.err("the interface after AC is not AudioStreaming (audiod_open rejects the function)"); return R.errors; }
  if (d[asOff + 2] != asItf) c.err("first AS interface is %u but baInterfaceNr says %u", d[asOff + 2], asItf);
  if (asItf != R.audioItfAc + 1) c.warn("AS interface %u is not AC + 1", asItf);

  // Walk the AS interface's alternates.
  bool sawAlt0 = false, sawStreamingAlt = false;
  for (off = asOff; off < len;) {
    const uint8_t* p = d + off;
    if (p[1] == 0x04 && p[2] != asItf) break;          // next interface: done
    if (p[1] == 0x0B) break;                           // next function
    if (p[1] == 0x04) {
      const uint8_t alt = p[3], neps = p[4];
      if (p[5] != 0x01 || p[6] != 0x02 || p[7] != 0x00) c.err("AS interface %u alt %u: class/sub/proto %02x/%02x/%02x (want 01/02/00)", asItf, alt, p[5], p[6], p[7]);
      if (alt == 0) {
        sawAlt0 = true;
        if (neps != 0) c.err("AS alt 0 has %u endpoints; alt 0 must be zero-bandwidth (audio10 4.5.1)", neps);
        else c.ok("AS alt 0: zero bandwidth");
      } else {
        if (neps != 1) c.err("AS alt %u has %u endpoints (want 1 data EP, no explicit sync EP for an async source)", alt, neps);
        // Expect: CS AS general, format type I, std iso EP (9), CS EP.
        size_t o2 = off + p[0];
        const uint8_t* g = d + o2;
        if (o2 + 7 > len || g[1] != 0x24 || g[2] != 0x01) { c.err("AS alt %u: AS_GENERAL does not follow", alt); return R.errors; }
        if (g[0] != 7) c.err("AS_GENERAL bLength %u != 7", g[0]);
        if (otIdx >= 0 && g[3] != ent[otIdx].id) c.err("AS_GENERAL bTerminalLink %u is not the USB-streaming OT %u", g[3], ent[otIdx].id);
        if (rd16(g + 5) != 0x0001) c.err("wFormatTag 0x%04x is not PCM (0x0001)", rd16(g + 5));
        o2 += g[0];
        const uint8_t* f = d + o2;
        if (o2 + 8 > len || f[1] != 0x24 || f[2] != 0x02) { c.err("AS alt %u: FORMAT_TYPE does not follow AS_GENERAL", alt); return R.errors; }
        if (f[3] != 0x01) c.err("bFormatType %u != FORMAT_TYPE_I", f[3]);
        const uint8_t nch = f[4], sub = f[5], bits = f[6], nfreq = f[7];
        if (f[0] != (nfreq ? 8 + 3 * nfreq : 14)) c.err("format type I bLength %u inconsistent with bSamFreqType %u", f[0], nfreq);
        if (R.audioChannels && nch != R.audioChannels) c.err("format bNrChannels %u != input terminal channels %u", nch, R.audioChannels);
        if (sub < 1 || sub > 4) c.err("bSubFrameSize %u not in 1..4", sub);
        if (bits > 8 * sub || bits < 8) c.err("bBitResolution %u does not fit bSubFrameSize %u", bits, sub);
        uint32_t firstRate = 0;
        bool rateFound = (expectRate == 0);
        if (nfreq == 0) {
          c.warn("continuous sample-rate range (bSamFreqType 0): not what this firmware means to send");
          firstRate = rd24(f + 8);
        } else {
          for (int k = 0; k < nfreq && o2 + 8 + 3 * k + 3 <= len; ++k) {
            const uint32_t r = rd24(f + 8 + 3 * k);
            if (k == 0) firstRate = r;
            if (r < 8000 || r > 48000) c.err("tSamFreq[%d] = %lu Hz outside 8..48 kHz (full-speed UAC1 stereo 16-bit)", k, static_cast<unsigned long>(r));
            if (r == expectRate) rateFound = true;
          }
        }
        if (!rateFound) c.err("tSamFreq does not list the running rate %lu Hz", static_cast<unsigned long>(expectRate));
        R.audioRate = firstRate; R.audioSubframe = sub; R.audioBits = bits;
        c.ok("format: PCM type I, %u ch, %u-byte subframe, %u bit, %u rate(s), first %lu Hz", nch, sub, bits, nfreq, static_cast<unsigned long>(firstRate));
        o2 += f[0];
        const uint8_t* e = d + o2;
        if (o2 + 9 > len || e[1] != 0x05) { c.err("AS alt %u: endpoint does not follow the format descriptor", alt); return R.errors; }
        if (e[0] != 9) c.err("audio data EP bLength %u (UAC1 uses the 9-byte form with bRefresh/bSynchAddress)", e[0]);
        const uint8_t addr = e[2], attr = e[3];
        const uint16_t mps = rd16(e + 4);
        if (!(addr & 0x80)) c.err("data EP 0x%02x is OUT; an input device needs an IN endpoint", addr);
        if ((attr & 0x03) != 0x01) c.err("data EP 0x%02x is not isochronous", addr);
        const int sync = (attr >> 2) & 0x03;
        if (sync != 0x01) c.err("data EP sync type %d: want asynchronous (1); the device clock (I2S) is not locked to SOF", sync);
        if (sync == 0) c.err("sync type NONE: the prebuilt TinyUSB driver would take this UAC1 EP for a FEEDBACK endpoint and never stream (audiod_set_interface)");
        if ((attr >> 4) & 0x03) c.err("usage bits set on a UAC1 endpoint");
        if (e[6] != 1) c.err("bInterval %u: full-speed audio data EPs use 1 (every frame)", e[6]);
        if (e[7] != 0) c.err("bRefresh %u must be 0 for a data endpoint", e[7]);
        if (e[8] != 0) c.err("bSynchAddress 0x%02x: an asynchronous SOURCE needs no sync endpoint", e[8]);
        const uint32_t fb = static_cast<uint32_t>(nch) * sub;
        if (firstRate && fb) {
          const uint32_t tusbMax = (firstRate / 1000u + 1u) * fb;          // audiod_calc_tx_packet_sz
          const uint32_t ceilFrames = ((firstRate + 999u) / 1000u) * fb;  // largest packet the nominal cadence needs
          if (mps < tusbMax) c.err("wMaxPacketSize %u < TinyUSB's flow-control maximum %lu (TU_ASSERT in audiod_calc_tx_packet_sz)", mps, static_cast<unsigned long>(tusbMax));
          if (mps < ceilFrames) c.err("wMaxPacketSize %u < ceil(rate/1000) frames (%lu B)", mps, static_cast<unsigned long>(ceilFrames));
          if (mps % fb) c.warn("wMaxPacketSize %u is not a whole number of %lu-byte frames", mps, static_cast<unsigned long>(fb));
          c.ok("data EP 0x%02x: iso IN async, interval 1, max packet %u B (>= %lu B flow-control max)", addr, mps, static_cast<unsigned long>(tusbMax));
        }
        R.audioEpIn = addr; R.audioMaxPacket = mps;
        o2 += e[0];
        const uint8_t* ce = d + o2;
        if (o2 + 7 > len || ce[1] != 0x25) { c.err("CS endpoint descriptor does not follow the data EP"); return R.errors; }
        if (ce[0] != 7 || ce[2] != 0x01) c.err("CS EP: bLength %u subtype %u (want 7, EP_GENERAL 1)", ce[0], ce[2]);
        if (ce[3] & 0x7C) c.err("CS EP bmAttributes 0x%02x uses reserved bits", ce[3]);
        if (ce[4] > 2) c.err("bLockDelayUnits %u > 2", ce[4]);
        sawStreamingAlt = true;
        off = o2 + ce[0];
        continue;
      }
    }
    off += p[0];
  }
  if (!sawAlt0) c.err("AS interface has no alternate 0");
  if (!sawStreamingAlt) c.err("AS interface has no streaming alternate");
  if (R.audioItfAs != 0xFF && R.audioItfAs >= numItf) c.err("AS interface %u >= bNumInterfaces %u", R.audioItfAs, numItf);
  return R.errors;
}

}  // namespace pgusb
