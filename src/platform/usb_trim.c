// Platform layer, USB builds (PG_USB_AUDIO) -- "linker diet" for the prebuilt TinyUSB of arduino-esp32 3.3.12.
//
// libarduino_tinyusb.a is built with nearly every class enabled. usbd.c's
// driver table references every device class driver, and usbh.c (pulled in by
// tusb.c's tuh_inited even in device mode) references every host class
// driver, so the linker brings in all of them with their static buffers --
// about 32 KB of .bss for a device that uses only CDC + audio (measured with
// host/usbspike/ram_diff.py, see docs/usb-audio.md).
//
// An archive member is only linked to resolve an undefined symbol. Defining the
// table entries here, in an object the linker sees first, means those members
// (msc_device.c.obj, ncm_device.c.obj, ...) are never extracted. If some other
// object ever needs another symbol from one of them, the link fails with
// "multiple definition" -- a build error, never a silent runtime change.
//
// Behaviour of the stubs is exactly that of a class driver which finds no
// interface of its class in the configuration: init/reset do nothing, open()
// returns 0 ("not mine", usbd.c process_set_config tries the next driver),
// and the control/transfer callbacks are never reached because no interface
// or endpoint is bound to them. The host-side stubs are never called at all
// (the stack is initialised in device mode only).
//
// Off with -DPG_USB_TRIM=0 (e.g. PLATFORMIO_BUILD_FLAGS=-DPG_USB_TRIM=0). Empty in
// every env without PG_USB_AUDIO.
// Prototypes come from the installed class headers via tusb.h, so the compiler
// checks every signature against the library.
#ifndef PG_USB_TRIM
#define PG_USB_TRIM 1
#endif
#ifndef PG_USB_TRIM_HOST   // also the host stack's core (below); -DPG_USB_TRIM_HOST=0 keeps it
#define PG_USB_TRIM_HOST 1
#endif

#if PG_USB_AUDIO && PG_USB_TRIM
#include "tusb.h"

#define UNUSED_ARGS4(a, b, c, d) ((void)(a), (void)(b), (void)(c), (void)(d))

// ---- device class drivers we do not use ------------------------------------
#define DEVICE_DRIVER_STUB(p)                                                                          \
  void p##_init(void) {}                                                                               \
  bool p##_deinit(void) { return true; }                                                               \
  void p##_reset(uint8_t rhport) { (void)rhport; }                                                     \
  uint16_t p##_open(uint8_t rhport, tusb_desc_interface_t const* d, uint16_t max_len) {               \
    (void)rhport; (void)d; (void)max_len;                                                              \
    return 0;                                                                                          \
  }                                                                                                    \
  bool p##_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const* r) {          \
    (void)rhport; (void)stage; (void)r;                                                                \
    return false;                                                                                      \
  }

#define DEVICE_XFER_STUB(p)                                                                            \
  bool p##_xfer_cb(uint8_t rhport, uint8_t ep, xfer_result_t res, uint32_t n) {                        \
    UNUSED_ARGS4(rhport, ep, res, n);                                                                  \
    return false;                                                                                      \
  }

#if CFG_TUD_MSC          // 4160 B .bss (_mscd_epbuf 4096)
DEVICE_DRIVER_STUB(mscd)
DEVICE_XFER_STUB(mscd)
#endif
#if CFG_TUD_DFU          // 4106 B .bss (_transfer_buf 4096)
DEVICE_DRIVER_STUB(dfu_moded)
#endif
#if CFG_TUD_NCM          // 6488 B .bss (ncm_epbuf 6416)
DEVICE_DRIVER_STUB(netd)
DEVICE_XFER_STUB(netd)
#endif
#if CFG_TUD_HID
DEVICE_DRIVER_STUB(hidd)
DEVICE_XFER_STUB(hidd)
#endif
#if CFG_TUD_MIDI
DEVICE_DRIVER_STUB(midid)
DEVICE_XFER_STUB(midid)
#endif
#if CFG_TUD_VENDOR
DEVICE_DRIVER_STUB(vendord)
DEVICE_XFER_STUB(vendord)
#endif
#if CFG_TUD_VIDEO
DEVICE_DRIVER_STUB(videod)
DEVICE_XFER_STUB(videod)
#endif
#if CFG_TUD_DFU_RUNTIME
DEVICE_DRIVER_STUB(dfu_rtd)
#endif

// ---- host class drivers (usbh.c's table; never run in device mode) ---------
#define HOST_DRIVER_STUB(p)                                                                            \
  bool p##_init(void) { return true; }                                                                 \
  bool p##_deinit(void) { return true; }                                                               \
  uint16_t p##_open(uint8_t rhport, uint8_t dev_addr, tusb_desc_interface_t const* d, uint16_t len) {  \
    UNUSED_ARGS4(rhport, dev_addr, d, len);                                                            \
    return 0;                                                                                          \
  }                                                                                                    \
  bool p##_set_config(uint8_t dev_addr, uint8_t itf_num) {                                             \
    (void)dev_addr; (void)itf_num;                                                                     \
    return false;                                                                                      \
  }                                                                                                    \
  bool p##_xfer_cb(uint8_t dev_addr, uint8_t ep, xfer_result_t res, uint32_t n) {                      \
    UNUSED_ARGS4(dev_addr, ep, res, n);                                                                \
    return false;                                                                                      \
  }                                                                                                    \
  void p##_close(uint8_t dev_addr) { (void)dev_addr; }

#if CFG_TUH_ENABLED
#if CFG_TUH_HID          // 2983 B .bss
HOST_DRIVER_STUB(hidh)
#endif
#if CFG_TUH_CDC          // 548 B .bss
HOST_DRIVER_STUB(cdch)
#endif
#if CFG_TUH_MSC          // 700 B .bss
HOST_DRIVER_STUB(msch)
#endif
#if CFG_TUH_MIDI         // 528 B .bss
HOST_DRIVER_STUB(midih)
#endif

// ---- the host stack's core (usbh.c, hub.c, hcd_dwc2.c: 2416 B .bss) --------
// tusb.c references these few symbols of usbh.c.obj (tusb_inited,
// tusb_rhport_init / tusb_deinit for a host-role port, the endpoint claim /
// xfer helpers it wraps for both roles) and tusb.c / dcd_dwc2.c reference
// hcd_int_handler (the DWC2 interrupt dispatch for the host role). This
// firmware initialises port 0 as a DEVICE only (tinyusb_init ->
// tusb_rhport_init(0, device)), so the host side is "not initialised": the
// stubs answer exactly that, and the three members are never linked. If
// anything else ever needs a symbol of usbh.c.obj, the link fails with
// "multiple definition". (Without these stubs the host class stubs above are
// what keeps usbh.c's driver table from pulling the class drivers in.)
#if PG_USB_TRIM_HOST
#include "host/hcd.h"
#include "host/usbh_pvt.h"

bool tuh_inited(void) { return false; }
bool tuh_deinit(uint8_t rhport) { (void)rhport; return true; }
bool tuh_rhport_init(uint8_t rhport, const tusb_rhport_init_t* rh_init) {
  (void)rhport; (void)rh_init;
  return false;
}
bool usbh_edpt_claim(uint8_t dev_addr, uint8_t ep_addr) { (void)dev_addr; (void)ep_addr; return false; }
bool usbh_edpt_release(uint8_t dev_addr, uint8_t ep_addr) { (void)dev_addr; (void)ep_addr; return false; }
bool usbh_edpt_xfer_with_callback(uint8_t dev_addr, uint8_t ep_addr, uint8_t* buffer, uint16_t total_bytes,
                                  tuh_xfer_cb_t complete_cb, uintptr_t user_data) {
  (void)dev_addr; (void)ep_addr; (void)buffer; (void)total_bytes; (void)complete_cb; (void)user_data;
  return false;
}
void hcd_int_handler(uint8_t rhport, bool in_isr) { (void)rhport; (void)in_isr; }
#endif  // PG_USB_TRIM_HOST
#endif  // CFG_TUH_ENABLED

#endif  // PG_USB_AUDIO && PG_USB_TRIM
