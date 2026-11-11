// USB audio spike -- "linker diet" for the prebuilt TinyUSB of arduino-esp32 3.3.12.
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
// Off with -DUSBSPIKE_TRIM=0 (e.g. PLATFORMIO_BUILD_FLAGS=-DUSBSPIKE_TRIM=0).
// Prototypes come from the installed class headers via tusb.h, so the compiler
// checks every signature against the library.
#ifndef USBSPIKE_TRIM
#define USBSPIKE_TRIM 1
#endif

#if USBSPIKE_TRIM && !USBSPIKE_NO_USB
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
#endif  // CFG_TUH_ENABLED

#endif  // USBSPIKE_TRIM && !USBSPIKE_NO_USB
