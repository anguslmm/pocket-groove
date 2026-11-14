#!/bin/sh
# Pocket Groove on the Daisy Seed: fetch and build libDaisy at the pinned commit.
#   tools/daisy-setup.sh            clone (once) + checkout + build libdaisy.a
#   tools/daisy-setup.sh --check    only say whether everything is in place
# libDaisy lands in daisy/libDaisy (ignored by git), at the commit in
# daisy/libdaisy.lock, with only the submodules the firmware needs (not
# googletest). Nothing outside daisy/ is touched; the Cardputer and desktop builds
# do not use it. Needs git, make and the Arm toolchain (docs/DAISY.md "Setting up":
# macOS  brew install --cask gcc-arm-embedded ; Debian/Ubuntu  apt install
# gcc-arm-none-eabi libnewlib-arm-none-eabi libstdc++-arm-none-eabi-newlib).
set -e
here=$(cd "$(dirname "$0")/.." && pwd)
lock="$here/daisy/libdaisy.lock"
dir="$here/daisy/libDaisy"
url=$(sed -n 's/^LIBDAISY_URL=//p' "$lock")
commit=$(sed -n 's/^LIBDAISY_COMMIT=//p' "$lock")
subs="Drivers/CMSIS_5 Drivers/CMSIS-Device/ST/STM32H7xx Drivers/STM32H7xx_HAL_Driver Middlewares/ST/STM32_USB_Device_Library Drivers/CMSIS-DSP"

say() { printf 'daisy-setup: %s\n' "$*"; }

if ! command -v arm-none-eabi-gcc >/dev/null 2>&1; then
  say "arm-none-eabi-gcc not found."
  say "  macOS:          brew install --cask gcc-arm-embedded   (then open a new terminal)"
  say "  Debian/Ubuntu:  sudo apt install gcc-arm-none-eabi libnewlib-arm-none-eabi libstdc++-arm-none-eabi-newlib"
  exit 1
fi

if [ "$1" = "--check" ]; then
  if [ -f "$dir/build/libdaisy.a" ] && [ "$(git -C "$dir" rev-parse HEAD 2>/dev/null)" = "$commit" ] &&
     [ "$(cat "$dir/build/pg-opt" 2>/dev/null)" = "-Os" ]; then
    say "libDaisy $commit built: ok"
    exit 0
  fi
  say "libDaisy missing, at another commit, or not built: run tools/daisy-setup.sh"
  exit 1
fi

if [ ! -d "$dir/.git" ]; then
  say "cloning $url into daisy/libDaisy"
  git clone --quiet "$url" "$dir"
fi
if [ "$(git -C "$dir" rev-parse HEAD)" != "$commit" ]; then
  git -C "$dir" fetch --quiet origin
  git -C "$dir" checkout --quiet "$commit"
  rm -rf "$dir/build"
fi
say "libDaisy at $(git -C "$dir" log -1 --format='%h %cd' --date=short)"
# shellcheck disable=SC2086
git -C "$dir" submodule update --init --quiet $subs
jobs=$( (getconf _NPROCESSORS_ONLN || sysctl -n hw.ncpu || echo 4) 2>/dev/null | head -n 1)
# libDaisy is built for size (-Os; its own Makefile says -O3): nearly all of it is
# main-loop code (HAL, USB, FatFS), and -Os leaves ~20 KB more of the 480 KB image
# for stage 1. Its only audio-path code is the per-block float conversion. The
# marker file records the choice; daisy/Makefile refuses a library built without it.
opt="-Os"
if [ -f "$dir/build/libdaisy.a" ] && [ "$(cat "$dir/build/pg-opt" 2>/dev/null)" != "$opt" ]; then
  say "libdaisy.a was built with other flags: rebuilding it with $opt"
  rm -rf "$dir/build"
fi
if [ ! -f "$dir/build/libdaisy.a" ]; then
  say "building libdaisy.a with $opt (about a minute)"
  make -C "$dir" -j"$jobs" OPT="$opt" >"$dir/build.log" 2>&1 || { tail -n 30 "$dir/build.log"; say "libDaisy build failed (log: daisy/libDaisy/build.log)"; exit 1; }
  echo "$opt" > "$dir/build/pg-opt"
fi
# Bosch's BMI270 configuration file for stage 1's tilt (optional: without it the IMU is off).
burl=$(sed -n 's/^BMI270_URL=//p' "$lock")
bcommit=$(sed -n 's/^BMI270_COMMIT=//p' "$lock")
bdir="$here/daisy/vendor/BMI270_SensorAPI"
if [ ! -f "$here/daisy/vendor/bmi270_config.inc" ]; then
  if { [ -d "$bdir/.git" ] || git clone --quiet "$burl" "$bdir"; } && git -C "$bdir" checkout --quiet "$bcommit"; then
    python3 "$here/tools/daisy_bmi270_blob.py" "$bdir/bmi270.c" "$here/daisy/vendor" || say "BMI270 file not extracted (tilt stays off)"
  else
    say "could not fetch $burl (only stage 1's tilt needs it)"
  fi
fi
say "ready: $(ls -l "$dir/build/libdaisy.a" | awk '{print $5}') bytes in daisy/libDaisy/build/libdaisy.a"
say "next:  make -C daisy        (then tools/daisy-flash.sh, docs/DAISY.md)"
