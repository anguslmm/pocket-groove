#!/usr/bin/env python3
"""tools/pg-daisy: drive the Daisy Seed build of Pocket Groove from the computer
(stage 0: the Seed has no keys and no screen yet; docs/DAISY.md "Stage 0").

    tools/pg-daisy log                 everything the device prints (Ctrl-C ends)
    tools/pg-daisy report              the 'r' report (paste it back when asked)
    tools/pg-daisy tap KEY [KEY ...]   tap keys by legend (q spc ent sft fn G0 ...)
    tools/pg-daisy down KEY / up KEY   hold / release one
    tools/pg-daisy script FILE         a desktop key script (host/ui_scripts/*.txt);
                                       its "shot NAME" lines save NAME.png (--out DIR)
    tools/pg-daisy shot FILE.png       the screen now, as a PNG (--scale N)
    tools/pg-daisy mirror              a window showing the screen live; its keyboard
                                       plays the device (key down and up, like the
                                       desktop app); needs tkinter. Without it: keys
    tools/pg-daisy keys                the terminal as a keyboard: each character taps
                                       its key; Ctrl-G holds / releases G0, Ctrl-F fn,
                                       Ctrl-T ctl, Ctrl-O opt, Ctrl-A alt; an upper-case
                                       letter is SHIFT + the letter; Ctrl-R report;
                                       Ctrl-S screenshot; Esc or Ctrl-C quits
    tools/pg-daisy bootloader          restart into the Daisy bootloader (to flash)
    tools/pg-daisy pads [MS]           stage 1's force-sensor pads while you press them:
                                       a [pads] line every MS (default 100: raw readings,
                                       conductance, peaks, pressure, state, velocity
                                       and the note velocity it makes); Ctrl-C ends.
                                       Then tune with cmd (below)
    tools/pg-daisy pad N VEL [hold MS] [press P]
                                       hit pad N (1..16) from the computer, no sensor
                                       needed: velocity VEL 0..1 (0.786 = note velocity
                                       100), released after MS (default 100), pressure
                                       P 0..1 while held; prints what the pad plays
                                       and what the engine hears while held and after.
                                       The same path as a sensor's hit (App::padEvent)
    tools/pg-daisy cmd WORDS...        one '>' command, its answer printed, e.g.
                                       cmd pad 1 thr 15 ceil 600 / cmd pads / cmd display turn
                                       / cmd touch 120 67 (a touch at canvas 120, 67)
                                       / cmd pads job notes (the pads' job: drums or notes;
                                       also the key padjob: tap padjob)

In the mirror window a touch on the device's screen (stage 1's FT6206) shows as a
red dot, from the "~PGT" lines the device sends while the mirror is on.

The serial protocol (one port, shared with tools/pg's transfer lines "~PGX"):
'>' + a key-script command per line (">tap q 40", ">down sft", ">wait 100",
">release", ">shot NAME", ">sync TAG"), single letters (r report, h help, f one
frame, m mirror on/off, S save /int now, B / R + y restart), and the screen comes
back as "~PGF" lines (daisy/core/fb_stream.h). The port is found by USB id (libDaisy's
0483:5740, as tools/pg does), or --port / $PG_PORT. Python 3 with pyserial.
"""
import argparse
import base64
import os
import re
import struct
import sys
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import pg  # noqa: E402  (find_port, PgError)

try:
    import serial  # noqa: E402
except ImportError:  # pragma: no cover
    serial = None

LEGENDS = set("` 1 2 3 4 5 6 7 8 9 0 - = del tab q w e r t y u i o p [ ] \\ fn sft a s d f g h j k l ; ' ent "
              "ctl opt alt z x c v b n m , . / spc G0".split())


class Device:
    def __init__(self, port, echo=None):
        if serial is None:
            raise pg.PgError("pyserial is missing (pip install pyserial)")
        self.s = serial.Serial(port, 115200, timeout=0.05)
        self.buf = b""
        self.echo = echo
        self.frame = None   # the screen: {"w", "h", "px": bytearray, "pal": [...]} once one arrived
        self.frames = 0
        self.last_name = None
        self.touch = None   # (down, x, y) in canvas coordinates, from "~PGT D|U X Y"
        self.touches = 0

    def close(self):
        self.s.close()

    def send(self, text):
        self.s.write(text.encode("ascii"))
        self.s.flush()

    def key(self, cmd):
        self.send(">%s\n" % cmd)

    def lines(self, seconds):
        """Yields received lines for `seconds` (screen lines are decoded, not yielded)."""
        end = time.time() + seconds
        while time.time() < end:
            data = self.s.read(4096)
            if data:
                self.buf += data
            while b"\n" in self.buf:
                line, self.buf = self.buf.split(b"\n", 1)
                text = line.decode("ascii", "replace").rstrip("\r")
                if text.startswith("~PGF "):
                    self._screen(text)
                    continue
                if text.startswith("~PGT "):
                    p = text.split()
                    if len(p) == 4 and p[1] in ("D", "U"):
                        try:
                            self.touch = (p[1] == "D", int(p[2]), int(p[3]))
                            self.touches += 1
                        except ValueError:
                            pass
                    continue
                if self.echo:
                    self.echo(text)
                yield text

    def pump(self, seconds):
        for _ in self.lines(seconds):
            pass

    def wait_for(self, pred, seconds):
        for line in self.lines(seconds):
            if pred(line):
                return line
        return None

    def _screen(self, text):
        parts = text.split(" ")
        kind = parts[1]
        if kind == "B":
            w, h = int(parts[3]), int(parts[4])
            if not self.frame or self.frame["w"] != w:
                self.frame = {"w": w, "h": h, "px": bytearray(w * h), "pal": [0] * 256, "dirty": set()}
            self.last_name = parts[7] if len(parts) > 7 else "-"
        elif kind == "P" and self.frame:
            raw = base64.b64decode(parts[2])
            self.frame["pal"] = [raw[2 * i] | raw[2 * i + 1] << 8 for i in range(256)]
        elif kind == "R" and self.frame:
            y = int(parts[2])
            row = base64.b64decode(parts[3])
            w = self.frame["w"]
            self.frame["px"][y * w:(y + 1) * w] = row
            self.frame["dirty"].add(y)
        elif kind == "E":
            self.frames += 1

    def sync(self, tag="s", seconds=5.0):
        """Waits until the device has run everything queued before this point."""
        self.key("sync %s" % tag)
        return self.wait_for(lambda l: l.strip() == "[key] sync %s" % tag, seconds) is not None

    def ask(self, cmd, seconds=5.0):
        """One '>' command; the lines it printed (everything before its sync)."""
        tag = "ask%d" % int(time.time() * 1000 % 100000)
        self.key(cmd)
        self.key("sync %s" % tag)
        out = []
        for line in self.lines(seconds):
            if line.strip() == "[key] sync %s" % tag:
                return out
            out.append(line)
        return out

    def pads(self):
        """What the pads play now (">pads"): {"job": ..., pad number: {"track", "note",
        "inst", "held", "heard": [4], "press": (macro, offset)}}."""
        return parse_pads(self.ask("pads"))

    def shot(self, name="frame", seconds=5.0):
        before = self.frames
        self.key("shot %s" % name)
        end = time.time() + seconds
        while time.time() < end and self.frames == before:
            self.pump(0.05)
        return self.frames > before


PAD_LINE = re.compile(r"\[pads\] (\d+) plays (\S+) note (\d+) \((\S+)\)( HELD)? heard (\d+) (\d+) (\d+) (\d+) "
                      r"press (-?\d+) ([+-]\d+)")


def parse_pads(lines):
    out = {}
    for line in lines:
        m = re.match(r"\[pads\] job (\w+);", line)
        if m:
            out["job"] = m.group(1)
            continue
        m = PAD_LINE.match(line)
        if m:
            out[int(m.group(1))] = {"track": m.group(2), "note": int(m.group(3)), "inst": m.group(4),
                                    "held": bool(m.group(5)), "heard": [int(m.group(i)) for i in range(6, 10)],
                                    "press": (int(m.group(10)), int(m.group(11)))}
    return out


def hit(dev, pad, vel, hold=100, press=None):
    """Pad `pad` (1..16) hit from the computer (">hit"): velocity 0..1, released after
    hold ms (0: held until ">hit N up"), pressure 0..1 while held."""
    cmd = "hit %d %s hold %d" % (pad, vel, hold)
    if press is not None:
        cmd += " press %s" % press
    return dev.ask(cmd)


def rgb(c):
    r, g, b = (c >> 11) & 31, (c >> 5) & 63, c & 31
    return (r * 255 // 31, g * 255 // 63, b * 255 // 31)


def write_png(path, frame, scale=1):
    w, h = frame["w"], frame["h"]
    pal = [rgb(c) for c in frame["pal"]]
    raw = bytearray()
    for y in range(h):
        row = bytearray()
        for x in range(w):
            row += bytes(pal[frame["px"][y * w + x]]) * scale
        for _ in range(scale):
            raw += b"\x00" + row

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w * scale, h * scale, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(bytes(raw), 6)))
        f.write(chunk(b"IEND", b""))


def run_script(dev, path, out, scale):
    """A desktop key script, line by line: keys go to the device, waits happen here (so a
    long script never fills the device's queue), shots are saved as PNGs."""
    n = 0
    with open(path) as fh:
        lines = fh.readlines()
    for raw in lines:
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        words = line.split()
        verb = words[0]
        if verb == "wait":
            dev.pump(int(words[1]) / 1000.0)
        elif verb in ("down", "up", "tap"):
            dev.key(line)
            if verb == "tap":
                dev.pump((int(words[2]) if len(words) > 2 else 40) / 1000.0)
        elif verb == "shot":
            name = words[1] if len(words) > 1 else "shot%d" % n
            dev.sync("pre-%s" % name)
            if dev.shot(name):
                os.makedirs(out, exist_ok=True)
                write_png(os.path.join(out, name + ".png"), dev.frame, scale)
                print("saved %s" % os.path.join(out, name + ".png"))
            else:
                print("no frame for %s" % name)
        elif verb in ("battery", "sd", "boot", "demo", "empty", "sdl", "focuslost", "app", "quit"):
            continue   # the desktop's / ui_shot's own lines
        else:
            dev.key(line)
        n += 1
    dev.sync("end")
    dev.key("release")


TERMINAL = {"\r": "ent", "\n": "ent", " ": "spc", "\x7f": "del", "\x08": "del", "\t": "tab"}
STICKY = {"\x07": "G0", "\x06": "fn", "\x14": "ctl", "\x0f": "opt", "\x01": "alt"}


def keys_terminal(dev):
    import termios
    import tty
    import select
    fd = sys.stdin.fileno()
    old = termios.tcgetattr(fd)
    held = set()
    print("keys: type to tap; Ctrl-G G0, Ctrl-F fn, Ctrl-T ctl, Ctrl-O opt, Ctrl-A alt (hold / release); "
          "upper case = shift; Ctrl-R report, Ctrl-S screenshot; Esc quits\r")
    try:
        tty.setraw(fd)
        while True:
            r, _, _ = select.select([fd], [], [], 0.02)
            dev.pump(0.0)
            if not r:
                continue
            ch = os.read(fd, 1).decode("latin-1")
            if ch in ("\x1b", "\x03"):
                break
            if ch in STICKY:
                k = STICKY[ch]
                dev.key(("up " if k in held else "down ") + k)
                held.symmetric_difference_update({k})
                sys.stdout.write("[%s %s]\r\n" % (k, "held" if k in held else "released"))
                continue
            if ch == "\x12":
                dev.send("r")
                continue
            if ch == "\x13":
                if dev.shot("keys"):
                    name = time.strftime("pg-daisy-%H%M%S.png")
                    write_png(name, dev.frame, 3)
                    sys.stdout.write("saved %s\r\n" % name)
                continue
            k = TERMINAL.get(ch, ch.lower())
            if k not in LEGENDS:
                continue
            if ch.isupper():
                dev.key("down sft")
                dev.key("tap %s" % k)
                dev.key("up sft")
            else:
                dev.key("tap %s" % k)
    finally:
        termios.tcsetattr(fd, termios.TCSADRAIN, old)
        dev.key("release")


TK_KEYS = {"space": "spc", "Return": "ent", "BackSpace": "del", "Tab": "tab", "Shift_L": "sft", "Shift_R": "sft",
           "Control_L": "ctl", "Control_R": "ctl", "Alt_L": "alt", "Alt_R": "alt", "Option_L": "opt",
           "Option_R": "opt", "Meta_L": "opt", "Super_L": "fn", "F1": "G0", "grave": "`", "minus": "-",
           "equal": "=", "bracketleft": "[", "bracketright": "]", "backslash": "\\", "semicolon": ";",
           "apostrophe": "'", "comma": ",", "period": ".", "slash": "/"}


def mirror(dev, scale):
    try:
        import tkinter as tk
    except ImportError:
        print("pg-daisy: no tkinter here (macOS Homebrew: brew install python-tk); using the terminal")
        dev.send("m")
        return keys_terminal(dev)
    root = tk.Tk()
    root.title("Pocket Groove (Daisy) - F1 = G0")
    img = tk.PhotoImage(width=240 * scale, height=135 * scale)
    tk.Label(root, image=img, bd=0).pack()
    held = set()

    def legend(ev):
        k = TK_KEYS.get(ev.keysym, ev.keysym.lower() if len(ev.keysym) == 1 else None)
        return k if k in LEGENDS else None

    def press(ev):
        k = legend(ev)
        if k and k not in held:
            held.add(k)
            dev.key("down %s" % k)

    def release(ev):
        k = legend(ev)
        if k and k in held:
            held.discard(k)
            dev.key("up %s" % k)

    root.bind("<KeyPress>", press)
    root.bind("<KeyRelease>", release)
    dev.send("m")   # the device streams the rows the UI changes

    dot = {"at": None, "seen": 0}   # the touch dot drawn now: (x, y) in canvas pixels

    def tick():
        dev.pump(0.005)
        f = dev.frame
        if dev.touches != dot["seen"] and f:   # the touch moved: repaint under the old dot
            dot["seen"] = dev.touches
            if dot["at"]:
                f["dirty"].update(y for y in range(dot["at"][1] - 2, dot["at"][1] + 3) if 0 <= y < 135)
            down, x, y = dev.touch
            dot["at"] = (x, y) if down else None
        if f and f["dirty"]:
            pal = ["#%02x%02x%02x" % rgb(c) for c in f["pal"]]
            for y in sorted(f["dirty"]):
                row = "{" + " ".join(pal[f["px"][y * 240 + x]] for x in range(240) for _ in range(scale)) + "}"
                for k in range(scale):
                    img.put(row, to=(0, y * scale + k))
            f["dirty"].clear()
        if dot["at"]:
            x, y = dot["at"]
            x0, y0 = max(0, (x - 2) * scale), max(0, (y - 2) * scale)
            x1, y1 = min(240 * scale, (x + 3) * scale), min(135 * scale, (y + 3) * scale)
            if x0 < x1 and y0 < y1:   # a touch on the border, outside the canvas, is not drawn
                img.put("#ff2020", to=(x0, y0, x1, y1))
        root.after(15, tick)

    root.after(15, tick)
    try:
        root.mainloop()
    finally:
        dev.send("m")
        dev.key("release")


def main(argv=None):
    ap = argparse.ArgumentParser(prog="tools/pg-daisy", description=__doc__.split("\n")[0])
    ap.add_argument("--port", help="serial port (default: the Daisy by USB id, or $PG_PORT)")
    ap.add_argument("--out", default="shots", help="where script shots go (default ./shots)")
    ap.add_argument("--scale", type=int, default=3, help="PNG / window scale (default 3)")
    ap.add_argument("cmd")
    ap.add_argument("args", nargs="*")
    a = ap.parse_args(argv)
    try:
        port = a.port or os.environ.get("PG_PORT") or pg.find_port()
        dev = Device(port)
    except (pg.PgError, OSError) as e:
        print("pg-daisy: %s" % e)
        return 1
    try:
        c = a.cmd
        if c == "log":
            dev.echo = print
            try:
                while True:
                    dev.pump(1.0)
            except KeyboardInterrupt:
                return 0
        elif c == "report":
            dev.send("r")
            out = []
            for line in dev.lines(5.0):
                if "REPORT BEGIN" in line:
                    out = [line]
                elif out:
                    out.append(line)
                    if "REPORT END" in line:
                        break
            print("\n".join(out) if out else "pg-daisy: no report (is this the Daisy's port?)")
            return 0 if out else 1
        elif c in ("tap", "down", "up"):
            for k in a.args:
                dev.key("%s %s" % (c, k))
            dev.sync("cli")
        elif c == "script":
            run_script(dev, a.args[0], a.out, a.scale)
        elif c == "shot":
            if not dev.shot("cli"):
                print("pg-daisy: no frame came back")
                return 1
            write_png(a.args[0] if a.args else "pg-daisy.png", dev.frame, a.scale)
        elif c == "keys":
            dev.echo = lambda l: sys.stdout.write(l + "\r\n")
            keys_terminal(dev)
        elif c == "mirror":
            mirror(dev, a.scale)
        elif c == "pads":
            ms = int(a.args[0]) if a.args else 100
            dev.key("pads")
            dev.key("pads watch %d" % ms)
            try:
                for line in dev.lines(1e9):
                    if line.startswith("[pads]"):
                        print(line)
            except KeyboardInterrupt:
                pass
            finally:
                dev.key("pads watch off")
                dev.pump(0.1)
        elif c == "pad":
            usage = "pg-daisy: pad N VEL [hold MS] [press P]   (N 1..16, VEL and P 0..1)"
            try:
                pad, vel = int(a.args[0]), float(a.args[1])
                opts = dict(zip(a.args[2::2], a.args[3::2]))
                if len(a.args) % 2 or set(opts) - {"hold", "press"}:
                    raise ValueError
                hold = int(opts.get("hold", 100))
                press = float(opts["press"]) if "press" in opts else None
            except (IndexError, ValueError):
                print(usage)
                return 2
            for line in hit(dev, pad, vel, hold, press):
                if line.startswith("[pads] ?"):
                    print(line)
                    return 2

            def show(when):
                p = dev.pads()
                if pad in p:
                    d = p[pad]
                    print("%-6s pad %d (job %s) plays %s note %d (%s)%s: heard %s, press macro %d %+d" % (
                        when, pad, p.get("job", "?"), d["track"], d["note"], d["inst"], " HELD" if d["held"] else "",
                        " ".join(str(v) for v in d["heard"]), d["press"][0], d["press"][1]))
            show("held")   # the round trip is about a UI frame: the engine's status has the hit
            if hold > 0:
                dev.pump(hold / 1000.0 + 0.2)
                show("after")
        elif c == "cmd":
            if not a.args:
                print("pg-daisy: cmd needs words, e.g. cmd pad 1 thr 15")
                return 2
            dev.echo = lambda l: None if l.startswith("[key] sync") or l.startswith("[stats]") else print(l)
            dev.key(" ".join(a.args))
            dev.sync("cmd")
            dev.pump(0.3)
        elif c == "bootloader":
            dev.send("B")
            time.sleep(0.2)
            dev.send("y")
            print("pg-daisy: the Seed restarts into its bootloader (its LED pulses): flash now")
        else:
            ap.print_help()
            return 2
        return 0
    finally:
        dev.close()


if __name__ == "__main__":
    sys.exit(main())
