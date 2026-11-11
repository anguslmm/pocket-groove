#!/usr/bin/env python3
"""RAM / flash cost of the USB audio stack: diff two firmware ELFs symbol by symbol.

    python3 host/usbspike/ram_diff.py .pio/build/usb-audio-spike-nousb/firmware.elf \
                                      .pio/build/usb-audio-spike/firmware.elf

Prints the static DRAM (.data + .bss) and IRAM/flash text that the second image
has and the first has not, grouped by the object file each symbol came from
(read from the linker map next to the ELF), plus the biggest single symbols.
Uses the toolchain's own nm (xtensa-esp-elf-nm from ~/.platformio/packages), never
a host binutils, so it gives the same answer on macOS and Linux. Python 3 stdlib only.
"""
import collections
import os
import re
import shutil
import subprocess
import sys

DRAM = (0x3FC88000, 0x3FD00000)    # ESP32-S3 internal DRAM (data bus view of SRAM1/2)
IRAM = (0x40370000, 0x403E0000)    # internal IRAM
FLASH_TEXT = (0x42000000, 0x44000000)


def find_nm():
    env = os.environ.get("XTENSA_NM")
    if env:
        return env
    base = os.path.expanduser("~/.platformio/packages/toolchain-xtensa-esp-elf/bin")
    for name in ("xtensa-esp32s3-elf-nm", "xtensa-esp-elf-nm"):
        p = os.path.join(base, name)
        if os.path.exists(p):
            return p
    p = shutil.which("xtensa-esp32s3-elf-nm") or shutil.which("xtensa-esp-elf-nm")
    if p:
        return p
    sys.exit("xtensa nm not found: set XTENSA_NM or build once with PlatformIO")


def region(addr):
    if DRAM[0] <= addr < DRAM[1]:
        return "dram"
    if IRAM[0] <= addr < IRAM[1]:
        return "iram"
    if FLASH_TEXT[0] <= addr < FLASH_TEXT[1]:
        return "flash"
    return None


def symbols(elf):
    out = subprocess.run([find_nm(), "-S", "--defined-only", elf], capture_output=True, text=True, check=True).stdout
    syms = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) < 4:
            continue
        addr, size, typ, name = int(parts[0], 16), int(parts[1], 16), parts[2], " ".join(parts[3:])
        r = region(addr)
        if r is None or size == 0:
            continue
        if r == "dram" and typ.lower() not in ("b", "d", "r", "v", "s"):
            continue
        syms[(name, r)] = (addr, size)
    return syms


def owners(mapfile):
    """symbol address -> object file, from the GNU ld map (sections of -ffunction/-fdata-sections)."""
    own = {}
    if not os.path.exists(mapfile):
        return own
    sec = re.compile(r"^\s*(\.[\w.$]+)?\s*(0x[0-9a-f]+)\s+(0x[0-9a-f]+)\s+(\S+\.(?:o|obj|a\(.+\)))\s*$")
    pending = None
    with open(mapfile, errors="replace") as fh:
        for line in fh:
            m = sec.match(line)
            if m:
                a, sz, obj = int(m.group(2), 16), int(m.group(3), 16), m.group(4)
                if sz:
                    own[a] = obj
                continue
            # a long section name puts the address on the next line
            if re.match(r"^\s*\.[\w.$]+\s*$", line):
                pending = True
    return own


def short(obj):
    obj = obj.replace("\\", "/")
    m = re.search(r"([^/]+\.a)\((.+)\)$", obj)
    if m:
        return "%s(%s)" % (m.group(1), m.group(2))
    return obj.split("/.pio/build/")[-1]


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    base, new = sys.argv[1], sys.argv[2]
    for f in (base, new):
        if not os.path.isfile(f):
            sys.exit("%s not found: build both envs first (pio run -e usb-audio-spike -e usb-audio-spike-nousb;\n"
                     "PlatformIO wipes .pio/build when the flags change, so build them in one command)" % f)
    a, b = symbols(base), symbols(new)
    own = owners(os.path.splitext(new)[0] + ".map")
    own_addrs = sorted(own)

    def owner_of(addr):
        import bisect
        i = bisect.bisect_right(own_addrs, addr) - 1
        return short(own[own_addrs[i]]) if i >= 0 else "?"

    for reg, title in (("dram", "static DRAM (.data/.bss)"), ("iram", "IRAM text"), ("flash", "flash text")):
        added = [(k[0], v) for k, v in b.items() if k[1] == reg and k not in a]
        removed = [(k[0], v) for k, v in a.items() if k[1] == reg and k not in b]
        grown = [(k[0], (v[0], v[1] - a[k][1])) for k, v in b.items() if k[1] == reg and k in a and v[1] != a[k][1]]
        total = sum(s for _, (_, s) in added) - sum(s for _, (_, s) in removed) + sum(s for _, (_, s) in grown)
        print("== %s: %+d bytes (%d symbols added, %d removed, %d resized)" % (title, total, len(added), len(removed), len(grown)))
        by_obj = collections.Counter()
        for name, (addr, size) in added + grown:
            by_obj[owner_of(addr)] += size
        for obj, size in by_obj.most_common(25 if reg == "dram" else 12):
            print("   %7d  %s" % (size, obj))
        if reg == "dram":
            print("   biggest symbols:")
            for name, (addr, size) in sorted(added + grown, key=lambda x: -x[1][1])[:25]:
                print("   %7d  %-40s %s" % (size, name[:40], owner_of(addr)))
            if removed:
                print("   only in the baseline: %d bytes (%s)" % (
                    sum(s for _, (_, s) in removed), ", ".join(n for n, _ in sorted(removed, key=lambda x: -x[1][1])[:6])))


if __name__ == "__main__":
    main()
