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

    python3 host/app/tests/desktop_check.py --bin build/host --work build/host/desktop_test
"""
import argparse
import os
import re
import shutil
import subprocess
import sys

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
        elif w[0] in ("expect-released", "quit"):
            continue
        out.append(line)
    return "\n".join(out) + "\n"


def run_app(args, cwd, script_text, name, timeout=60):
    script = os.path.join(cwd, name + ".txt")
    with open(script, "w") as f:
        f.write(script_text)
    cmd = [APP, "--script", script, "--out", os.path.join(cwd, "shots-" + name)] + args
    p = subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout)
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
    script = "wait 500\nsdl tap F9\nwait 300\ntap spc\nwait 4500\ntap spc\nwait 300\nsdl tap F9\nquit\n"
    for label, extra, ref_args in (("stereo-44k", [], []),
                                   ("mono-32k-buf100", ["--mono", "--sr", "32000", "--buffer", "100"],
                                    ["--mono", "--sr", "32000"])):
        cwd = fresh(os.path.join(WORK, "audio-" + label))
        rc, log = run_app(extra, cwd, script, "audio")
        check(rc == 0, "%s: app exited 0" % label)
        m = re.search(r"\[rec\] started: (\S+) at audio frame (\d+)", log)
        check(m is not None, "%s: F9 started a recording" % label)
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


def main():
    global APP, UI_SHOT, AUDIO_REF, XFER_SIM, WORK
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True, help="host build directory")
    ap.add_argument("--work", required=True, help="scratch directory (wiped)")
    a = ap.parse_args()
    b = os.path.abspath(a.bin)
    APP, UI_SHOT = os.path.join(b, "pocket-groove"), os.path.join(b, "ui_shot")
    AUDIO_REF, XFER_SIM = os.path.join(b, "desktop_audio_ref"), os.path.join(b, "xfer_sim")
    WORK = fresh(os.path.abspath(a.work))
    os.environ.setdefault("SDL_VIDEODRIVER", "dummy")
    os.environ.setdefault("SDL_AUDIODRIVER", "dummy")
    part_keys()
    part_session()
    part_audio()
    part_xfer()
    print("FAILED: %d check(s)" % len(failures) if failures else "OK")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
