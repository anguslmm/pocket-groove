// USB audio spike -- the UAC1 (USB Audio Class 1.0) descriptor block for a
// stereo 16-bit USB *input* device ("microphone" from the host's point of
// view): one AudioControl interface + one AudioStreaming interface with an
// asynchronous isochronous IN endpoint.
//
// Portable (no Arduino / TinyUSB / ESP-IDF headers): the firmware builds its
// descriptor with this, and host/usbspike/check_usb_descriptors.cpp builds the
// very same bytes on the desktop and validates them field by field.
//
// Spec references: "USB Device Class Definition for Audio Devices" 1.0
// (audio10.pdf) sections 4.3 - 4.6, "... for Audio Data Formats" 1.0
// (frmts10.pdf) 2.2.5, USB 2.0 9.6.5/9.6.6.
//
// Topology (entity IDs):  Input Terminal 1 (line connector, 2 ch, L/R front)
//                      -> Output Terminal 2 (USB streaming)  -> AS interface
// No Feature Unit: the device has no volume/mute for the host to control, so
// there are no class-specific entity requests to answer. The only class
// request is the endpoint's Sampling Frequency Control (SET_CUR / GET_CUR),
// declared because hosts commonly send SET_CUR even for a single rate.
#pragma once
#include <cstddef>
#include <cstdint>

namespace pgusb {

struct Uac1Params {
  uint8_t  itfAc       = 0;     // AudioControl interface number; AudioStreaming is itfAc + 1
  uint8_t  epIn        = 0x83;  // isochronous IN endpoint address (bit 7 set)
  uint8_t  strIdx      = 0;     // iInterface string index for both interfaces (0 = none)
  uint32_t sampleRate  = 44100; // the ONE discrete rate advertised (bSamFreqType = 1)
};

constexpr uint8_t  kChannels       = 2;      // stereo
constexpr uint8_t  kBytesPerSample = 2;      // bSubFrameSize
constexpr uint8_t  kBitsPerSample  = 16;     // bBitResolution
constexpr uint8_t  kFrameBytes     = kChannels * kBytesPerSample;
constexpr uint8_t  kEntityIt       = 1;      // Input Terminal ID
constexpr uint8_t  kEntityOt       = 2;      // Output Terminal ID
constexpr uint16_t kItTerminalType = 0x0603; // Line connector (Terminal Types 1.0, 2.3 External)
constexpr uint16_t kOtTerminalType = 0x0101; // USB streaming
constexpr uint16_t kChannelConfig  = 0x0003; // Left Front | Right Front

// Descriptor lengths (audio10.pdf / frmts10.pdf tables).
constexpr size_t kLenStdAc      = 9;   // 4.3.1  standard AC interface
constexpr size_t kLenCsAcHeader = 9;   // 4.3.2  class-specific AC header, 8 + bInCollection (1)
constexpr size_t kLenIt         = 12;  // 4.3.2.1 input terminal
constexpr size_t kLenOt         = 9;   // 4.3.2.2 output terminal
constexpr size_t kLenStdAs      = 9;   // 4.5.1  standard AS interface (per alternate setting)
constexpr size_t kLenCsAs       = 7;   // 4.5.2  class-specific AS general
constexpr size_t kLenFormatI    = 11;  // frmts10 2.2.5 type I format, 8 + 3 * bSamFreqType (1)
constexpr size_t kLenStdIsoEp   = 9;   // 4.6.1.1 standard AS isochronous data endpoint (audio-class 9-byte form)
constexpr size_t kLenCsIsoEp    = 7;   // 4.6.1.2 class-specific AS isochronous data endpoint

constexpr size_t kUac1DescLen = kLenStdAc + kLenCsAcHeader + kLenIt + kLenOt +
                                kLenStdAs + kLenStdAs + kLenCsAs + kLenFormatI + kLenStdIsoEp + kLenCsIsoEp;  // 91
constexpr uint16_t kCsAcTotalLen = kLenCsAcHeader + kLenIt + kLenOt;  // wTotalLength of the CS AC block: 30

// wMaxPacketSize: room for ceil(rate / 1000) + 1 frames per 1 ms frame. That
// covers TinyUSB's largest flow-control packet, (floor(rate / 1000) + 1) frames
// (audio_device.c audiod_calc_tx_packet_sz), at every rate. 44100 -> 46 * 4 =
// 184 bytes, 32000 -> 33 * 4 = 132 bytes. Same formula as TinyUSB's
// TUD_AUDIO_EP_SIZE(false, rate, 2, 2).
constexpr uint16_t maxPacketBytes(uint32_t rate) {
  return static_cast<uint16_t>(((rate + 999u) / 1000u + 1u) * kFrameBytes);
}

// Writes kUac1DescLen bytes to dst (cap must be >= kUac1DescLen). Returns the
// number of bytes written, 0 if cap is too small or the rate is not encodable.
inline size_t buildUac1(uint8_t* dst, size_t cap, const Uac1Params& p) {
  if (!dst || cap < kUac1DescLen || p.sampleRate == 0 || p.sampleRate > 0xFFFFFFu) return 0;
  const uint8_t itfAs = static_cast<uint8_t>(p.itfAc + 1);
  const uint16_t mps = maxPacketBytes(p.sampleRate);
  size_t n = 0;
  auto b  = [&](uint32_t v) { dst[n++] = static_cast<uint8_t>(v & 0xFFu); };
  auto w  = [&](uint32_t v) { b(v); b(v >> 8); };
  auto w3 = [&](uint32_t v) { b(v); b(v >> 8); b(v >> 16); };

  // --- Standard AC interface descriptor (4.3.1)
  b(kLenStdAc); b(0x04 /*INTERFACE*/); b(p.itfAc); b(0 /*bAlternateSetting*/);
  b(0 /*bNumEndpoints: no interrupt EP*/); b(0x01 /*AUDIO*/); b(0x01 /*AUDIOCONTROL*/);
  b(0x00 /*bInterfaceProtocol: UAC1*/); b(p.strIdx);
  // --- Class-specific AC interface header (4.3.2)
  b(kLenCsAcHeader); b(0x24 /*CS_INTERFACE*/); b(0x01 /*HEADER*/); w(0x0100 /*bcdADC*/);
  w(kCsAcTotalLen); b(1 /*bInCollection*/); b(itfAs /*baInterfaceNr(1)*/);
  // --- Input terminal (4.3.2.1)
  b(kLenIt); b(0x24); b(0x02 /*INPUT_TERMINAL*/); b(kEntityIt); w(kItTerminalType);
  b(0 /*bAssocTerminal*/); b(kChannels); w(kChannelConfig); b(0 /*iChannelNames*/); b(0 /*iTerminal*/);
  // --- Output terminal (4.3.2.2)
  b(kLenOt); b(0x24); b(0x03 /*OUTPUT_TERMINAL*/); b(kEntityOt); w(kOtTerminalType);
  b(0 /*bAssocTerminal*/); b(kEntityIt /*bSourceID*/); b(0 /*iTerminal*/);
  // --- Standard AS interface, alternate 0: zero bandwidth (4.5.1)
  b(kLenStdAs); b(0x04); b(itfAs); b(0); b(0 /*bNumEndpoints*/); b(0x01); b(0x02 /*AUDIOSTREAMING*/);
  b(0x00); b(p.strIdx);
  // --- Standard AS interface, alternate 1: streaming (4.5.1)
  b(kLenStdAs); b(0x04); b(itfAs); b(1); b(1 /*bNumEndpoints*/); b(0x01); b(0x02); b(0x00); b(p.strIdx);
  // --- Class-specific AS general (4.5.2)
  b(kLenCsAs); b(0x24); b(0x01 /*AS_GENERAL*/); b(kEntityOt /*bTerminalLink*/);
  b(1 /*bDelay, frames*/); w(0x0001 /*wFormatTag: PCM*/);
  // --- Type I format type (frmts10 2.2.5)
  b(kLenFormatI); b(0x24); b(0x02 /*FORMAT_TYPE*/); b(0x01 /*FORMAT_TYPE_I*/); b(kChannels);
  b(kBytesPerSample); b(kBitsPerSample); b(1 /*bSamFreqType: one discrete rate*/); w3(p.sampleRate);
  // --- Standard AS isochronous audio data endpoint (4.6.1.1)
  // bmAttributes 0x05 = isochronous | asynchronous: the device's own clock (the
  // I2S clock) sets the rate; the number of frames per packet carries it.
  b(kLenStdIsoEp); b(0x05 /*ENDPOINT*/); b(p.epIn); b(0x05 /*bmAttributes*/); w(mps);
  b(1 /*bInterval: every frame*/); b(0 /*bRefresh*/); b(0 /*bSynchAddress*/);
  // --- Class-specific AS isochronous audio data endpoint (4.6.1.2)
  b(kLenCsIsoEp); b(0x25 /*CS_ENDPOINT*/); b(0x01 /*EP_GENERAL*/);
  b(0x01 /*bmAttributes: Sampling Frequency control*/); b(0 /*bLockDelayUnits*/); w(0 /*wLockDelay*/);
  return n;
}

}  // namespace pgusb
