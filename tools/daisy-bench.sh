#!/bin/sh
# Compare build variants of the Daisy firmware on the board (docs/DAISY.md section 11):
# for each variant, build it, flash it, run the on-device bench (tools/pg-daisy bench),
# and at the end print one table of all of them. The board must be running Pocket
# Groove (any build) and be on USB; the script restarts it into the bootloader itself.
#
#   tools/daisy-bench.sh                     the default list, most likely win first
#   tools/daisy-bench.sh base itcm           only these
#   tools/daisy-bench.sh --stage 1 ...       stage 1 builds (default stage 0)
#   tools/daisy-bench.sh --quick ...         short bench spans (a quick look, a few s each)
#
# Variants (daisy/Makefile options):
#   base         today's build
#   itcm         ITCM=1: the hot audio functions run from ITCM
#   itcm-align   ITCM=1 ALIGN=1: and the audio path aligned to cache lines
#   itcm-O3part  ITCM=1 AUDIO_OPT=-O3 AUDIO_OPT_FILES="lead/lead drums/%": the lead and the
#                drums at -O3
#   dtcm         TABLES=dtcm: the bass's sine and BLEP tables in DTCM
#   align        ALIGN=1 alone
# Each run's [bench] lines go to build/daisy/bench/<variant>.txt (paste those back, or
# the final table). The board is left running the LAST variant: flash the one you want
# afterwards (tools/daisy-flash.sh flashes the base build).
#
# A variant that does not boot: the script says so and goes on to the next one; the
# next flash brings the board back (a crash resets through the bootloader, which
# listens for 2.5 s each time; a hang needs a tap on RESET when the flash script asks).
set -u
here=$(cd "$(dirname "$0")/.." && pwd)
stage=0
quick=""
variants=""
while [ $# -gt 0 ]; do
  case "$1" in
    --stage) stage=$2; shift 2 ;;
    --quick) quick=quick; shift ;;
    -h|--help) sed -n '2,27p' "$0"; exit 0 ;;
    *) variants="$variants $1"; shift ;;
  esac
done
[ -n "$variants" ] || variants="base itcm itcm-align itcm-O3part dtcm"

flags() {
  case "$1" in
    base) echo "" ;;
    itcm) echo "ITCM=1" ;;
    itcm-align) echo "ITCM=1 ALIGN=1" ;;
    itcm-O3part) echo "ITCM=1 AUDIO_OPT=-O3" ;;   # AUDIO_OPT_FILES is passed apart (it has a space)
    dtcm) echo "TABLES=dtcm" ;;
    align) echo "ALIGN=1" ;;
    *) return 1 ;;
  esac
}
dir() {   # the variant's build directory (daisy/Makefile BUILD)
  if [ "$1" = base ]; then echo "$here/build/daisy/stage$stage"; else echo "$here/build/daisy/stage$stage-$1"; fi
}
say() { printf 'daisy-bench: %s\n' "$*"; }

for v in $variants; do
  flags "$v" >/dev/null || { say "unknown variant $v (base itcm itcm-align itcm-O3part dtcm align)"; exit 2; }
done
out="$here/build/daisy/bench"
mkdir -p "$out"
files=""
for v in $variants; do
  say "=== $v: building ($(flags "$v"))"
  if [ "$v" = itcm-O3part ]; then
    make -C "$here/daisy" -j4 STAGE="$stage" $(flags "$v") "AUDIO_OPT_FILES=lead/lead drums/%" > "$out/$v.build.txt" 2>&1
  else
    make -C "$here/daisy" -j4 STAGE="$stage" $(flags "$v") > "$out/$v.build.txt" 2>&1
  fi
  if [ $? -ne 0 ]; then
    say "$v did not build: see $out/$v.build.txt"
    continue
  fi
  grep 'daisy_size:' "$out/$v.build.txt" | sed 's/^/daisy-bench:   /'
  bin="$(dir "$v")/pocket-groove-daisy.bin"
  say "$v: flashing $bin"
  if ! "$here/tools/daisy-flash.sh" "$bin"; then
    say "$v: flashing failed; next variant"
    continue
  fi
  # The board restarts: wait for its serial port to answer (it reports in a few s).
  ok=""
  i=0
  while [ $i -lt 30 ]; do
    sleep 1
    if "$here/tools/pg-daisy" report > "$out/$v.report.txt" 2>&1; then ok=1; break; fi
    i=$((i + 1))
  done
  if [ -z "$ok" ]; then
    say "$v DID NOT ANSWER within 30 s after flashing (LED steady or fast-blinking: it did not boot)."
    say "  Nothing is lost: the next variant's flash brings the board back. To go back by hand:"
    say "    tools/daisy-flash.sh"
    say "  (tap RESET when it asks; pressing BOOT while the LED pulses keeps the bootloader waiting)"
    continue
  fi
  grep -E '^\[crash\]|^\[board\]' "$out/$v.report.txt" | sed 's/^/daisy-bench:   /'
  sleep 2   # let the boot settle (the first blocks, the session load)
  say "$v: bench (about a minute, the sound stops, the board restarts at the end)"
  if "$here/tools/pg-daisy" --file "$out/$v.txt" bench $quick > /dev/null; then
    files="$files $out/$v.txt"
    say "$v: done -> $out/$v.txt"
  else
    say "$v: the bench did not finish (see $out/$v.txt)"
    [ -s "$out/$v.txt" ] && files="$files $out/$v.txt"
  fi
  sleep 5   # the board restarts after its bench
done

if [ -z "$files" ]; then
  say "no bench results"
  exit 1
fi
echo
"$here/tools/pg-daisy" compare $files | tee "$out/compare.txt"
echo
say "the table is in $out/compare.txt; each run's lines in $out/<variant>.txt"
say "the board now runs the last variant: tools/daisy-flash.sh   puts the base build back"
