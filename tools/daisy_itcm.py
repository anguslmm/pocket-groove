#!/usr/bin/env python3
"""tools/daisy_itcm.py: choose the functions that go into the Daisy's ITCM (make -C daisy
ITCM=1; docs/DAISY.md section 11 "Build variants").

    python3 tools/daisy_itcm.py PROFILE ELF OUT.ld [--budget BYTES] [--nm NM] [--objdump OBJDUMP]

PROFILE is daisy/itcm_profile.txt: each function's self cost in x86-64 instructions
per frame, summed over the bench scenes (host/daisy/daisy_bench_ref --profile, the
same scenes as the on-device bench). ELF is the firmware linked WITHOUT ITCM (the
Makefile's first pass), whose symbol sizes are the ARM ones. The choice:
  1. every profiled function that exists in the ARM build, by cost per byte (the
     hottest code per byte of ITCM first), until the budget is used;
  2. then the ARM functions those call (bl / b.w in the disassembly) that are not in
     it yet, up to 4 KB each: the host inlines differently, so a function the x86
     build inlined (its cost counted in the caller) can be a real call on the ARM, and
     leaving it in AXI SRAM would cost a veneer and an I-cache miss on every call.
memcpy and memset are never chosen: the startup code may call them before the ITCM
is loaded (daisy/src/memory.cpp loads it in the preinit hook).
OUT.ld gets one input-section line per function (`*(.text.NAME)`, the
-ffunction-sections name), for the linker script's .itcm_text (daisy/pg_daisy.lds
INCLUDEs it), and a header with what it covers: the share of the profile's per-frame
cost that the chosen functions carry, on the host's count.
"""
import argparse
import re
import subprocess
import sys

NEVER = {"memcpy", "memset", "memmove", "__aeabi_memcpy", "__aeabi_memset", "Reset_Handler"}
SUFFIX = re.compile(r"\.(part|constprop|isra|cold|lto_priv)\.\d+$|\.(cold)$")


def base(name):
    prev = None
    while prev != name:
        prev = name
        name = SUFFIX.sub("", name)
    return name


def read_profile(path):
    prof = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            ir, name = line.split(None, 1)
            prof.append((float(ir), name.strip()))
    return prof


def arm_symbols(nm, elf):
    """name -> (address, size) of the ELF's functions (text symbols)."""
    out = subprocess.run([nm, "-S", elf], capture_output=True, text=True, check=True).stdout
    syms = {}
    for line in out.splitlines():
        p = line.split()
        if len(p) != 4 or p[2] not in ("t", "T", "W", "w"):
            continue
        syms[p[3]] = (int(p[0], 16), int(p[1], 16))
    return syms


def calls(objdump, elf, names):
    """name -> set of the functions it calls (bl / b.w / b targets in other functions)."""
    out = subprocess.run([objdump, "-d", "--no-show-raw-insn", elf], capture_output=True, text=True,
                         check=True).stdout
    cur, res = None, {}
    head = re.compile(r"^[0-9a-f]+ <(.+)>:$")
    branch = re.compile(r"\s(bl|b\.w|b)\s+[0-9a-f]+ <([^+>]+)>")
    for line in out.splitlines():
        m = head.match(line)
        if m:
            cur = m.group(1) if m.group(1) in names else None
            if cur:
                res[cur] = set()
            continue
        if cur:
            m = branch.search(line)
            if m and m.group(2) != cur:
                res[cur].add(m.group(2))
    return res


def choose(prof, syms, budget, callgraph=None):
    """The greedy choice (see the top). Returns (chosen names in order, covered Ir,
    total Ir, used bytes, notes)."""
    cost = lambda size: (size + 3) // 4 * 4 + 8   # alignment, and room for a veneer
    total = sum(ir for ir, _ in prof)
    by_base = {}
    for n in syms:
        by_base.setdefault(base(n), []).append(n)
    cand = {}
    for ir, name in prof:
        targets = [name] if name in syms else by_base.get(base(name), [])
        for t in targets:
            if t in NEVER:
                continue
            cand[t] = cand.get(t, 0.0) + ir / len(targets)
    order = sorted(cand, key=lambda n: -cand[n] / max(1, syms[n][1]))
    chosen, used, covered = [], 0, 0.0
    for n in order:
        c = cost(syms[n][1])
        if used + c > budget:
            continue
        chosen.append(n)
        used += c
        covered += cand[n]
    added = 0
    if callgraph is not None:
        queue = list(chosen)
        seen = set(chosen)
        while queue:
            n = queue.pop(0)
            for callee in sorted(callgraph(n)):
                if callee in seen or callee not in syms or callee in NEVER:
                    continue
                seen.add(callee)
                size = syms[callee][1]
                if size == 0 or size > 4096 or used + cost(size) > budget:
                    continue
                chosen.append(callee)
                used += cost(size)
                added += 1
                queue.append(callee)
    return chosen, covered, total, used, added


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("profile")
    ap.add_argument("elf")
    ap.add_argument("out")
    ap.add_argument("--budget", type=int, default=60 * 1024,
                    help="bytes of ITCM to fill (default 60 KB of the 63 KB the linker script gives)")
    ap.add_argument("--nm", default="arm-none-eabi-nm")
    ap.add_argument("--objdump", default="arm-none-eabi-objdump")
    a = ap.parse_args(argv)
    prof = read_profile(a.profile)
    syms = arm_symbols(a.nm, a.elf)
    graph = {}

    def callgraph(n):
        if not graph:
            # one disassembly pass for everything the closure may visit (cheap enough)
            graph.update(calls(a.objdump, a.elf, set(syms)))
        return graph.get(n, set())

    chosen, covered, total, used, added = choose(prof, syms, a.budget, callgraph)
    pct = 100.0 * covered / total if total else 0.0
    with open(a.out, "w") as f:
        f.write("/* Generated by tools/daisy_itcm.py from %s and %s: do not edit.\n" % (a.profile, a.elf))
        f.write("   %d functions, %d bytes with veneer room, of a %d-byte budget; %d of them callees added\n"
                % (len(chosen), used, a.budget, added))
        f.write("   after the profiled set. They carry %.1f %% of the profile's per-frame cost (host count:\n"
                "   %.0f of %.0f x86 instructions per frame summed over the bench scenes). */\n" % (pct, covered, total))
        for n in chosen:
            f.write("*(.text.%s)\n" % n)
    print("daisy_itcm: %d functions (%d added as callees), %.1f KB, covering %.1f %% of the profiled per-frame cost"
          % (len(chosen), added, used / 1024.0, pct))
    return 0


if __name__ == "__main__":
    sys.exit(main())
