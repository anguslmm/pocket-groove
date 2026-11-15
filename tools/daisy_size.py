#!/usr/bin/env python3
"""The Daisy firmware's memory map from a real link (docs/DAISY.md "Memory map").

    python3 tools/daisy_size.py ELF MAP            the report
    python3 tools/daisy_size.py --check ELF IMAGE ROM   the .bin's sanity checks (make)

The report attributes every input section in the linker map to a component (the
lib/pg directory, daisy/core, daisy/app, daisy/src, libDaisy, the C/C++ runtime) and
to the memory region its address lies in, split into code (.text), constants
(.rodata), initialised data (.data: in the image AND in RAM) and zeroed data (.bss
and the NOLOAD sections). Then the regions: used, size, free. Standard library only.
"""
import os
import re
import struct
import subprocess
import sys

REGIONS = [
    ("ITCM", 0x00000000, 64 * 1024),
    ("DTCM", 0x20000000, 128 * 1024),
    ("AXI SRAM", 0x24000000, 512 * 1024),
    ("D2 SRAM", 0x30000000, 288 * 1024),
    ("D3 SRAM", 0x38000000, 64 * 1024),
    ("BACKUP", 0x38800000, 4 * 1024),
    ("QSPI", 0x90000000, 8 * 1024 * 1024),
    ("SDRAM", 0xC0000000, 64 * 1024 * 1024),
]
IMAGE_MAX = 0x78000


def region(addr):
    for name, base, size in REGIONS:
        if base <= addr < base + size:
            return name
    return "other"


def component(path):
    p = path.replace("\\", "/")
    m = re.search(r"/obj/lib/pg/src/pg/([a-z0-9_]+)/", p)
    if m:
        return "pg/" + m.group(1)
    for key, name in (("/obj/daisy/core/", "daisy/core"), ("/obj/daisy/app/", "daisy/app"),
                      ("/obj/daisy/src/", "daisy/src"), ("startup_stm32h750xx", "startup")):
        if key in p:
            return name
    if "libdaisy.a" in p:
        return "libDaisy"
    for lib in ("libc_nano.a", "libc.a", "libm.a", "libstdc++", "libsupc++", "libgcc.a", "libnosys"):
        if lib in p:
            return "C/C++ runtime"
    return "other"


def kind(sec):
    if sec.startswith(".text") or sec.startswith(".glue") or sec in (".init", ".fini"):
        return "text"
    if sec.startswith(".rodata") or sec.startswith(".qspi_rom") or sec.startswith(".ARM.ex"):
        return "rodata"
    if sec.startswith(".data") or sec.startswith(".init_array") or sec.startswith(".fini_array") \
            or sec.startswith(".preinit_array") or sec.startswith(".isr_vector"):
        return "data"
    return "bss"


def parse_map(path):
    """Yields (output section, input section, address, size, file) from a GNU ld map."""
    out = None
    pending = None
    started = False
    with open(path, errors="replace") as f:
        for line in f:
            if line.startswith("Linker script and memory map"):
                started = True
                continue
            if not started:
                continue
            if line.startswith("Cross Reference Table"):
                break
            m = re.match(r"^(\.\S+)\s*(0x[0-9a-f]+)?", line)
            if m and not line.startswith(" "):
                out = m.group(1)
                continue
            if pending is not None:
                m = re.match(r"^\s+(0x[0-9a-f]+)\s+(0x[0-9a-f]+)\s+(\S.*)$", line)
                if m:
                    yield out, pending, int(m.group(1), 16), int(m.group(2), 16), m.group(3).strip()
                pending = None
                continue
            m = re.match(r"^ (\.\S+)\s+(0x[0-9a-f]+)\s+(0x[0-9a-f]+)\s+(\S.*)$", line)
            if m:
                yield out, m.group(1), int(m.group(2), 16), int(m.group(3), 16), m.group(4).strip()
                continue
            m = re.match(r"^ (\.\S+)\s*$", line)
            if m:
                pending = m.group(1)


def clipped(rows):
    """Merged string sections (.rodata.str1.*) are listed per input file with their size
    before merging, overlapping each other (often all at the merged block's address): of
    the entries at one address only the largest counts, clipped at the next entry with a
    higher address, so the totals are what the output really holds."""
    rows = [r for r in rows if r[3] > 0]
    by_out = {}
    for r in rows:
        by_out.setdefault(r[0], []).append(r)
    for out, rs in by_out.items():
        rs.sort(key=lambda r: (r[2], -r[3]))
        for i, (o, sec, addr, size, fname) in enumerate(rs):
            if i > 0 and rs[i - 1][2] == addr:
                size = 0               # the largest entry at this address already counts
            else:
                j = i + 1
                while j < len(rs) and rs[j][2] == addr:
                    j += 1
                nxt = rs[j][2] if j < len(rs) else addr + size
                if nxt < addr + size:
                    size = max(0, nxt - addr)
            yield o, sec, addr, size, fname


def symbols(elf):
    nm = os.environ.get("NM", "arm-none-eabi-nm")
    out = subprocess.run([nm, elf], capture_output=True, text=True, check=True).stdout
    syms = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3:
            syms[parts[2]] = int(parts[0], 16)
    return syms


def kb(n):
    return "%.1f KB" % (n / 1024.0)


def report(elf, mapfile):
    table = {}
    regions_used = {}
    for out, sec, addr, size, fname in clipped(parse_map(mapfile)):
        if size == 0 or out in (".ARM.attributes", ".comment", ".debug_info") or out.startswith(".debug"):
            continue
        if out.startswith(".debug") or sec.startswith(".debug") or sec == ".comment":
            continue
        comp = component(fname)
        k = "rom" if out == ".qspi_rom" else kind(sec)
        reg = region(addr)
        table.setdefault(comp, {}).setdefault(k, 0)
        table[comp][k] += size
        key = (out, reg)
        regions_used[key] = regions_used.get(key, 0) + size
    comps = sorted(table, key=lambda c: -sum(table[c].values()))
    print("Per component (bytes): text = code, rodata = constants (both in the image), rom = constants in the "
          "ROM part (QSPI, copied to SDRAM), data = initialised RAM (also in the image), bss = zeroed / "
          "uninitialised RAM (all regions)")
    print("  %-16s %9s %9s %9s %9s %9s" % ("component", "text", "rodata", "rom", "data", "bss"))
    ks = ("text", "rodata", "rom", "data", "bss")
    tot = dict((k, 0) for k in ks)
    for c in comps:
        row = table[c]
        print("  %-16s %9d %9d %9d %9d %9d" % ((c,) + tuple(row.get(k, 0) for k in ks)))
        for k in ks:
            tot[k] += row.get(k, 0)
    print("  %-16s %9d %9d %9d %9d %9d" % (("total",) + tuple(tot[k] for k in ks)))
    print()
    print("Output sections -> memory regions (VMA; .qspi_rom is loaded from QSPI 0x900B8000):")
    for (out, reg), size in sorted(regions_used.items(), key=lambda kv: (kv[0][1], -kv[1])):
        print("  %-20s %-9s %10d  (%s)" % (out, reg, size, kb(size)))
    print()
    s = symbols(elf)
    image = s["_pg_image_end"] - s["_pg_image_start"]
    rom = s["_eqspi_rom"] - s["_sqspi_rom"]
    dtcm_static = s["_edtcmram_bss"] - 0x20000000
    stack = s["_estack"] - s["_edtcmram_bss"]
    axi = s["_eaxi_bss"] - s["_saxi_bss"]
    d2dma = s["_esram1_bss"] - s["_ssram1_bss"]
    d2 = s["_ed2_bss"] - s["_sd2_bss"]
    sdram = s["_esdram_bss"] - 0xC0000000
    print("Regions:")
    print("  image (AXI SRAM, copied by the bootloader) %s of 480 KB, %s free" % (kb(image), kb(IMAGE_MAX - image)))
    print("  AXI SRAM after the image                    %s of statics, %s free (of 512 KB)" %
          (kb(axi), kb(0x24080000 - s["_eaxi_bss"])))
    print("  DTCM statics (.data + .bss + .dtcmram_bss)  %s; stack %s (the rest of the 128 KB)" %
          (kb(dtcm_static), kb(stack)))
    print("  D2 SRAM: DMA (not cached) %s of 32 KB; statics %s, %s free of 256 KB" %
          (kb(d2dma), kb(d2), kb(0x30048000 - s["_pg_d2_end"])))
    print("  SDRAM: ROM copy %s + buffers %s = %s, %s free of 64 MB" %
          (kb(rom), kb(s["_esdram_bss"] - s["_ssdram_bss"]), kb(sdram), kb(64 * 1024 * 1024 - sdram)))
    print("  QSPI: image 0x90040000 %s, ROM 0x900B8000 %s (of 288 KB), /int 0x90100000 2 MB, pool 0x90300000 3.2 MB" %
          (kb(image), kb(rom)))


def check(elf, image, rom):
    s = symbols(elf)
    ok = True
    isz = os.path.getsize(image)
    if isz != IMAGE_MAX:
        print("daisy_size: image.bin is %d bytes, must be exactly %d (0x78000)" % (isz, IMAGE_MAX))
        ok = False
    with open(image, "rb") as f:
        sp, reset = struct.unpack("<II", f.read(8))
    if sp != s["_estack"] or not (0x24000000 <= reset < 0x24078000) or not reset & 1:
        print("daisy_size: vector table looks wrong: sp 0x%08x reset 0x%08x" % (sp, reset))
        ok = False
    rsz = os.path.getsize(rom)
    if rsz != s["_eqspi_rom"] - s["_sqspi_rom"] or rsz > 288 * 1024:
        print("daisy_size: rom.bin is %d bytes (section %d, max %d)" % (rsz, s["_eqspi_rom"] - s["_sqspi_rom"],
                                                                         288 * 1024))
        ok = False
    used = s["_pg_image_end"] - s["_pg_image_start"]
    print("daisy_size: image %d of %d bytes (%.1f KB free), ROM part %d bytes, vectors sp 0x%08x reset 0x%08x: %s" %
          (used, IMAGE_MAX, (IMAGE_MAX - used) / 1024.0, rsz, sp, reset, "ok" if ok else "FAILED"))
    return 0 if ok else 1


def main(argv):
    if len(argv) == 5 and argv[1] == "--check":
        return check(argv[2], argv[3], argv[4])
    if len(argv) == 3:
        report(argv[1], argv[2])
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
