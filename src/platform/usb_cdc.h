// Platform-internal (for src/platform/*.cpp and the spike, NOT for src/app):
// the serial port object of the USB builds (PG_USB_AUDIO).
//
// With ARDUINO_USB_CDC_ON_BOOT=0 the core defines no USB serial object
// (USBSerial exists only with CDC_ON_BOOT=1) and `Serial` is UART0, whose TX
// pin GPIO43 is the I2S word clock on this board. So the CDC port is our own
// USBCDC instance, defined in usb_audio.cpp: its constructor registers the CDC
// interface (interfaces 0-1, EPs 0x85 / 0x03 / 0x84) before the USB stack
// starts, the same as the core's USBSerial would.
#pragma once
#if PG_USB_AUDIO
#include <USBCDC.h>
extern USBCDC PgSerial;
#endif
