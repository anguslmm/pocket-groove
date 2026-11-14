#!/bin/sh
# Flash Pocket Groove onto a Daisy Seed over USB (docs/DAISY.md "Flashing from macOS").
#   tools/daisy-flash.sh bootloader   ONCE per board: the Daisy bootloader into the
#                                     STM32's internal flash (the board in the chip's
#                                     own DFU mode: hold BOOT, tap RESET, let go of BOOT)
#   tools/daisy-flash.sh [STAGE]      the firmware (default stage 0) into QSPI through the
#                                     Daisy bootloader. If the firmware already runs, its
#                                     serial port is asked to restart into the bootloader;
#                                     otherwise tap RESET (then the bootloader waits 2.5 s:
#                                     press BOOT during that time to make it wait forever)
#   tools/daisy-flash.sh list         what dfu-util sees
# Needs dfu-util (macOS: brew install dfu-util). Both DFU modes show as USB 0483:df11;
# the script tells them apart by the memory layout dfu-util lists, so the bootloader
# never goes to QSPI and the firmware never to the internal flash. The commands are
# libDaisy's own (core/Makefile program-boot / program-dfu).
set -e
here=$(cd "$(dirname "$0")/.." && pwd)
say() { printf 'daisy-flash: %s\n' "$*"; }

if ! command -v dfu-util >/dev/null 2>&1; then
  say "dfu-util not found. macOS: brew install dfu-util   Debian/Ubuntu: sudo apt install dfu-util"
  exit 1
fi

# Which DFU device is there: "system" (the STM32's ROM: Internal Flash at 0x08000000),
# "daisy" (the Daisy bootloader: Flash at 0x90000000), or "none".
mode() {
  l=$(dfu-util -l 2>/dev/null || true)
  if printf '%s' "$l" | grep -q '0483:df11'; then
    if printf '%s' "$l" | grep -q '0x90000000'; then echo daisy; return; fi
    if printf '%s' "$l" | grep -qi 'Internal Flash'; then echo system; return; fi
    echo unknown
    return
  fi
  echo none
}

wait_mode() {   # wait_mode WANT SECONDS
  n=0
  while [ "$n" -lt "$2" ]; do
    m=$(mode)
    [ "$m" = "$1" ] && return 0
    sleep 1
    n=$((n + 1))
  done
  return 1
}

case "${1:-0}" in
  list)
    dfu-util -l
    exit 0
    ;;
  bootloader)
    boot="$here/daisy/libDaisy/core/dsy_bootloader_v6_4-intdfu-2000ms.bin"
    [ -f "$boot" ] || { say "run tools/daisy-setup.sh first (it fetches libDaisy and the bootloader)"; exit 1; }
    say "waiting for the STM32's DFU mode: hold BOOT, tap RESET, let go of BOOT (60 s)"
    if ! wait_mode system 60; then
      say "no STM32 DFU device (dfu-util -l shows: $(mode)). Use a data USB cable, try again"
      exit 1
    fi
    dfu-util -a 0 -s 0x08000000:leave -D "$boot" -d ,0483:df11
    say "done: the LED should now pulse slowly for 2.5 s at each power-on (the bootloader's grace period)"
    say "next: tools/daisy-flash.sh   (while it pulses, or any time later)"
    exit 0
    ;;
  0 | 1)
    stage=${1:-0}
    ;;
  *)
    sed -n '2,15p' "$0"
    exit 2
    ;;
esac

bin="$here/build/daisy/stage$stage/pocket-groove-daisy.bin"
if [ ! -f "$bin" ]; then
  say "no $bin: building it (make -C daisy STAGE=$stage)"
  make -C "$here/daisy" STAGE="$stage" -j4 >/dev/null
fi
size=$(wc -c < "$bin" | tr -d ' ')
say "firmware: $bin ($size bytes)"

if [ "$(mode)" != "daisy" ]; then
  # The firmware may be running: ask it (serial 'B' then 'y') to restart into the
  # bootloader, which then waits for DFU without a time limit.
  port=${PG_PORT:-$(ls /dev/cu.usbmodem* /dev/ttyACM* 2>/dev/null | head -n 1)}
  if [ -n "$port" ] && [ -e "$port" ]; then
    say "asking the firmware on $port to restart into the Daisy bootloader"
    stty -f "$port" 115200 2>/dev/null || stty -F "$port" 115200 2>/dev/null || true
    printf 'B' > "$port" 2>/dev/null || true
    sleep 0.3
    printf 'y' > "$port" 2>/dev/null || true
  else
    say "tap RESET on the Seed (the LED pulses: the bootloader is listening; press BOOT then to keep it waiting)"
  fi
  if ! wait_mode daisy 30; then
    m=$(mode)
    if [ "$m" = "system" ]; then
      say "this is the STM32's own DFU mode (BOOT was held): the Daisy bootloader is not installed,"
      say "or the board was started with BOOT held. First time: tools/daisy-flash.sh bootloader"
    else
      say "the Daisy bootloader did not appear (dfu-util: $m). Tap RESET and run this again within 2 s"
    fi
    exit 1
  fi
fi
# The image (480 KB) and the ROM part go to 0x90040000 in one file. :leave starts it.
dfu-util -a 0 -s 0x90040000:leave -D "$bin" -d ,0483:df11
say "done: the LED goes steady, then double-flashes every second (running); in stage 0 the demo plays"
say "then: tools/pg-daisy report     (the serial port appears as /dev/cu.usbmodem*)"
