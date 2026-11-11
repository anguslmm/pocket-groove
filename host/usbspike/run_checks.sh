#!/bin/sh
# Builds and runs the offline USB descriptor / flow-control check of the USB
# audio spike (host/usbspike/check_usb_descriptors.cpp). Run from the
# repository root. Needs a host C++17 compiler and the PlatformIO packages that
# `pio run -e usb-audio-spike` installs (only TinyUSB's headers are used).
#
#   sh host/usbspike/run_checks.sh                 # offline checks
#   sh host/usbspike/run_checks.sh --dump log.txt  # + validate a device 'd' dump
set -e
PIO_PKGS="${PLATFORMIO_CORE_DIR:-$HOME/.platformio}/packages"
TUSB="$PIO_PKGS/framework-arduinoespressif32-libs/esp32s3/include/arduino_tinyusb/tinyusb/src"
if [ ! -f "$TUSB/device/usbd.h" ]; then
  echo "TinyUSB headers not found at $TUSB -- build the firmware once (pio run -e usb-audio-spike)" >&2
  exit 2
fi
CXX="${CXX:-c++}"
mkdir -p build/usbspike
"$CXX" -std=c++17 -O1 -Wall -Wextra -Werror \
  -DCFG_TUSB_MCU=OPT_MCU_NONE -DCFG_TUSB_OS=OPT_OS_NONE -DCFG_TUD_ENABLED=1 -DTUP_DCD_ENDPOINT_MAX=7 \
  -Ihost/usbspike/tusb_stub -I"$TUSB" -Isrc/usbspike \
  host/usbspike/check_usb_descriptors.cpp -o build/usbspike/check_usb_descriptors
exec build/usbspike/check_usb_descriptors "$@"
