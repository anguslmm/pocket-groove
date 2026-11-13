"""System test of the desktop app (ctest: test_desktop_app).

Runs build/host/pocket-groove headless (SDL_VIDEODRIVER=dummy,
SDL_AUDIODRIVER=dummy: a real SDL window, renderer and audio thread with no
screen or sound card) and checks it against the rest of the host build:

  1. keys: host/app/tests/desktop_keys.txt goes through SDL's event queue and
     the key map; its shots must be pixel-identical (identical PNG bytes) to
     host/tools/ui_shot running the same keys, with the app's default keyboard
     and with --device-keys (matrix model + phantom filter, as ui_shot has).
     A pad and SHIFT held when the window loses focus must both come up.
  2. quit saves the session: projects/_last.pgv exists, carries the edit
     (tempo 124 -> 125) and the next launch resumes it.
  3. audio: SPACE plays, F9 records the live output; desktop_audio_ref must
     find the recording BIT-IDENTICAL to the offline engine (see that file
     for why that is possible). Stereo 44.1 kHz with a 256-frame buffer, and
     mono 32 kHz with a 100-frame buffer (not a multiple of the 16-frame quantum).
  4. a session the desktop saved goes through the device's transfer protocol
     (host/tools/xfer_sim + tools/pg.py) unchanged both ways (skipped without
     pyserial, as test_pg_client).
  5. the strip under the screen (desktop_strip_keys.txt): the device screen is
     still pixel-identical to ui_shot, also in the window screenshot; the
     modifier chips light exactly while the key mapper holds the key; the
     strip's clicks, Cmd+I/L/B and keys typed into the Stems list deliver no
     key to the instrument (the app's count equals the script's).
  6. Listen against a simulated device (--listen-sim: a 1 kHz tone on a clock
     500 ppm fast): it plays, Cmd+R records it, unplug and re-plug recover,
     Cmd+L stops it. The recording is fitted (host/app/tests/listen_analyze):
     the tone's frequency, level and THD+N, not just "not silent". Then the
     same device delivering obtained specs other than the one asked for
     (f32 mono 48 kHz in 1024-frame callbacks, CoreAudio-like bursts of 4
     callbacks, s32 at 32 kHz in odd and varying sizes, 6-channel big-endian
     float at 96 kHz): the recording must be the same clean tone, nothing
     underruns, and --listen-dump holds exactly what was delivered with the
     obtained spec in its WAV header. And SDL's own capture code path, with
     SDL's disk audio driver reading a sine from a file as the input device:
     the dump must be a byte-exact slice of that file and the recording the
     clean tone.
  7. Stems end to end against xfer_sim (tools/pg --json as a subprocess): the
     device's list, a bounce of a device project to bounces/NAME/ with every
     stem file, the folder "revealed"; and the error paths in plain words:
     device not found, port busy, project that does not load, pyserial missing.
     (Skipped without pyserial.)
  8. the macro lab (--lab, docs/MACROLAB.md) with a copy of macros/ and a test
     set: choosing it, dragging a macro's slider and clicking the XY pad by
     mouse events puts the evaluated values into the engine's project; a
     rewrite of the file while the app runs is reloaded and live; a broken
     rewrite is reported with file, line and reason and changes nothing; Cmd+G
     with a typed note and the BAD button with Esc write the two lab-notes.txt
     lines; Cmd+Left from ACID steps to "none" (CLASSIC: the values stay); the
     set's macro position is remembered; deleting the active file (an extra
     instrument) leaves the track playing its values; no lab key, click or note reaches the instrument; the window grows
     by the panel and the device screen in it is untouched.

    python3 host/app/tests/desktop_check.py --bin build/host --work build/host/desktop_test
"""
import argparse
import math
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import wave
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import pg  # noqa: E402  (tools/pg.py: header parsing; serial only for part 4)

LEGENDS = ["`", "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "-", "=", "del",
           "tab", "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "[", "]", "\\",
           "fn", "sft", "a", "s", "d", "f", "g", "h", "j", "k", "l", ";", "'", "ent",
           "ctl", "opt", "alt", "z", "x", "c", "v", "b", "n", "m", ",", ".", "/", "spc"]
ALIASES = {"backtick": 0, "minus": 11, "equals": 12, "backspace": 13, "shift": 29, "enter": 41,
           "ctrl": 42, "space": 55, "semicolon": 39, "quote": 40, "comma": 52, "dot": 53, "slash": 54}

failures = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        failures.append(what)


def key_id(name):
    n = name.lower()
    if n in LEGENDS:
        return LEGENDS.index(n)
    return ALIASES[n]


def ui_shot_script(body):
    """The desktop key script as a ui_shot script: same keys, same timing."""
    out = ["battery 255", "sd 0", "boot"]   # the app's UiEnv; boot = the device's power-on
    held = set()
    for raw in body.splitlines():
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        w = line.split()
        if w[0] == "down":
            held.add(key_id(w[1]))
        elif w[0] == "up":
            held.discard(key_id(w[1]))
        elif w[0] == "focuslost":   # the app releases every held key, lowest id first
            out += ["up %s" % LEGENDS[k] for k in sorted(held)]
            held.clear()
            continue
        elif w[0] in ("expect-released", "quit", "app"):
            continue
        elif w[0] == "sdl":   # app-level keys (Command shortcuts, G0): no Ui event, same time
            if w[1] == "tap":
                out.append("wait 40")
            continue
        out.append(line)
    return "\n".join(out) + "\n"


def run_app(args, cwd, script_text, name, timeout=60, env=None):
    script = os.path.join(cwd, name + ".txt")
    with open(script, "w") as f:
        f.write(script_text)
    cmd = [APP, "--script", script, "--out", os.path.join(cwd, "shots-" + name)] + args
    p = subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout, env=env)
    log = p.stdout.decode(errors="replace")
    sys.stdout.write("".join("    | " + l + "\n" for l in log.splitlines()))
    return p.returncode, log


def fresh(path):
    shutil.rmtree(path, ignore_errors=True)
    os.makedirs(path)
    return path


def read(path):
    with open(path, "rb") as f:
        return f.read()


def part_keys():
    print("1. keys and screenshots against ui_shot")
    body = read(os.path.join(HERE, "desktop_keys.txt")).decode()
    ref = fresh(os.path.join(WORK, "ui_shot"))
    with open(os.path.join(ref, "keys.txt"), "w") as f:
        f.write(ui_shot_script(body))
    p = subprocess.run([UI_SHOT, os.path.join(ref, "keys.txt"), ref], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    check(p.returncode == 0, "ui_shot ran the converted script (%s)" % p.stdout.decode().strip().replace("\n", ", "))
    for label, extra in (("default", []), ("device-keys", ["--device-keys"])):
        cwd = fresh(os.path.join(WORK, "keys-" + label))
        rc, log = run_app(extra, cwd, body, "keys")
        check(rc == 0, "%s: app exited 0 (script ok, session saved)" % label)
        check("expect-released" not in log, "%s: no key left held after focus loss" % label)
        check(log.count("[rec] started") == 1 and log.count("[rec] stopped") == 1,
              "%s: Cmd+R started and stopped a recording" % label)
        for shot in ("keys", "after-focus"):
            a = os.path.join(cwd, "shots-keys", shot + ".png")
            b = os.path.join(ref, shot + ".png")
            same = os.path.exists(a) and os.path.exists(b) and read(a) == read(b)
            check(same, "%s: %s.png pixel-identical to ui_shot" % (label, shot))


def part_session():
    print("2. quit saves the session, the next launch resumes it")
    cwd = os.path.join(WORK, "keys-default")   # from part 1: tempo +1, quit
    sess = os.path.join(cwd, "projects", "_last.pgv")
    check(os.path.exists(sess), "projects/_last.pgv written at quit")
    if not os.path.exists(sess):
        return
    h = pg.pgv_header(read(sess))
    check(h["tempo"] == 125, "the session carries the edit (tempo %d, want 125)" % h["tempo"])
    before = read(sess)
    rc, log = run_app([], cwd, "wait 300\nshot boot2\nquit\n", "boot2")
    check(rc == 0 and "resumed the session" in log, "second launch resumed the session")
    check(pg.pgv_header(read(sess))["tempo"] == 125, "still tempo 125 after the second quit")
    check(read(sess)[64:] == before[64:], "unchanged session saved unchanged")


def part_audio():
    print("3. live audio against the offline engine")
    # 4.5 s of play for a 2.5 s comparison: SDL's dummy driver paces its callbacks with
    # SDL_Delay and runs slower than the wall clock with small buffers, and the stop
    # (which the reference does not model) must come after the compared stretch.
    f9 = "wait 500\nsdl tap F9\nwait 300\ntap spc\nwait 4500\ntap spc\nwait 300\nsdl tap F9\nquit\n"
    cmd_r = f9.replace("sdl tap F9", "sdl down Left GUI\nsdl tap R\nsdl up Left GUI")
    for label, extra, ref_args, script in (("stereo-44k-F9", [], [], f9),
                                           ("mono-32k-buf100-CmdR", ["--mono", "--sr", "32000", "--buffer", "100"],
                                            ["--mono", "--sr", "32000"], cmd_r)):
        cwd = fresh(os.path.join(WORK, "audio-" + label))
        rc, log = run_app(extra, cwd, script, "audio")
        check(rc == 0, "%s: app exited 0" % label)
        m = re.search(r"\[rec\] started: (\S+) at audio frame (\d+)", log)
        check(m is not None, "%s: the record key started a recording" % label)
        if not m:
            continue
        wav = os.path.join(cwd, m.group(1))
        check(os.path.dirname(m.group(1)) == "bounces/live", "%s: recording in ./bounces/live" % label)
        p = subprocess.run([AUDIO_REF, "--wav", wav, "--start-frame", m.group(2), "--seconds", "2.5", "--demo", "0"]
                           + ref_args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        out = p.stdout.decode().strip()
        sys.stdout.write("".join("    | " + l + "\n" for l in out.splitlines()))
        check(p.returncode == 0, "%s: recording bit-identical to the offline render" % label)


def part_xfer():
    print("4. a desktop-saved file through the device transfer protocol")
    try:
        import serial  # noqa: F401
    except ImportError:
        print("  skipped: pyserial not installed")
        return
    sess = os.path.join(WORK, "keys-default", "projects", "_last.pgv")
    if not os.path.exists(sess):
        check(False, "no desktop session to transfer")
        return
    data = read(sess)
    root = fresh(os.path.join(WORK, "xfer"))
    proc = subprocess.Popen([XFER_SIM, "--root", root], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        port = proc.stdout.readline().decode().strip()
        check(port.startswith("/dev/"), "xfer_sim up on %s" % port)
        with pg.Link(port, timeout=0.3) as link:
            pg.hello(link)
            check(pg.put_file(link, "int", "DESKTOP", data) == ["int", "DESKTOP"], "push: device accepted it")
            dev = read(os.path.join(root, "int", "pg", "DESKTOP.pgv"))
            check(dev[64:] == data[64:] and pg.pgv_header(dev)["stem"] == "DESKTOP",
                  "on the device: same body, header renamed to DESKTOP")
            back = pg.get_file(link, "int", "DESKTOP")
            check(back == dev, "pull: identical to the device's file")
            check(pg.with_stem(back, pg.pgv_header(data)["stem"]) == data, "and identical to the desktop's bytes")
        # what `tools/pg pull` leaves in ./projects opens on the desktop
        cwd = fresh(os.path.join(WORK, "pulled"))
        os.makedirs(os.path.join(cwd, "projects"))
        with open(os.path.join(cwd, "projects", "DESKTOP.pgv"), "wb") as f:
            f.write(back)
        rc, log = run_app([], cwd, "wait 300\ntap \\\nwait 300\nshot project\nquit\n", "pulled")
        check(rc == 0, "desktop app ran on a pulled file")
    finally:
        proc.kill()
        proc.wait()


def png_rgb(path):
    """(width, height, rows of RGB bytes) of a PNG written by host/common/ui_harness.h (filter 0)."""
    d = read(path)
    pos, idat, w, h = 8, b"", 0, 0
    while pos < len(d):
        n = int.from_bytes(d[pos:pos + 4], "big")
        t, body = d[pos + 4:pos + 8], d[pos + 8:pos + 8 + n]
        pos += 12 + n
        if t == b"IHDR":
            w, h = int.from_bytes(body[0:4], "big"), int.from_bytes(body[4:8], "big")
        elif t == b"IDAT":
            idat += body
    raw = zlib.decompress(idat)
    stride = w * 3 + 1
    return w, h, [raw[y * stride + 1:(y + 1) * stride] for y in range(h)]


def px(rows, x, y):
    return tuple(rows[y][x * 3:x * 3 + 3])


AMBER = (255, 182, 0)       # the palette's AMBER as RGB565 expands it
RAISED = (33, 36, 49)
STRIP_MODS = ["SHIFT", "FN", "CTRL", "OPT", "ALT", "TAB", "G0"]


def chip_fill(rows, i):
    """Fill colour of modifier chip i in a 1x window shot (strip.cpp's layout)."""
    card = STRIP_MODS[i]
    cx = 1 + i * 34 + 17
    wd = len(card) * 6 - 1 + 4
    return px(rows, cx - wd // 2 + 1, 135 + 11 + 4)


def instrument_key_events(body):
    """Key events the script sends to the Ui: down/up 1 each, tap 2 (sdl/app lines: none)."""
    n = 0
    for raw in body.splitlines():
        w = raw.split("#", 1)[0].split()
        if w and w[0] in ("down", "up"):
            n += 1
        elif w and w[0] == "tap":
            n += 2
    return n


def part_strip():
    print("5. the strip: device screen unchanged, modifier lights, controls never reach the instrument")
    body = read(os.path.join(HERE, "desktop_strip_keys.txt")).decode()
    ref = fresh(os.path.join(WORK, "ui_shot-strip"))
    with open(os.path.join(ref, "keys.txt"), "w") as f:
        f.write(ui_shot_script(body))
    p = subprocess.run([UI_SHOT, os.path.join(ref, "keys.txt"), ref], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    check(p.returncode == 0, "ui_shot ran the instrument keys alone")
    cwd = fresh(os.path.join(WORK, "strip"))
    env = dict(os.environ, PG_TOOL="false", PG_REVEAL="")   # no device work in this part
    rc, log = run_app([], cwd, body, "strip", env=env)
    check(rc == 0, "app exited 0")
    shots = os.path.join(cwd, "shots-strip")
    a, b = os.path.join(shots, "keys.png"), os.path.join(ref, "keys.png")
    check(os.path.exists(a) and os.path.exists(b) and read(a) == read(b), "keys.png pixel-identical to ui_shot")
    m = re.search(r"\[keys\] (\d+) key events reached the instrument", log)
    want = instrument_key_events(body)
    check(m is not None and int(m.group(1)) == want,
          "exactly the script's %d instrument key events reached the Ui (app: %s)" % (want, m and m.group(1)))
    lit = re.findall(r"\[strip\] lit: (.*)", log)
    want_lit = ["-", "SHIFT", "-", "FN", "-", "CTRL", "-", "OPT", "-", "ALT", "-", "TAB", "-", "G0", "-",
                "SHIFT", "SHIFT OPT", "SHIFT", "-"]
    check(lit == want_lit, "modifier lights followed the keys: %s" % " | ".join(lit))
    # the window screenshot: 240 x (135 + strip), the device part identical to the shot
    w, h, win = png_rgb(os.path.join(shots, "window.png"))
    w2, h2, dev = png_rgb(b)
    check(w == 240 and h > 135 + 30, "window shot is 240 x %d (screen + strip)" % h)
    check(win[:135] == dev, "the window's device screen is the ui_shot screenshot, pixel for pixel")
    _, _, one = png_rgb(os.path.join(shots, "lit-shift.png"))
    _, _, two = png_rgb(os.path.join(shots, "lit-two.png"))
    check(chip_fill(one, 0) == AMBER and all(chip_fill(one, i) == RAISED for i in range(1, 7)),
          "SHIFT held: its chip lit, the others not")
    check([chip_fill(two, i) == AMBER for i in range(7)] == [True, False, False, True, False, False, False],
          "SHIFT + OPT held: both lit")
    check(all(chip_fill(win, i) == RAISED for i in range(7)), "nothing lit at the end")
    check("[strip] click stats" in log and "[strip] click listen" in log and "[strip] click stems" in log,
          "clicks on STATS, LISTEN and STEMS reached their buttons")
    check(log.count("[listen] on") == 2 and "[listen] playing" not in log,
          "Listen without a device: switched on twice (click, Cmd+L), nothing played, no crash")
    check(log.count("[stems] open") == 2 and log.count("[stems] closed") == 2, "Stems list opened and closed twice")


def wav_frames(path):
    with wave.open(path, "rb") as f:
        n, ch, sw, sr = f.getnframes(), f.getnchannels(), f.getsampwidth(), f.getframerate()
        data = f.readframes(n)
    v = struct.unpack("<%dh" % (n * ch), data)
    return sr, v[0::2], v[1::2]


def part_listen():
    print("6. Listen against a simulated device (clock +500 ppm)")
    script = """wait 400
app click-button listen
app wait-listening 8000
sdl down Left GUI
sdl tap R
sdl up Left GUI
wait 1500
app print-listen
app winshot listening
app listen-sim unplug
wait 2500
app winshot unplugged
app listen-sim plug
app wait-listening 8000
wait 1000
sdl down Left GUI
sdl tap R
sdl tap L
sdl up Left GUI
wait 300
app winshot off
app print-listen
quit
"""
    cwd = fresh(os.path.join(WORK, "listen"))
    rc, log = run_app(["--listen-sim", "500"], cwd, script, "listen", env=dict(os.environ, PG_TOOL="false"))
    check(rc == 0, "app exited 0")
    check(log.count("[listen] playing") == 2, "played, and played again after the re-plug")
    check("[listen] closed (device gone)" in log, "unplug while listening: input closed, no crash")
    check("[listen] closed (switched off)" in log, "Cmd+L switched it off")
    st = listen_stats(log)
    check(len(st) >= 2 and st[0]["primed"] == 1, "primed and playing after the start (%s)" % (st[:1],))
    if st:
        u, o, r = st[-1]["underruns"], st[-1]["overflows"], st[-1]["resyncs"]
        check(o == 0 and r == 0 and u <= 1, "no overflow or resync, at most the unplug's underrun "
              "(underruns %d, overflows %d, resyncs %d)" % (u, o, r))
    check("[listen] capture obtained: s16le, 2 ch, 44100 Hz" in log and "[listen] conversion: s16le as is" in log,
          "the diagnostics block: obtained spec and conversion")
    m = re.search(r"\[keys\] (\d+) key events", log)
    check(m is not None and m.group(1) == "0", "no key reached the instrument")
    m = re.search(r"\[rec\] started: (\S+)", log)
    check(m is not None, "Cmd+R recorded while listening")
    if m:
        sr, L, R = wav_frames(os.path.join(cwd, m.group(1)))
        seg = slice(int(0.3 * sr), int(1.3 * sr))   # the engine is stopped: only the device is heard
        l, r = L[seg], R[seg]
        rms = (sum(x * x for x in l) / max(len(l), 1)) ** 0.5
        cross = sum(1 for i in range(1, len(l)) if (l[i - 1] < 0) != (l[i] < 0))
        freq = cross / 2.0 / (len(l) / sr) if l else 0
        anti = max((abs(a + b) for a, b in zip(l, r)), default=99)
        check(rms > 3000 and 980 < freq < 1020 and anti <= 2,
              "the recording carries the device's tone (rms %.0f, %.1f Hz, L = -R within %d)" % (rms, freq, anti))
        check_tone(os.path.join(cwd, m.group(1)), 1000.5, "the recording", seconds=1.0)   # before the unplug
    for n in ("listening", "unplugged", "off"):
        check(os.path.exists(os.path.join(cwd, "shots-listen", n + "_4x.png")), "window shot %s" % n)


LISTEN_STATS = re.compile(
    r"\[listen\] stats: primed (?P<primed>\d), fill (?P<fill>[\d.]+) fr \([\d.]+ ms\), target (?P<target>\d+), "
    r"step (?P<step>[\d.]+), rate (?P<rate>[-+\d.]+) ppm, clock (?P<clock>[-+\d.]+) ppm, underruns (?P<underruns>\d+), "
    r"overflows (?P<overflows>\d+), resyncs (?P<resyncs>\d+), primes (?P<primes>\d+), pushed (?P<pushed>\d+), "
    r"device peak (?P<dev>\S+) dBFS, after mix (?P<mix>\S+) dBFS, clipped (?P<clipped>\d+)")


def listen_stats(log):
    out = []
    for m in LISTEN_STATS.finditer(log):
        d = m.groupdict()
        out.append({k: (float(v) if k in ("fill", "step", "rate", "clock") else v if k in ("dev", "mix") else int(v))
                    for k, v in d.items()})
    return out


def analyze(path, start=0.3, seconds=1.0, channel=0, freq=1000.0):
    p = subprocess.run([ANALYZE, path, "--from", str(start), "--seconds", str(seconds), "--channel", str(channel),
                        "--freq", str(freq)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    out = p.stdout.decode().strip()
    w = out.split()
    if p.returncode != 0 or len(w) < 16:
        return None, out
    return {w[i]: float(w[i + 1]) for i in range(0, len(w) - 1, 2)}, out


def check_tone(path, freq, what, dbfs=-14.74, max_thdn=-60.0, start=0.3, seconds=2.0, channel=0, ppm_tol=2500):
    """The 1 kHz tone at -14.7 dBFS (6000) and nothing else: frequency, level and THD+N of a fit.

    The frequency within 0.25 %: in the first seconds the rate match may still move the rate by
    up to its 0.2 % limit (test_desktop_listen checks the settled frequency to 100 ppm); what this
    catches is a wrong rate (48 kHz played as 44.1 kHz is 8.8 %)."""
    r, out = analyze(path, start, seconds, channel, freq)
    sys.stdout.write("    | %s\n" % out)
    check(r is not None and abs(r["freq"] / freq - 1) < ppm_tol * 1e-6 and abs(r["dbfs"] - dbfs) < 0.1
          and r["thdn"] < max_thdn and r["worst"] < max_thdn + 10,
          "%s: the tone at %.1f Hz, %.2f dBFS, THD+N < %.0f dB, no window above %.0f dB (%s)"
          % (what, freq, dbfs, max_thdn, max_thdn + 10, out))
    return r


def wav_header(path):
    with open(path, "rb") as f:
        h = f.read(44)
    tag, ch, rate, _, _, bits = struct.unpack("<HHIIHH", h[20:36])
    return {"tag": tag, "channels": ch, "rate": rate, "bits": bits, "data": struct.unpack("<I", h[40:44])[0]}


LISTEN_SPEC_SCRIPT = """wait 400
app click-button listen
app wait-listening 8000
sdl down Left GUI
sdl tap R
sdl up Left GUI
wait 2500
app print-listen
sdl down Left GUI
sdl tap R
sdl tap L
sdl up Left GUI
wait 200
quit
"""


def part_listen_specs():
    print("6b. Listen: the device delivers another spec than asked for")
    # (label, --listen-sim-spec, burst, vary, WAV tag, bits, conversion text)
    cases = [("f32-mono-48k-1024", "f32:1:48000:1024", 1, False, 3, 32,
              "f32le -> s16 (saturating), mono -> both sides, 48000 -> 44100 Hz"),
             ("s16-stereo-44k-bursts-4x256", "s16:2:44100:256", 4, False, 1, 16, "s16le as is, stereo as is"),
             ("s32-stereo-32k-odd-441", "s32:2:32000:441", 1, True, 1, 32,
              "s32le -> s16 (saturating), stereo as is, 32000 -> 44100 Hz"),
             ("f32be-6ch-96k-480", "f32be:6:96000:480", 1, False, 3, 32,
              "f32be -> s16 (saturating), 6 ch -> the first two, 96000 -> 44100 Hz")]
    for label, spec, burst, vary, tag, bits, conv in cases:
        cwd = fresh(os.path.join(WORK, "listen-" + label))
        args = ["--listen-sim", "0", "--listen-sim-spec", spec, "--listen-dump", "dump.wav"]
        if burst > 1:
            args += ["--listen-sim-burst", str(burst)]
        if vary:
            args += ["--listen-sim-vary"]
        rc, log = run_app(args, cwd, LISTEN_SPEC_SCRIPT, "listen", env=dict(os.environ, PG_TOOL="false"))
        fmt, ch, rate, frames = spec.split(":")
        ch, rate = int(ch), int(rate)
        check(rc == 0 and "[listen] playing" in log, "%s: plays" % label)
        check("[listen] conversion: " + conv in log, "%s: conversion %s" % (label, conv))
        st = listen_stats(log)
        check(len(st) >= 1 and st[0]["primed"] == 1 and st[0]["underruns"] == 0 and st[0]["overflows"] == 0
              and st[0]["resyncs"] == 0 and st[0]["primes"] == 1 and st[0]["clipped"] == 0,
              "%s: primed once, no underrun, overflow, resync or clip (%s)" % (label, st[:1]))
        m = re.search(r"\[rec\] started: (\S+)", log)
        check(m is not None, "%s: recorded" % label)
        if m:
            rec = os.path.join(cwd, m.group(1))
            check_tone(rec, 1000.0, "%s: recording, left" % label)
            if ch == 1:
                sr, L, R = wav_frames(rec)
                check(L == R, "%s: mono input on both sides" % label)
            else:
                check_tone(rec, 1000.0, "%s: recording, right" % label, channel=1)
        dump = os.path.join(cwd, "dump.wav")
        check(os.path.exists(dump), "%s: --listen-dump wrote %s" % (label, dump))
        if os.path.exists(dump):
            h = wav_header(dump)
            check(h["tag"] == tag and h["channels"] == ch and h["rate"] == rate and h["bits"] == bits and h["data"] > 0,
                  "%s: the dump's header is the obtained spec (%s)" % (label, h))
            # the simulated device's tone, exactly as it made it (its own clock: exactly 1000 Hz)
            check_tone(dump, 1000.0, "%s: dump" % label, start=0.1, ppm_tol=1, max_thdn=-80.0)


def part_listen_sdl():
    print("6c. Listen through SDL's real capture path (SDL's disk driver: a file as the input device)")
    cwd = fresh(os.path.join(WORK, "listen-sdl-disk"))
    rate, secs = 44100, 30
    raw = bytearray()
    for i in range(rate * secs):
        v = int(round(6000 * math.sin(2 * math.pi * 1000 * i / rate)))
        raw += struct.pack("<hh", v, -v)
    inp = os.path.join(cwd, "in.raw")
    with open(inp, "wb") as f:
        f.write(raw)
    env = dict(os.environ, PG_TOOL="false", SDL_AUDIODRIVER="disk", SDL_DISKAUDIOFILEIN=inp,
               SDL_DISKAUDIOFILE=os.path.join(cwd, "out.raw"))
    rc, log = run_app(["--listen-device", "capture", "--listen-dump", "dump.wav"], cwd, LISTEN_SPEC_SCRIPT, "listen",
                      env=env, timeout=90)
    m = re.search(r'\[listen\] playing "([^"]+)"', log)
    if m is None and "[listen] capture" not in log and "NOT RUNNING" in log:
        print("  skipped: this SDL has no disk audio driver")
        return
    check(rc == 0 and m is not None, "SdlCapture opened SDL's capture device (%s)" % (m.group(1) if m else "-"))
    check(re.search(r"\[listen\] SDL built against \S+, running \S+ = (SDL2 itself|sdl2-compat)", log) is not None,
          "the diagnostics name the SDL")
    check("[listen] capture obtained: s16le, 2 ch, 44100 Hz" in log, "obtained spec printed")
    check(re.search(r"\[listen\] capture callbacks: \d+ callbacks", log) is not None, "capture callbacks counted")
    st = listen_stats(log)
    check(len(st) >= 1 and st[0]["primed"] == 1 and st[0]["overflows"] == 0 and st[0]["clipped"] == 0,
          "primed, no overflow or clip (%s)" % (st[:1],))
    dump = os.path.join(cwd, "dump.wav")
    if os.path.exists(dump):
        d = read(dump)[44:]
        at = raw.find(d[:4096]) if len(d) >= 4096 else -1
        check(at >= 0 and at % 4 == 0 and raw[at:at + len(d)] == d,
              "the dump is a byte-exact slice of the input file (%d bytes at offset %d)" % (len(d), at))
    else:
        check(False, "--listen-dump wrote dump.wav")
    m = re.search(r"\[rec\] started: (\S+)", log)
    check(m is not None, "recorded")
    if m:
        # SDL's disk driver paces both devices with SDL_Delay, so their clocks differ by an unknown,
        # small amount: the rate match follows it; the tone stays clean.
        check_tone(os.path.join(cwd, m.group(1)), 1000.0, "the recording", ppm_tol=20000, max_thdn=-50.0)


def stems_script(*middle):
    return "wait 400\nsdl down Left GUI\nsdl tap B\nsdl up Left GUI\napp wait-stems 30000\n" + \
        "\n".join(middle) + "\nsdl tap Escape\nwait 100\nquit\n"


def part_stems():
    print("7. Stems against the simulated device")
    try:
        import serial  # noqa: F401
    except ImportError:
        print("  skipped: pyserial not installed")
        return
    base_env = dict(os.environ, PG_REVEAL="echo")
    base_env.pop("PG_PORT", None)
    base_env.pop("PG_TOOL", None)
    sim_root = fresh(os.path.join(WORK, "stems-sim"))
    proc = subprocess.Popen([XFER_SIM, "--root", sim_root, "--demos"], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        port = proc.stdout.readline().decode().strip()
        check(port.startswith("/dev/"), "xfer_sim up on %s" % port)
        # a. the list, then bounce ACIDJAM (second on the device's list: the session comes first)
        cwd = fresh(os.path.join(WORK, "stems"))
        rc, log = run_app(["--pg-port", port], cwd, stems_script(
            "app winshot list", "sdl tap Down", "sdl tap Return", "wait 300", "app winshot working",
            "app wait-stems 240000", "app winshot done", "app print-keys"), "stems", timeout=300, env=base_env)
        check(rc == 0, "app exited 0")
        check("[stems] device: 3 project(s), firmware sim-1.0" in log, "the device's projects listed")
        m = re.search(r"\[stems\] done: (\S+) \((\d+) files\)", log)
        check(m is not None, "bounce finished")
        if m:
            d = m.group(1)
            check(os.path.realpath(d) == os.path.realpath(os.path.join(cwd, "bounces", "ACIDJAM")),
                  "stems in ./bounces/ACIDJAM")
            files = sorted(os.listdir(d))
            stems = ["ACIDJAM_124bpm_%02d_%s.wav" % (i + 1, n) for i, n in enumerate(
                ["kick", "snare", "clap", "closedhat", "openhat", "perc", "bass", "lead", "delay", "reverb"])]
            want = set(stems + ["ACIDJAM_124bpm_mix.wav", "ACIDJAM_124bpm_info.txt"])
            check(want <= set(files) and m.group(2) == "12", "the 10 stems, the mix and info.txt (%s)" % files)
            pulled = os.path.join(d, "ACIDJAM.pgv")
            check(os.path.exists(pulled) and read(pulled) == read(os.path.join(sim_root, "int", "pg", "ACIDJAM.pgv")),
                  "the pulled project sits next to its stems, identical to the device's")
            check(not os.path.exists(os.path.join(cwd, "projects", "ACIDJAM.pgv")), "./projects untouched")
            check("[reveal] %s" % d in log, "the folder was opened (PG_REVEAL)")
        check("[keys] 0 key events" in log, "no key reached the instrument")
        # c. port busy: a serial monitor holds the port
        with pg.Link(port, timeout=0.3):
            cwd = fresh(os.path.join(WORK, "stems-busy"))
            rc, log = run_app(["--pg-port", port], cwd, stems_script("app winshot busy"), "busy", env=base_env)
        check(re.search(r"\[stems\] device: error: .* is busy: close the serial monitor", log) is not None,
              "port busy: says so")
    finally:
        proc.kill()
        proc.wait()
    # b. no device (no port named, none found) and a project that does not load
    cwd = fresh(os.path.join(WORK, "stems-none"))
    os.makedirs(os.path.join(cwd, "projects"))
    with open(os.path.join(cwd, "projects", "BROKEN.pgv"), "wb") as f:
        f.write(b"not a project " * 20)
    rc, log = run_app([], cwd, stems_script("sdl tap Return", "app wait-stems 120000", "app winshot failed"), "none",
                      timeout=180, env=base_env)
    check("[stems] device: error: Pocket Groove not found on USB" in log, "no device: says so")
    check(re.search(r"\[stems\] failed: BROKEN.pgv did not load: ", log) is not None,
          "a project that does not load: says so")
    # d. pyserial missing: a python3 without site-packages first on PATH, no PlatformIO Python
    cwd = fresh(os.path.join(WORK, "stems-nopyserial"))
    fake = os.path.join(cwd, "fakebin")
    os.makedirs(fake)
    with open(os.path.join(fake, "python3"), "w") as f:
        f.write("#!/bin/sh\nexec %s -S \"$@\"\n" % sys.executable)
    os.chmod(os.path.join(fake, "python3"), 0o755)
    env = dict(base_env, PATH=fake + os.pathsep + os.environ.get("PATH", ""), HOME=cwd)
    env.pop("PG_PYTHON", None)
    rc, log = run_app([], cwd, stems_script("app winshot nopyserial"), "nopyserial", env=env)
    check("[stems] device: error: pyserial is missing" in log, "pyserial missing: says so")


LAB_TEST_SET = """voice bass
name TEST
macro A
  cutoff 0:10 1:110
  reso   0:20 1:120
"""
LAB_SCRIPT = """wait 300
down sft
tap j
up sft
app lab-select bass test
app lab-wait-synced
app lab-print bass
app lab-drag 0 0.5
wait 200
app lab-wait-synced
app lab-print bass
app lab-wait-reloads 1 10000
app lab-wait-synced
app lab-print bass
wait 1500
app lab-print bass
app lab-pad 1.0 0.0
wait 200
app lab-wait-synced
app lab-print bass
shot lab
app winshot lab-window
sdl down Left GUI
sdl tap G
sdl up Left GUI
wait 100
app lab-type harsh up here
wait 100
sdl tap Return
wait 100
app lab-click bad
wait 100
sdl tap Escape
wait 100
app lab-select bass acid
app lab-wait-synced
app lab-print bass
sdl down Left GUI
sdl tap Left
sdl up Left GUI
wait 200
app lab-wait-synced
app lab-print bass
app lab-select bass test
app lab-wait-synced
app lab-print bass
app lab-wait-reloads 2 10000
wait 100
app lab-print bass
app print-keys
quit
"""


def engine_values(line):
    """{SHORT: value} from an [lab-engine] line."""
    return dict(kv.split("=") for kv in line.split()[2:])


def part_lab():
    print("8. the macro lab: the track's macros follow the mouse into the engine, hot reload, errors, marks, none")
    cwd = fresh(os.path.join(WORK, "lab"))
    shutil.copytree(os.path.join(ROOT, "macros"), os.path.join(cwd, "macros"))
    test_set = os.path.join(cwd, "macros", "bass", "test.pgm")
    with open(test_set, "w") as f:
        f.write(LAB_TEST_SET)
    script = os.path.join(cwd, "lab.txt")
    with open(script, "w") as f:
        f.write(LAB_SCRIPT)
    cmd = [APP, "--lab", "--script", script, "--out", os.path.join(cwd, "shots")]
    p = subprocess.Popen(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    engine, state, log = [], [], []
    deadline = time.time() + 90
    for raw in p.stdout:
        line = raw.decode(errors="replace").rstrip("\n")
        log.append(line)
        sys.stdout.write("    | " + line + "\n")
        if line.startswith("[lab-engine]"):
            engine.append(engine_values(line))
            if len(engine) == 2:     # after the drag: a new version of the file (same second, other curve)
                with open(test_set, "w") as f:
                    f.write(LAB_TEST_SET.replace("cutoff 0:10 1:110", "cutoff 0:30 1:50"))
            elif len(engine) == 3:   # then a broken one: the last good version must stay
                with open(test_set, "w") as f:
                    f.write(LAB_TEST_SET.replace("reso   0:20 1:120", "reso   0:20 1:300"))
            elif len(engine) == 8:   # then it goes away: an extra instrument, so the track keeps its values
                os.remove(test_set)
        elif line.startswith("[lab-state]"):
            state.append(line)
        if time.time() > deadline:
            p.kill()
            break
    rc = p.wait()
    log = "\n".join(log)
    check(rc == 0, "app exited 0 (every lab script command succeeded)")
    check(len(engine) == 9, "nine engine snapshots (got %d)" % len(engine))
    if len(engine) < 9:
        return
    e0, e1, e2, e3, e4, acid, e5, e6, e7 = engine
    check(e0["CUT"] == "10" and e0["RES"] == "20", "set chosen: CUT/RES at the macro's start (10/20): %s/%s"
          % (e0["CUT"], e0["RES"]))
    # (the slider's pixel for 0.5 is 0.505; positions are bytes, 0..255: 60 or 61)
    check(e1["CUT"] in ("60", "61") and e1["RES"] in ("70", "71"),
          "slider dragged to 0.5 by mouse: the engine has 60/70 (%s/%s)"
          % (e1["CUT"], e1["RES"]))
    check("[lab] reloaded macros/bass/test.pgm" in log, "the rewritten file was reloaded while playing")
    check(e2["CUT"] == "40" and e2["RES"] == e1["RES"], "the new curve is live at the same position: CUT 40 (%s)"
          % e2["CUT"])
    check(re.search(r"\[lab\] macros/bass/test\.pgm:5: 300 is out of range \(0\.\.127\) \(keeping the last good", log)
          is not None, "the broken version is reported with file, line and reason")
    check(e3 == e2, "the broken version changed nothing (last good kept)")
    check(e4["CUT"] == "50" and e4["RES"] == "120", "XY pad clicked at the right edge: A (X and Y with one macro, "
          "X wins) at 1.0: CUT 50 RES 120 (got %s/%s)" % (e4["CUT"], e4["RES"]))
    notes = read(os.path.join(cwd, "lab-notes.txt")).decode().splitlines()
    check(len(notes) == 2, "two marks in lab-notes.txt (%d)" % len(notes))
    if len(notes) == 2:
        check(re.match(r"\d{4}-\d\d-\d\d \d\d:\d\d:\d\d GOOD bass TEST macros/bass/test\.pgm@[0-9a-f]{8} A=1\.00 "
                       r"\| CUT=50 RES=120 \| harsh up here$", notes[0]) is not None,
              "the GOOD line: time, track, set, file@version, positions, values, the note: %s" % notes[0])
        check(re.match(r".* BAD  bass TEST .* A=1\.00 \| CUT=50 RES=120$", notes[1]) is not None,
              "the BAD line (button, Esc: no note): %s" % notes[1])
    # ACID is the bass's first instrument: Cmd+Left from it goes to none (CLASSIC), which keeps
    # the values the track has (instruments, docs/MACROLAB.md)
    check(acid["CUT"] != e4["CUT"] or acid["RES"] != e4["RES"], "choosing ACID changed the bass (%s/%s)"
          % (acid["CUT"], acid["RES"]))
    check("[lab] bass: none" in log, "Cmd+Left stepped from ACID to none")
    mixer = ("LVL", "PAN", "DLY", "REV", "DUCK")   # the mixer rows: the player's, with the instrument's level gone
    check(all(e5[k] == acid[k] for k in acid if k not in mixer),
          "none = CLASSIC: the bass keeps playing the sound ACID gave it")
    check(e6["CUT"] == "50" and e6["RES"] == "120", "choosing TEST again restores its macro position (A=1.00)")
    check("[lab] removed macros/bass/test.pgm" in log, "the deleted file was noticed")
    check(e7 == e6, "deleted while active (an extra instrument): the track keeps its values (CUT %s RES %s)"
          % (e7["CUT"], e7["RES"]))
    m = re.search(r"\[keys\] (\d+) key events reached the instrument", log)
    check(m is not None and int(m.group(1)) == 4, "only the script's 4 instrument key events reached the Ui; "
          "lab keys, clicks and the typed note did not (%s)" % (m and m.group(1)))
    w, h, win = png_rgb(os.path.join(cwd, "shots", "lab-window.png"))
    _, _, dev = png_rgb(os.path.join(cwd, "shots", "lab.png"))
    check(w == 240 + 184 and h == 135 + 47, "window shot with the lab is 424 x 182 (%d x %d)" % (w, h))
    check([r[:240 * 3] for r in win[:135]] == dev, "the device screen is untouched by the panel, pixel for pixel")


def main():
    global APP, UI_SHOT, AUDIO_REF, XFER_SIM, WORK, ANALYZE
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True, help="host build directory")
    ap.add_argument("--work", required=True, help="scratch directory (wiped)")
    ap.add_argument("--only", default="", help="comma-separated parts (keys,session,audio,xfer,strip,listen,stems,lab)")
    a = ap.parse_args()
    b = os.path.abspath(a.bin)
    APP, UI_SHOT = os.path.join(b, "pocket-groove"), os.path.join(b, "ui_shot")
    AUDIO_REF, XFER_SIM = os.path.join(b, "desktop_audio_ref"), os.path.join(b, "xfer_sim")
    ANALYZE = os.path.join(b, "listen_analyze")
    WORK = fresh(os.path.abspath(a.work))
    os.environ.setdefault("SDL_VIDEODRIVER", "dummy")
    os.environ.setdefault("SDL_AUDIODRIVER", "dummy")
    only = set(x for x in a.only.split(",") if x)
    parts = [("keys", part_keys), ("session", part_session), ("audio", part_audio), ("xfer", part_xfer),
             ("strip", part_strip), ("listen", part_listen), ("listen", part_listen_specs),
             ("listen", part_listen_sdl), ("stems", part_stems), ("lab", part_lab)]
    for name, fn in parts:
        if not only or name in only:
            fn()
    print("FAILED: %d check(s)" % len(failures) if failures else "OK")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
