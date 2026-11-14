// See usb_serial.h. Checked against libDaisy 1289a00: src/hid/usb.cpp (UsbHandle),
// src/usbd/usbd_cdc_if.c (CDC_Transmit_FS returns USBD_BUSY while TxState != 0 and
// dereferences pClassData without a check; CDC_Receive_FS re-arms the endpoint, then
// calls the receive callback with the packet).
#include "usb_serial.h"

#include "../core/ring.h"
#include "daisy_seed.h"
#include "usbd_def.h"

extern "C" USBD_HandleTypeDef hUsbDeviceFS;   // libDaisy src/hid/usb.cpp

namespace pgdev {
namespace usbserial {

namespace {

daisy::UsbHandle g_usb;
pgd::ByteRing g_rx, g_tx;
uint8_t g_buf[2][kChunk];
int g_cur = 0;
uint32_t g_pend = 0;   // bytes waiting in g_buf[g_cur]
volatile uint32_t g_rxBytes = 0, g_rxDropped = 0;
uint32_t g_txBytes = 0, g_txDropped = 0, g_txPackets = 0, g_txBusy = 0;

void onReceive(uint8_t* buf, uint32_t* len) {   // USB interrupt
  const uint32_t n = *len;
  const uint32_t k = g_rx.pushSome(buf, n);
  g_rxBytes = g_rxBytes + n;
  if (k < n) g_rxDropped = g_rxDropped + (n - k);
}

}  // namespace

void begin(uint8_t* rxMem, uint32_t rxBytes, uint8_t* txMem, uint32_t txBytes) {
  g_rx.attach(rxMem, rxBytes);
  g_tx.attach(txMem, txBytes);
  g_usb.Init(daisy::UsbHandle::FS_INTERNAL);
  // Right after Init: CDC_Receive_FS calls the callback without a null check.
  g_usb.SetReceiveCallback(onReceive, daisy::UsbHandle::FS_INTERNAL);
}

bool configured() {
  // CDC_Transmit_FS reads pClassData->TxState unchecked: before the computer has
  // configured the device pClassData is null (address 0 is ITCM on the H7, so the
  // read would not even fault; the write that follows would land in ITCM).
  return hUsbDeviceFS.dev_state == USBD_STATE_CONFIGURED && hUsbDeviceFS.pClassData != nullptr;
}

int read() { return g_rx.popByte(); }

void write(const char* p, size_t n) {
  const uint32_t k = g_tx.pushSome(reinterpret_cast<const uint8_t*>(p), static_cast<uint32_t>(n));
  g_txBytes += k;
  if (k < n) g_txDropped += static_cast<uint32_t>(n - k);
}

size_t room() { return g_tx.space(); }

void flush() {
  for (int round = 0; round < 4; ++round) {
    if (!g_pend) g_pend = g_tx.pop(g_buf[g_cur], kChunk);
    if (!g_pend) return;
    if (!configured()) {
      // Nobody there yet: the ring keeps filling, then drops what does not fit.
      return;
    }
    if (g_usb.TransmitInternal(g_buf[g_cur], g_pend) != daisy::UsbHandle::Result::OK) {
      ++g_txBusy;   // the previous packet is still going out
      return;
    }
    ++g_txPackets;
    g_pend = 0;
    g_cur ^= 1;   // this buffer is the stack's until the next transmit is accepted
  }
}

Stats stats() { return Stats{g_rxBytes, g_rxDropped, g_txBytes, g_txDropped, g_txPackets, g_txBusy}; }

}  // namespace usbserial
}  // namespace pgdev
