# PlatformIO extra script (pre:) for env:cardputer-adv and the profile envs.
#
# Places the audio hot path in internal RAM without touching lib/pg sources
# and without rebuilding the ESP-IDF libraries:
#
#   After each lib/pg object is compiled (-ffunction-sections, the framework
#   default), its per-function sections are renamed with objcopy:
#     .text.<fn>     -> .iram1.pg.<fn>      (code)
#     .literal.<fn>  -> .iram1.pgl.<fn>     (its L32R literal pool: must stay
#                                            within reach of the code, i.e. in IRAM too)
#     .rodata.<obj>  -> .dram1.pg.<obj>     (only for the objects in RAM_RODATA)
#   The prebuilt sections.ld already collects `*(.iram1 .iram1.*)` into
#   .iram0.text and `*(.dram1 .dram1.*)` into .dram0.data (that is how IRAM_ATTR /
#   DRAM_ATTR work), so nothing else is needed. IRAM and DRAM share SRAM1 on the
#   ESP32-S3 (the heap starts after _iram_end), so every byte moved costs one
#   byte of heap.
#
# Which functions: the demangled name is matched against HOT (per-quantum work:
# render loops, mixer, FX, master, sequencer tick) and, with tier >= 2, STEP
# (per-step work: noteOn, fireTrackStep, ...). Everything else (init, sanitize,
# commands, the stereo-only template variants that device mode never runs, the
# 16 bass loop variants of which one is active) stays in flash.
#
# Selected by -DPG_AUDIO_RAM=<tier> in build_flags (0/absent = off).
# Also gives lib/pg the same warning set as src/ (what src/app/pio_pg_flags.py
# does for the stock env; an env can only have one such middleware).
import re
import subprocess

Import("env")  # noqa: F821

PG_WARNINGS = ["-Wall", "-Wextra", "-Wdouble-promotion", "-Wfloat-conversion"]


def _tier():
    for d in env.get("CPPDEFINES", []):  # noqa: F821
        if isinstance(d, (list, tuple)) and d[0] == "PG_AUDIO_RAM":
            return int(d[1])
    for f in env.get("BUILD_FLAGS", []):  # noqa: F821
        m = re.match(r"-DPG_AUDIO_RAM=(\d+)", str(f))
        if m:
            return int(m.group(1))
    return 0


TIER = _tier()
print("[audio-ram] PG_AUDIO_RAM tier %d" % TIER)

# Per-quantum hot set (device = mono mode). Regexes on demangled names.
HOT = [
    r"^pg::Engine::process\(",
    r"^pg::engine_detail::State::(render|quantum|drainCommands|ensureParams|publishStatus|sequencerTick|rollTick)\(",
    r"^pg::Voice::mono\(\) const",
    r"^pg::engine_detail::Delay::(run<true>|readTap)",
    r"^void pg::engine_detail::Delay::run<true>",
    r"^void pg::engine_detail::Reverb::run<true>",
    r"^void pg::engine_detail::MasterBus::run<true>",
    r"^pg::engine_detail::MasterBus::setParams\(",
    r"^_ZN2pg13engine_detail9MasterBus6rumble",
    r"^pg::lead::LeadVoice::(render|globalQuantum|active|mono|setParams|shapeFor)\(",
    r"^_ZN2pg4lead12_GLOBAL__N_16osc(One|Two)ILb0",
    r"^void pg::lead::\(anonymous namespace\)::osc(One|Two)<true>",
    r"^pg::bass::BassVoice::(render|setParams|active|mono)\(",
    r"^pg::drums::\w+Voice::(render|renderTonal|renderCowbell|renderShaker|updateCoefs|endBlock|setParams|active|mono)\(",
]
# Per-step / per-note paths (tier 2).
STEP = [
    r"^pg::engine_detail::State::(fireTrackStep|fireMasterStep|hit|afterNoteOn|buildEff)\(",
    r"^_ZN2pg13engine_detail5State10decideNext",
    r"^pg::drums::\w+Voice::(noteOn|beginHit|noteOff)\(",
    r"^pg::bass::BassVoice::(noteOn|noteOff)\(",
    r"^pg::lead::LeadVoice::(noteOn|noteOff|startVoice|queueNote|allocVoice|setPan)\(",
]
# Objects whose .rodata goes to DRAM (tier 3): the bass tables (4.2 KB).
RAM_RODATA = ["bass/bass_tables_data.cpp"]

AUDIO_OBJS = ("/pg/drums/", "/pg/bass/bass.cpp", "/pg/lead/lead.cpp", "/pg/engine/engine.cpp",
              "/pg/engine/delay.cpp", "/pg/engine/reverb.cpp", "/pg/engine/master.cpp")

_hot = [re.compile(p) for p in HOT + (STEP if TIER >= 2 else [])]


def _sections(objcopy, path):
    readelf = objcopy.replace("objcopy", "readelf")
    out = subprocess.run([readelf, "-S", "-W", path], capture_output=True, text=True).stdout
    return re.findall(r"\]\s+(\S+)\s+(?:PROGBITS|NOBITS)", out)


def _demangle(cxxfilt, names):
    # The toolchain's own c++filt, never the host's: on macOS the system
    # c++filt strips a leading underscore first and so leaves ELF names
    # mangled, which made every demangled-name pattern miss silently.
    if not names:
        return {}
    r = subprocess.run([cxxfilt], input="\n".join(names), capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError("[audio-ram] %s failed: %s" % (cxxfilt, r.stderr.strip()))
    out = r.stdout.split("\n")
    return dict(zip(names, out))


_moved = []


def _relocate(target, source, env):
    # $OBJCOPY is esptool on this platform: use the toolchain's binutils.
    objcopy = env.WhereIs(env.subst("$CC").replace("gcc", "objcopy")) or env.subst("$CC").replace("gcc", "objcopy")
    cxxfilt = objcopy.replace("objcopy", "c++filt")
    for t in target:
        path = str(t)
        rel = path.replace("\\", "/")
        secs = _sections(objcopy, path)
        if not secs:
            raise RuntimeError("[audio-ram] could not read sections of " + path)
        args = []
        if any(o in rel for o in AUDIO_OBJS):
            fns = sorted({s.split(".", 2)[2] for s in secs if s.startswith((".text.", ".literal."))})
            dm = _demangle(cxxfilt, fns)
            pick = {f for f in fns if any(h.search(dm.get(f, f)) or h.search(f) for h in _hot)}
            for s in secs:
                if s.startswith(".text.") and s[6:] in pick:
                    args += ["--rename-section", "%s=.iram1.pg.%s" % (s, s[6:])]
                elif s.startswith(".literal.") and s[9:] in pick:
                    args += ["--rename-section", "%s=.iram1.pgl.%s" % (s, s[9:])]
            _moved.extend(dm.get(f, f) for f in pick)
            if pick:
                print("[audio-ram] %s: %d functions -> IRAM" % (rel.split("/pg/")[-1], len(pick)))
        if TIER >= 3 and any(o in rel for o in RAM_RODATA):
            for s in secs:
                if s.startswith(".rodata"):
                    args += ["--rename-section", "%s=.dram1.pg%s" % (s, s[7:])]
        if args:
            subprocess.check_call([objcopy] + args + [path])
    return 0


def _pg_flags(env, node):
    path = node.srcnode().get_path().replace("\\", "/")
    if "/lib/pg/" not in path and not path.startswith("lib/pg/"):
        return node
    obj = env.Object(node, CCFLAGS=env["CCFLAGS"] + PG_WARNINGS)
    return obj


def _relocate_all(target, source, env):
    # Runs right before the link, over every lib/pg object of this build
    # (idempotent: renamed sections no longer match .text.* / .literal.*).
    # A post-action on each object would be neater, but PlatformIO does not run
    # post-actions attached inside a build middleware.
    import glob
    import os
    objs = [o for o in glob.glob(os.path.join(env.subst("$BUILD_DIR"), "**", "*.o"), recursive=True)
            if "/pg/pg/" in o.replace("\\", "/")]
    _relocate([type("N", (), {"__str__": lambda self, o=o: o})() for o in objs], None, env)
    # Fail the build rather than ship an image that silently stayed in flash.
    # (On a re-link without recompiling, the sections are already renamed and
    # nothing is moved now: only check when this run did the renaming.)
    if _moved:
        for must in ("pg::Engine::process(", "pg::lead::LeadVoice::render(", "pg::bass::BassVoice::render("):
            if not any(m.startswith(must) for m in _moved):
                raise RuntimeError("[audio-ram] %s was not moved to IRAM (%d functions matched)" % (must, len(_moved)))
        print("[audio-ram] total: %d functions -> IRAM" % len(_moved))
    elif not objs:
        raise RuntimeError("[audio-ram] found no lib/pg objects under " + env.subst("$BUILD_DIR"))
    return 0


env.AddBuildMiddleware(_pg_flags)  # noqa: F821
if TIER > 0:
    env.AddPreAction("$BUILD_DIR/${PROGNAME}.elf", env.Action(_relocate_all, "[audio-ram] moving the audio hot path to IRAM"))  # noqa: F821
