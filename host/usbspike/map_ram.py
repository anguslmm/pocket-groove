#!/usr/bin/env python3
"""Static internal RAM by input object, from GNU ld map files (no toolchain
needed, works on a map whose link failed with "region overflowed").

    python3 host/usbspike/map_ram.py A.map [B.map]

One map: DRAM (.dram0.data + .dram0.bss) and IRAM (.iram0.text +
.iram0.vectors) totals and the largest contributors. Two maps: the B-A
difference per object, largest first, and the totals. Used for the RAM
budget of env:cardputer-adv-usb against env:cardputer-adv (docs/usb-audio.md).
IRAM and DRAM share SRAM1 on the ESP32-S3, so both count against the heap
byte for byte (rounded to the linker's alignment).
"""
import re
import sys
from collections import defaultdict

OUT_SECTIONS = {".dram0.data": "dram", ".dram0.bss": "dram", ".iram0.text": "iram", ".iram0.vectors": "iram"}
LINE = re.compile(r"^ (\S+)?\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S.*)$")


def short(obj):
    obj = obj.strip()
    m = re.search(r"([^/]+\.a)\(([^)]+)\)$", obj)
    if m:
        return "%s(%s)" % (m.group(1), m.group(2))
    obj = re.sub(r"^.*?\.pio/build/[^/]+/", "", obj)   # same object, different env
    return re.sub(r"^lib[0-9a-f]{3}/", "lib/", obj)       # PlatformIO's per-env library dir hash


def parse(path):
    sums = {"dram": defaultdict(int), "iram": defaultdict(int)}
    cur = None
    pending = None
    with open(path, errors="replace") as f:
        for raw in f:
            line = raw.rstrip("\n")
            if line and not line.startswith(" "):
                name = line.split()[0]
                cur = OUT_SECTIONS.get(name)
                pending = None
                continue
            if cur is None:
                continue
            m = LINE.match(line)
            if m:
                sec = m.group(1) or pending
                pending = None
                if sec is None or sec.startswith("*") or "load address" in line:
                    continue
                size = int(m.group(3), 16)
                if size and int(m.group(2), 16):
                    sums[cur][short(m.group(4))] += size
                continue
            s = line.strip()
            if s.startswith(".") and len(s.split()) == 1:
                pending = s   # section name on its own line, address/size on the next
    return sums


def main():
    if len(sys.argv) not in (2, 3):
        print(__doc__)
        return 2
    a = parse(sys.argv[1])
    if len(sys.argv) == 2:
        for k in ("dram", "iram"):
            tot = sum(a[k].values())
            print("== %s total %d B" % (k.upper(), tot))
            for obj, n in sorted(a[k].items(), key=lambda kv: -kv[1])[:25]:
                print("  %7d  %s" % (n, obj))
        return 0
    b = parse(sys.argv[2])
    for k in ("dram", "iram"):
        ta, tb = sum(a[k].values()), sum(b[k].values())
        print("== %s: A %d B, B %d B, B-A %+d B" % (k.upper(), ta, tb, tb - ta))
        keys = set(a[k]) | set(b[k])
        d = sorted(((b[k].get(o, 0) - a[k].get(o, 0), o) for o in keys), key=lambda t: -abs(t[0]))
        for n, o in d:
            if n:
                print("  %+7d  %s" % (n, o))
    return 0


if __name__ == "__main__":
    sys.exit(main())
