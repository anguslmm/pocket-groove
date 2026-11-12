#!/usr/bin/env python3
"""Are two firmware ELFs the same program? Compares every allocated section
byte by byte (contents, addresses, sizes) with the toolchain's objcopy /
readelf, and explains the differences it expects: the build timestamp
(__DATE__ / __TIME__ strings in .flash.rodata) and the app descriptor
(.flash.appdesc: build time and the ELF's own SHA-256).

    python3 host/usbspike/elf_same.py A.elf B.elf

Exit 0 when nothing else differs. Used to show that env:cardputer-adv is
unchanged by the USB audio work (docs/usb-audio.md).
"""
import os
import re
import subprocess
import sys
import tempfile

TOOLS = os.path.expanduser(os.environ.get("PG_XTENSA_BIN", "~/.platformio/packages/toolchain-xtensa-esp-elf/bin"))
READELF = os.path.join(TOOLS, "xtensa-esp32s3-elf-readelf")
OBJCOPY = os.path.join(TOOLS, "xtensa-esp32s3-elf-objcopy")
TS = re.compile(rb"(Jan|Feb|Mar|Apr|May|Jun|Jul|Aug|Sep|Oct|Nov|Dec) [ 0-9]\d \d{4}|\d\d:\d\d:\d\d")


def sections(elf):
    out = subprocess.run([READELF, "-S", "-W", elf], capture_output=True, text=True, check=True).stdout
    res = {}
    for m in re.finditer(r"\]\s+(\S+)\s+(PROGBITS|NOBITS)\s+([0-9a-f]+)\s+[0-9a-f]+\s+([0-9a-f]+)\s+\S+\s+(\S*A\S*)", out):
        res[m.group(1)] = (m.group(2), int(m.group(3), 16), int(m.group(4), 16))
    return res


def content(elf, name, tmp):
    path = os.path.join(tmp, re.sub(r"[^A-Za-z0-9]", "_", name) + os.path.basename(elf) + ".bin")
    subprocess.run([OBJCOPY, "-O", "binary", "--only-section=" + name, elf, path], check=True)
    with open(path, "rb") as f:
        return f.read()


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    a, b = sys.argv[1], sys.argv[2]
    sa, sb = sections(a), sections(b)
    bad = 0
    if set(sa) != set(sb):
        print("section sets differ: only in A %s, only in B %s" % (sorted(set(sa) - set(sb)), sorted(set(sb) - set(sa))))
        bad += 1
    with tempfile.TemporaryDirectory() as tmp:
        for name in sorted(set(sa) & set(sb)):
            if sa[name] != sb[name]:
                print("%-24s type/address/size differ: A %s B %s" % (name, sa[name], sb[name]))
                bad += 1
                continue
            if sa[name][0] == "NOBITS":
                print("%-24s %8d B  NOBITS, same address and size" % (name, sa[name][2]))
                continue
            ca, cb = content(a, name, tmp), content(b, name, tmp)
            diff = [i for i in range(min(len(ca), len(cb))) if ca[i] != cb[i]]
            if not diff and len(ca) == len(cb):
                print("%-24s %8d B  identical" % (name, len(ca)))
                continue
            if name == ".flash.appdesc":
                print("%-24s %8d B  %d bytes differ: app descriptor (build time, ELF SHA-256) -- expected" % (name, len(ca), len(diff)))
                continue
            # every differing byte must sit inside a timestamp string in both images
            explained = True
            stamps = set()
            for i in diff:
                ok = False
                for img in (ca, cb):
                    lo = max(0, i - 11)
                    for m in TS.finditer(img[lo:i + 12]):
                        if lo + m.start() <= i < lo + m.end():
                            ok = True
                            stamps.add((ca[lo + m.start():lo + m.end()], cb[lo + m.start():lo + m.end()]))
                if not ok:
                    explained = False
                    print("%-24s byte 0x%x differs (A %02x B %02x), not a timestamp" % (name, i, ca[i], cb[i]))
                    break
            if explained:
                print("%-24s %8d B  %d bytes differ, all inside __DATE__/__TIME__ strings: %s -- expected"
                      % (name, len(ca), len(diff), ", ".join("%s->%s" % (x.decode(), y.decode()) for x, y in sorted(stamps))))
            else:
                bad += 1
    print("SAME PROGRAM (only build timestamps differ)" if not bad else "DIFFERENT: %d unexplained difference(s)" % bad)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
