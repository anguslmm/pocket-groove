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
# commands, the template variants of the output layout this build does not
# run, the 16 bass loop variants of which one is active) stays in flash.
#
# Output layout: the engine renders MONO (device mode, one bus, L == R) in
# every env except the USB audio one (-DPG_USB_AUDIO=1), where it renders
# STEREO for the computer and the jack gets (L+R)/2. Each layout has its own
# template variants of the delay, reverb, master bus and lead voice loop
# (run<true> = mono, run<false> = stereo; LeadVoice::renderVoiceT<true> =
# stereo). Only the variants of this build's layout are moved, never both:
# every byte of IRAM is a byte of heap.
#
# Selected by -DPG_AUDIO_RAM=<tier> in build_flags (0/absent = off).
#
# Checks (a wrong placement fails the build instead of shipping an image
# that overloads): before the link, every function of MUST_COMMON + the
# layout's MUST list has been renamed; after the link, the toolchain's nm
# reads the final ELF and requires those at IRAM addresses and the other
# layout's variants in flash. The ELF check also runs on a re-link that did
# not recompile anything.
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


def _define(name):
    for d in env.get("CPPDEFINES", []):  # noqa: F821
        if isinstance(d, (list, tuple)) and d[0] == name:
            return str(d[1])
        if d == name:
            return "1"
    for f in env.get("BUILD_FLAGS", []):  # noqa: F821
        m = re.match(r"-D%s(?:=(\S+))?$" % name, str(f))
        if m:
            return m.group(1) or "1"
    return None


TIER = _tier()
STEREO = (_define("PG_USB_AUDIO") or "0") not in ("0", "")
print("[audio-ram] PG_AUDIO_RAM tier %d, %s output layout" % (TIER, "STEREO (USB audio)" if STEREO else "mono (device)"), flush=True)

# Per-quantum hot set. Regexes on demangled names.
HOT_COMMON = [
    r"^pg::Engine::process\(",
    r"^pg::engine_detail::State::(render|quantum|drainCommands|ensureParams|publishStatus|sequencerTick|rollTick)\(",
    # Macro motion (engine.h "MACRO MOTION"): what runs every 4 quanta per moving
    # track: the engine's tick (interpolation and feed) and the bass's and lead's
    # fractional mappings (below). The set's evaluation (fineInterval ->
    # macro::evaluateFine), once per 11.6 ms per moving track, and the drums'
    # fractional mappings, fed every 11.6 ms, stay in flash: ~2.5 KB of IRAM saved
    # for a cost at 86 Hz that env:profile's MOTION lines measure.
    r"^pg::engine_detail::State::(fineTick|fineEval)\(",
    r"^pg::Voice::mono\(\) const",
    r"^pg::engine_detail::Delay::readTap\(",
    r"^pg::engine_detail::MasterBus::setParams\(",
    r"^_ZN2pg13engine_detail9MasterBus6rumble",
    r"^pg::lead::LeadVoice::(render|globalQuantum|active|mono|setParams|setParamsFine|applyFine|shapeFor)\(",
    r"^pg::lead::\(anonymous namespace\)::exp2Call\(",
    r"^_ZN2pg4lead12_GLOBAL__N_16osc(One|Two)ILb0",
    r"^void pg::lead::\(anonymous namespace\)::osc(One|Two)<true>",
    r"^pg::bass::BassVoice::(render|setParams|setParamsFine|mapParams|active|mono)\(",
    r"^pg::bass::\(anonymous namespace\)::mapExp2\(",
    r"^pg::drums::\w+Voice::(render|renderTonal|renderCowbell|renderShaker|updateCoefs|endBlock|setParams|active|mono)\(",
    # Not the drums' fractional path (setParamsFine, updateFine): the engine feeds a
    # drum once per evaluation interval (11.6 ms), not per tick, so it stays in flash.
]
# The variants only one output layout runs.
HOT_MONO = [
    r"^void pg::engine_detail::Delay::run<true>",
    r"^void pg::engine_detail::Reverb::run<true>",
    r"^void pg::engine_detail::MasterBus::run<true>",
]
HOT_STEREO = [
    r"^void pg::engine_detail::Delay::run<false>",
    r"^void pg::engine_detail::Reverb::run<false>",
    r"^void pg::engine_detail::MasterBus::run<false>",
    r"^void pg::lead::LeadVoice::renderVoiceT<true>",
]
HOT = HOT_COMMON + (HOT_STEREO if STEREO else HOT_MONO)
# Placement checks (regexes on demangled names, see the header).
MUST_COMMON = [r"^pg::Engine::process\(", r"^pg::lead::LeadVoice::render\(", r"^pg::bass::BassVoice::render\(",
               r"^pg::engine_detail::State::fineEval\(", r"^pg::bass::BassVoice::mapParams\("]
MUST = MUST_COMMON + (HOT_STEREO if STEREO else HOT_MONO)
MUST_NOT = HOT_MONO if STEREO else HOT_STEREO
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


TOOL_TIMEOUT_S = 60


def _run(cmd, input=None):
    # Every toolchain call goes through here. Nothing is connected by a pipe:
    # the tool reads its input from a file and writes stdout / stderr to files,
    # so no reader, buffer size or inherited terminal can make it block. A tool
    # that does not finish is killed and the build fails naming it and its
    # last arguments, instead of the build sitting silent.
    import tempfile
    with tempfile.TemporaryDirectory(prefix="pg-audio-ram-") as tmp:
        fin = None
        if input is not None:
            with open(tmp + "/in", "w") as f:
                f.write(input)
            fin = open(tmp + "/in", "r")
        try:
            with open(tmp + "/out", "w") as fo, open(tmp + "/err", "w") as fe:
                p = subprocess.Popen(cmd, stdin=fin if fin else subprocess.DEVNULL, stdout=fo, stderr=fe)
                try:
                    rc = p.wait(timeout=TOOL_TIMEOUT_S)
                except subprocess.TimeoutExpired:
                    p.kill()
                    try:
                        p.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        pass
                    raise RuntimeError("[audio-ram] %s did not finish within %d s (last arguments: %s)"
                                       % (cmd[0], TOOL_TIMEOUT_S, " ".join(cmd[-2:])))
        finally:
            if fin:
                fin.close()
        with open(tmp + "/out", "r", errors="replace") as f:
            out = f.read()
        with open(tmp + "/err", "r", errors="replace") as f:
            err = f.read()
    return subprocess.CompletedProcess(cmd, rc, out, err)


def _sections(objcopy, path):
    import os
    readelf = objcopy.replace("objcopy", "readelf")
    if os.environ.get("PG_AUDIO_RAM_FAKE_HANG") and path.endswith(os.environ["PG_AUDIO_RAM_FAKE_HANG"]):
        _run(["sleep", "600"])   # self-test of the stall reporting: see docs/BRINGUP.md
    out = _run([readelf, "-S", "-W", path]).stdout
    return re.findall(r"\]\s+(\S+)\s+(?:PROGBITS|NOBITS)", out)


def _demangle(cxxfilt, names):
    # The toolchain's own c++filt, never the host's: on macOS the system
    # c++filt strips a leading underscore first and so leaves ELF names
    # mangled, which made every demangled-name pattern miss silently.
    if not names:
        return {}
    r = _run([cxxfilt], input="\n".join(names) + "\n")
    if r.returncode != 0:
        raise RuntimeError("[audio-ram] %s failed: %s" % (cxxfilt, r.stderr.strip()))
    out = r.stdout.split("\n")
    return dict(zip(names, out))


_moved = []
_kept = []     # functions an earlier build already moved (objects not recompiled this time)
_step = ["starting"]   # what the placement step is doing right now (for the watchdog)


def _relocate(target, source, env):
    # $OBJCOPY is esptool on this platform: use the toolchain's binutils.
    objcopy = env.WhereIs(env.subst("$CC").replace("gcc", "objcopy")) or env.subst("$CC").replace("gcc", "objcopy")
    cxxfilt = objcopy.replace("objcopy", "c++filt")
    for t in target:
        path = str(t)
        rel = path.replace("\\", "/")
        short = rel.split("/pg/")[-1]
        _step[0] = "readelf " + short
        secs = _sections(objcopy, path)
        if not secs:
            raise RuntimeError("[audio-ram] could not read sections of " + path)
        args = []
        if any(o in rel for o in AUDIO_OBJS):
            _step[0] = "c++filt " + short
            fns = sorted({s.split(".", 2)[2] for s in secs if s.startswith((".text.", ".literal."))})
            # An object that was not recompiled still carries the names an
            # earlier build gave it: count those as placed, do not rename again.
            old = sorted({s[len(".iram1.pg."):] for s in secs if s.startswith(".iram1.pg.")})
            dm = _demangle(cxxfilt, fns + old)
            _kept.extend(dm.get(f, f) for f in old)
            _step[0] = "matching " + short
            pick = {f for f in fns if any(h.search(dm.get(f, f)) or h.search(f) for h in _hot)}
            for s in secs:
                if s.startswith(".text.") and s[6:] in pick:
                    args += ["--rename-section", "%s=.iram1.pg.%s" % (s, s[6:])]
                elif s.startswith(".literal.") and s[9:] in pick:
                    args += ["--rename-section", "%s=.iram1.pgl.%s" % (s, s[9:])]
            _moved.extend(dm.get(f, f) for f in pick)
            if pick:
                print("[audio-ram] %s: %d functions -> IRAM" % (rel.split("/pg/")[-1], len(pick)), flush=True)
        if TIER >= 3 and any(o in rel for o in RAM_RODATA):
            for s in secs:
                if s.startswith(".rodata"):
                    args += ["--rename-section", "%s=.dram1.pg%s" % (s, s[7:])]
        if args:
            _step[0] = "objcopy " + short
            r = _run([objcopy] + args + [path])
            if r.returncode != 0:
                raise RuntimeError("[audio-ram] objcopy failed on %s: %s" % (path, r.stderr.strip()))
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
    import faulthandler
    import glob
    import os
    import sys
    # If this step stalls, print where (the Python stack of every thread)
    # every 20 s instead of sitting silent.
    import threading
    done = threading.Event()

    def _watch():
        waited = 0
        while not done.wait(10):
            waited += 10
            sys.__stderr__.write("[audio-ram] still working after %d s: %s\n" % (waited, _step[0]))
            sys.__stderr__.flush()

    threading.Thread(target=_watch, daemon=True).start()
    faulthandler.dump_traceback_later(30, repeat=True, file=sys.__stderr__)
    try:
        return _relocate_all_body(target, source, env)
    finally:
        done.set()
        faulthandler.cancel_dump_traceback_later()


def _relocate_all_body(target, source, env):
    import glob
    import os
    _step[0] = "listing the objects under " + env.subst("$BUILD_DIR")
    objs = [o for o in glob.glob(os.path.join(env.subst("$BUILD_DIR"), "**", "*.o"), recursive=True)
            if "/pg/pg/" in o.replace("\\", "/")]
    _relocate([type("N", (), {"__str__": lambda self, o=o: o})() for o in objs], None, env)
    # Fail the build rather than ship an image that silently stayed in flash.
    # An incremental build recompiles only some objects: the others were
    # renamed by an earlier build (_kept) and count as placed.
    placed = _moved + _kept
    if placed:
        for must in MUST:
            if not any(re.search(must, m) for m in placed):
                raise RuntimeError("[audio-ram] %s was not moved to IRAM (%d functions matched)" % (must, len(placed)))
        for nope in MUST_NOT:
            if any(re.search(nope, m) for m in placed):
                raise RuntimeError("[audio-ram] %s was moved to IRAM, but this build does not run it" % nope)
        print("[audio-ram] total: %d functions -> IRAM (%d moved now, %d already placed)"
              % (len(placed), len(_moved), len(_kept)), flush=True)
    elif not objs:
        raise RuntimeError("[audio-ram] found no lib/pg objects under " + env.subst("$BUILD_DIR"))
    return 0


IRAM_LO, IRAM_HI = 0x40370000, 0x403E0000   # ESP32-S3 internal SRAM, instruction bus


def _verify_elf(target, source, env):
    # The final image, read with the toolchain's nm (never the host's: macOS
    # nm/c++filt read these names differently, see _demangle).
    elf = str(target[0])
    nm = env.WhereIs(env.subst("$CC").replace("gcc", "nm")) or env.subst("$CC").replace("gcc", "nm")
    r = _run([nm, "-C", elf])
    if r.returncode != 0 or not r.stdout:
        raise RuntimeError("[audio-ram] %s -C %s failed: %s" % (nm, elf, r.stderr.strip()))
    syms = []
    for line in r.stdout.splitlines():
        p = line.split(None, 2)
        if len(p) == 3 and p[1] in "tTwW":
            try:
                syms.append((int(p[0], 16), p[2]))
            except ValueError:
                pass
    bad = []
    for must in MUST:
        hits = [(a, n) for a, n in syms if re.search(must, n)]
        if not hits:
            bad.append("%s: not in the image" % must)
        bad += ["%s at 0x%08x: not in IRAM" % (n, a) for a, n in hits if not IRAM_LO <= a < IRAM_HI]
    for nope in MUST_NOT:
        bad += ["%s at 0x%08x: in IRAM, but this build does not run it" % (n, a)
                for a, n in syms if re.search(nope, n) and IRAM_LO <= a < IRAM_HI]
    if bad:
        raise RuntimeError("[audio-ram] placement check of %s FAILED:\n  %s" % (elf, "\n  ".join(bad)))
    print("[audio-ram] placement check of the ELF: %d hot-path functions in IRAM, %d variants of the other "
          "layout in flash: OK" % (len(MUST), len(MUST_NOT)), flush=True)


env.AddBuildMiddleware(_pg_flags)  # noqa: F821
if TIER > 0:
    env.AddPreAction("$BUILD_DIR/${PROGNAME}.elf", env.Action(_relocate_all, "[audio-ram] moving the audio hot path to IRAM"))  # noqa: F821
    env.AddPostAction("$BUILD_DIR/${PROGNAME}.elf", env.Action(_verify_elf, "[audio-ram] checking the placement in the ELF"))  # noqa: F821
