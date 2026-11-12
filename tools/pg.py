"""Pocket Groove computer-side tool: projects over USB, and stem bounces.

Run it through the launcher, from the repository root:

    tools/pg ls                       # projects on the device (internal flash and SD card)
    tools/pg pull MYSONG              # copy MYSONG to ./projects/MYSONG.pgv
    tools/pg pull session             # the autosaved session (what was playing at power-off)
    tools/pg bounce MYSONG            # pull, then render stems to ./bounces/MYSONG/
    tools/pg bounce path/to/file.pgv  # render a file already on disk
    tools/pg push path/to/file.pgv    # copy a project to the device (--name, --sd, --force)

Needs only pyserial. The protocol (pg/io/xfer.h on the device): every line is
"~PGX <body> <crc32 hex>"; the host sends one request and waits for its reply,
resending on a timeout (every request is idempotent), so a lost or corrupted
byte costs a retry, never a wrong file. Whole files are checked with CRC32 at
both ends; the device installs a pushed file with the same verify-and-rename
as its own saves.

Opening the port. Two firmware builds, two kinds of port, both found
automatically (Espressif VID 0x303A; product "Pocket Groove" preferred):
  * env:cardputer-adv: the ESP32-S3's built-in USB-Serial/JTAG. It resets the
    chip when the host sets RTS while DTR is clear (that is how esptool enters
    the bootloader). The operating system raises DTR and RTS together when the
    port is opened and pyserial keeps both raised, so opening the port does NOT
    reset the device.
  * env:cardputer-adv-usb (stereo USB audio): a TinyUSB CDC port of the
    composite device "Pocket Groove". The firmware ignores DTR/RTS entirely;
    the only thing that restarts it is the port being opened at 1200 baud
    (PlatformIO's upload "touch", which reboots it into the bootloader).
This tool never touches DTR/RTS and always opens at BAUD (115200), never 1200
(test_pg.py checks both).
"""
import argparse
import base64
import os
import random
import re
import shutil
import subprocess
import sys
import time
import zlib

try:
    import serial
    import serial.tools.list_ports
except ImportError:  # pragma: no cover - the launcher prints the fix
    serial = None

ESPRESSIF_VID = 0x303A
USJ_PID = 0x1001                  # the chip's USB-Serial/JTAG (and the variant's TinyUSB PID, same number)
PRODUCT = "Pocket Groove"         # USB product string of the USB audio build
BAUD = 115200                     # NEVER 1200: that reboots the USB audio build into its bootloader
assert BAUD != 1200
PREFIX = b"~PGX "
PROTO_VERSION = 1
CHUNK = 128
NAME_RE = re.compile(r"^[A-Z0-9_-]{1,8}$")
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

ERRORS = {
    "PLAYING": "the device is playing. Stop the sequencer first (saving to flash interrupts the audio), "
               "or add --force",
    "NOT_FOUND": "no such project on the device",
    "NO_MOUNT": "no SD card in the device",
    "NO_SPACE": "not enough free space on the device",
    "BAD_NAME": "invalid project name (1-8 characters: A-Z 0-9 - _)",
    "BAD_SIZE": "that file is too small or too big to be a Pocket Groove project",
    "NO_STORAGE": "the device has no storage",
}


class PgError(Exception):
    pass


def frame(body):
    data = body.encode("ascii")
    return PREFIX + data + b" %08X\n" % (zlib.crc32(data) & 0xFFFFFFFF)


def parse_line(line):
    """Returns the body of a valid protocol line, or None (log line, noise, bad CRC)."""
    i = line.rfind(PREFIX)
    if i < 0:
        return None
    rest = line[i + len(PREFIX):].rstrip(b"\r\n")
    sp = rest.rfind(b" ")
    if sp < 0 or len(rest) - sp - 1 != 8:
        return None
    body, crc = rest[:sp], rest[sp + 1:]
    try:
        if int(crc, 16) != zlib.crc32(body) & 0xFFFFFFFF:
            return None
        return body.decode("ascii")
    except ValueError:
        return None


# ---- the link --------------------------------------------------------------

def is_pocket_groove(p):
    """The USB audio build's CDC port: its USB product string (pyserial: product, or on some
    systems only the description) is "Pocket Groove"."""
    return p.vid == ESPRESSIF_VID and any(PRODUCT in (x or "") for x in (p.product, p.description))


def find_port(ports=None):
    """The device's serial port. `ports`: pyserial ListPortInfo-like objects (default: the system's)."""
    if ports is None:
        if serial is None:
            raise PgError("pyserial is missing")
        ports = serial.tools.list_ports.comports()
    ports = [p for p in ports if p.vid == ESPRESSIF_VID]
    # macOS lists each device twice (/dev/cu.* and /dev/tty.*): use cu.*, which does not wait for carrier.
    ports.sort(key=lambda p: (not p.device.startswith("/dev/cu."), p.device))
    seen, uniq = set(), []
    for p in ports:
        key = p.serial_number or p.location or p.device
        if key not in seen:
            seen.add(key)
            uniq.append(p)
    if not uniq:
        raise PgError("Pocket Groove not found on USB. Plug it in with a data cable (USB-C) and switch it on, "
                      "or name the port with --port")
    if len(uniq) > 1:
        # The USB audio build names itself; the USB-Serial/JTAG of the normal build does not
        # (any ESP32-S3 board shows the same 303a:1001 "USB JTAG/serial debug unit").
        named = [p for p in uniq if is_pocket_groove(p)]
        if len(named) == 1:
            return named[0].device
        raise PgError("several Espressif devices found (%s): choose one with --port" %
                      ", ".join(p.device for p in uniq))
    return uniq[0].device


class Link:
    def __init__(self, port, timeout=0.6, retries=8, verbose=False):
        self.port_name = port
        self.timeout = timeout
        self.retries = retries
        self.verbose = verbose
        self.buf = b""
        self.next_id = random.randint(1, 50000)
        self.retried = 0
        try:
            # DTR/RTS stay as the OS raised them on open (both set): no reset, see the module doc.
            # Always BAUD, never 1200 (the USB audio build reboots into its bootloader at 1200).
            self.ser = serial.Serial(port, BAUD, timeout=0.02, write_timeout=2, exclusive=True)
        except (serial.SerialException, OSError) as e:
            msg = str(e)
            if "busy" in msg.lower() or "lock" in msg.lower() or getattr(e, "errno", None) == 16:
                raise PgError("%s is busy: close the serial monitor (pio device monitor, screen, ...) "
                              "and try again" % port)
            if "No such file" in msg or getattr(e, "errno", None) == 2:
                raise PgError("%s does not exist (device unplugged?)" % port)
            raise PgError("cannot open %s: %s" % (port, msg))

    def close(self):
        try:
            self.ser.close()
        except Exception:
            pass

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.close()

    def _lines(self, deadline):
        """Yields complete lines until the deadline."""
        while True:
            nl = self.buf.find(b"\n")
            if nl >= 0:
                line, self.buf = self.buf[:nl + 1], self.buf[nl + 1:]
                yield line
                continue
            if time.monotonic() >= deadline:
                return
            try:
                chunk = self.ser.read(4096)
            except serial.SerialException as e:
                raise PgError("lost the connection to %s: %s" % (self.port_name, e))
            if chunk:
                self.buf += chunk
                if len(self.buf) > 1 << 20:
                    self.buf = self.buf[-4096:]

    def request(self, cmd, until=None, rid=None):
        """Sends `cmd`, returns the list of reply bodies (id stripped).

        until: predicate on a reply body that ends the reply (default: the first reply).
        The request is resent with the same id on a timeout; ids make stale replies harmless."""
        if rid is None:
            rid = self.next_id
            self.next_id = self.next_id % 99999 + 1
        prefix = "%d " % rid
        line = b"\n" + frame(prefix + cmd)
        assert len(line) <= 241, "request line too long"
        for attempt in range(self.retries):
            if attempt:
                self.retried += 1
            got = []
            try:
                self.ser.write(line)
                self.ser.flush()
            except serial.SerialException as e:
                raise PgError("lost the connection to %s: %s" % (self.port_name, e))
            for raw in self._lines(time.monotonic() + self.timeout * (1 + attempt * 0.5)):
                body = parse_line(raw)
                if body is None:
                    if self.verbose and raw.strip():
                        sys.stderr.write("device: %s\n" % raw.decode("ascii", "replace").rstrip())
                    continue
                if not body.startswith(prefix):
                    continue   # a late reply to an earlier request
                body = body[len(prefix):]
                got.append(body)
                if body.startswith("ERR ") or until is None or until(body):
                    return got
            # timeout: resend
        raise PgError("the device did not answer (%s). Is the Pocket Groove firmware with USB transfer "
                      "flashed and running, and no serial monitor open?" % self.port_name)

    def one(self, cmd, rid=None):
        return self.request(cmd, rid=rid)[-1]


def check(reply, what):
    if reply.startswith("ERR "):
        parts = reply.split()
        code = parts[1] if len(parts) > 1 else "?"
        detail = " ".join(parts[2:])
        msg = ERRORS.get(code, code.replace("_", " ").lower())
        raise PgError("%s: %s%s" % (what, msg, (" (%s)" % detail) if detail and code not in ERRORS else ""))
    return reply


def hello(link):
    r = check(link.one("HELLO"), "hello")
    p = r.split()
    if len(p) < 5 or p[0] != "HI" or p[1] != "PGX":
        raise PgError("unexpected answer from the device: %s" % r)
    if int(p[2]) != PROTO_VERSION:
        raise PgError("the device speaks transfer protocol %s, this tool %d: update the firmware or this "
                      "repository" % (p[2], PROTO_VERSION))
    return {"firmware": p[3], "playing": p[4] == "1"}


def list_projects(link):
    rep = link.request("LS", until=lambda b: b.startswith("END "))
    end = check(rep[-1], "list")
    ents = []
    for b in rep[:-1]:
        p = b.split()
        if len(p) == 6 and p[0] == "ENT":
            ents.append({"mount": p[1], "name": p[2], "size": int(p[3]), "bpm": int(p[4]), "valid": p[5] == "1"})
    e = end.split()
    if len(e) < 4 or int(e[1]) != len(ents):
        # an ENT line was lost: ask again (LS is read-only)
        rep = link.request("LS", until=lambda b: b.startswith("END "))
        ents = [dict(zip(("mount", "name", "size", "bpm", "valid"),
                         (p[1], p[2], int(p[3]), int(p[4]), p[5] == "1")))
                for p in (b.split() for b in rep[:-1]) if len(p) == 6 and p[0] == "ENT"]
        e = rep[-1].split()
        if len(e) < 4 or int(e[1]) != len(ents):
            raise PgError("the project list keeps arriving incomplete (bad cable?)")
    return ents, e[2] == "1", e[3] == "1"


def get_file(link, mount, name, progress=None):
    r = check(link.one("GET %s %s" % (mount, name)), "%s" % name)
    p = r.split()
    if p[0] != "FILE":
        raise PgError("unexpected answer: %s" % r)
    size, crc = int(p[1]), int(p[2], 16)
    data = bytearray()
    while len(data) < size:
        d = check(link.one("RD %d" % len(data)), "read")
        q = d.split()
        if len(q) != 3 or q[0] != "DATA" or int(q[1]) != len(data):
            continue   # out of step: ask again for the same offset
        chunk = base64.b64decode(q[2], validate=True)
        if not chunk:
            raise PgError("empty chunk from the device")
        data += chunk
        if progress:
            progress(len(data), size)
    link.one("END")
    if len(data) != size or zlib.crc32(bytes(data)) & 0xFFFFFFFF != crc:
        raise PgError("transfer of %s failed its CRC check: try again" % name)
    return bytes(data)


# ---- .pgv files --------------------------------------------------------------

def pgv_header(data):
    if len(data) < 64 or data[:4] != b"PGVF":
        raise PgError("not a Pocket Groove project file (.pgv)")
    hs = data[6] | data[7] << 8
    if hs < 64 or hs > len(data):
        raise PgError("damaged .pgv header")
    want = int.from_bytes(data[hs - 4:hs], "little")
    if zlib.crc32(data[:hs - 4]) & 0xFFFFFFFF != want:
        raise PgError("damaged .pgv header (CRC)")
    stem = data[28:44].split(b"\0")[0].decode("ascii", "replace")
    return {"headerSize": hs, "tempo": data[10], "stem": stem, "compat": data[5]}


def with_stem(data, stem):
    """The file with its header stem set to `stem` (the device installs a pushed file under it)."""
    h = pgv_header(data)
    hs = h["headerSize"]
    b = bytearray(data)
    s = stem.encode("ascii")[:15]
    b[28:44] = s + b"\0" * (16 - len(s))
    b[hs - 4:hs] = (zlib.crc32(bytes(b[:hs - 4])) & 0xFFFFFFFF).to_bytes(4, "little")
    return bytes(b)


def normalize_name(n):
    n = re.sub(r"[^A-Z0-9_-]", "", n.upper())[:8]
    if not NAME_RE.match(n):
        raise PgError("project names are 1-8 characters of A-Z 0-9 - _")
    return n


def put_file(link, mount, name, data, force=False, progress=None):
    data = with_stem(data, name)
    crc = zlib.crc32(data) & 0xFFFFFFFF
    r = check(link.one("PUT %s %s %d %08X %d" % (mount, name, len(data), crc, 1 if force else 0)), "push")
    p = r.split()
    if p[0] != "READY":
        raise PgError("unexpected answer: %s" % r)
    chunk = min(CHUNK, int(p[1]))
    off = 0
    while off < len(data):
        piece = data[off:off + chunk]
        a = check(link.one("WR %d %s" % (off, base64.b64encode(piece).decode("ascii"))), "push")
        q = a.split()
        if q[0] == "ACK":
            off = int(q[1])
        elif q[0] == "NAK":
            off = int(q[1])   # resume where the device is
        else:
            raise PgError("unexpected answer: %s" % a)
        if progress:
            progress(off, len(data))
    d = check(link.one("DONE"), "push")   # resent with the same id on a timeout: the device answers again
    if not d.startswith("SAVED "):
        raise PgError("unexpected answer: %s" % d)
    return d.split()[1:]


# ---- commands ------------------------------------------------------------------

def open_link(args):
    if serial is None:
        raise PgError("pyserial is missing (see tools/pg)")
    port = args.port or os.environ.get("PG_PORT") or find_port()
    link = Link(port, timeout=args.timeout, verbose=args.verbose)
    try:
        info = hello(link)
    except Exception:
        link.close()
        raise
    return link, info


def resolve(link, name, want_mount=None):
    """(mount, device name) of a project, by name; 'session' / '_last' is the autosave."""
    if name.lower() in ("session", "_last"):
        return "int", "_last"
    n = normalize_name(name)
    ents, _, _ = list_projects(link)
    found = [e for e in ents if e["name"] == n and (want_mount is None or e["mount"] == want_mount)]
    if not found:
        names = ", ".join(sorted(set(e["name"] for e in ents if e["name"] != "_last"))) or "none"
        raise PgError("no project %s on the device (it has: %s)" % (n, names))
    if len(found) > 1:
        raise PgError("%s exists on the internal flash and on the SD card: add --int or --sd" % n)
    return found[0]["mount"], n


def bar(done, total):
    if sys.stderr.isatty():
        w = 30
        k = w * done // max(total, 1)
        sys.stderr.write("\r  [%s%s] %d/%d bytes" % ("#" * k, "." * (w - k), done, total))
        if done >= total:
            sys.stderr.write("\n")
        sys.stderr.flush()


def mount_arg(args):
    return "sd" if getattr(args, "sd", False) else ("int" if getattr(args, "int", False) else None)


def cmd_ls(args):
    link, info = open_link(args)
    with link:
        ents, sd, trunc = list_projects(link)
    print("Pocket Groove (firmware %s)%s" % (info["firmware"], ", PLAYING" if info["playing"] else ""))
    print("  %-10s %-6s %5s %8s" % ("NAME", "WHERE", "BPM", "BYTES"))
    for e in ents:
        name = "session" if e["name"] == "_last" else e["name"]
        where = "SD" if e["mount"] == "sd" else "flash"
        print("  %-10s %-6s %5s %8d%s" % (name, where, e["bpm"] or "-", e["size"], "" if e["valid"] else "  (unreadable)"))
    if not ents:
        print("  (no saved projects)")
    if not sd:
        print("  (no SD card)")
    if trunc:
        print("  (more files than the device lists at once)")
    return 0


def pull(args, name, out=None, out_dir="projects"):
    link, _ = open_link(args)
    with link:
        mount, dev = resolve(link, name, mount_arg(args))
        data = get_file(link, mount, dev, progress=bar)
    pgv_header(data)
    local = "SESSION" if dev == "_last" else dev
    path = out or os.path.join(out_dir, local + ".pgv")
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    tmp = path + ".part"
    with open(tmp, "wb") as f:
        f.write(data)
    os.replace(tmp, path)
    print("pulled %s (%s, %d bytes) -> %s" % (local, "SD" if mount == "sd" else "flash", len(data), path))
    return path


def cmd_pull(args):
    pull(args, args.name, out=args.out)
    return 0


def cmd_push(args):
    with open(args.file, "rb") as f:
        data = f.read()
    h = pgv_header(data)
    if args.name:
        name = args.name
    elif NAME_RE.match(h["stem"]):
        name = h["stem"]   # the name it was saved under
    else:
        name = os.path.splitext(os.path.basename(args.file))[0]
    name = normalize_name(name)
    mount = "sd" if args.sd else "int"
    link, info = open_link(args)
    with link:
        if info["playing"] and not args.force:
            raise PgError(ERRORS["PLAYING"])
        put_file(link, mount, name, data, force=args.force, progress=bar)
    print("pushed %s -> %s (%s); open it on the device from the PROJECT view" %
          (args.file, name, "SD card" if mount == "sd" else "internal flash"))
    return 0


def newest_mtime(paths):
    t = 0.0
    for top in paths:
        if os.path.isfile(top):
            t = max(t, os.path.getmtime(top))
            continue
        for d, _, files in os.walk(top):
            for f in files:
                if f.endswith((".cpp", ".h", ".txt")):
                    t = max(t, os.path.getmtime(os.path.join(d, f)))
    return t


def ensure_bounce_tool():
    build = os.path.join(ROOT, "build", "host")
    exe = os.path.join(build, "bounce")
    srcs = [os.path.join(ROOT, "lib", "pg", "src"), os.path.join(ROOT, "host", "common"),
            os.path.join(ROOT, "host", "tools", "bounce.cpp"), os.path.join(ROOT, "host", "CMakeLists.txt")]
    if os.path.exists(exe) and os.path.getmtime(exe) >= newest_mtime(srcs):
        return exe
    if not shutil.which("cmake"):
        raise PgError("cmake is needed to build the renderer (macOS: brew install cmake)")
    print("building the renderer (build/host/bounce)...", file=sys.stderr)
    if not os.path.exists(os.path.join(build, "CMakeCache.txt")):
        subprocess.run(["cmake", "-S", os.path.join(ROOT, "host"), "-B", build], check=True,
                       stdout=subprocess.DEVNULL)
    jobs = str(max(2, os.cpu_count() or 2))
    r = subprocess.run(["cmake", "--build", build, "--target", "bounce", "-j", jobs])
    if r.returncode != 0 or not os.path.exists(exe):
        raise PgError("building build/host/bounce failed (see above)")
    return exe


def cmd_bounce(args):
    src = args.what
    if src.lower().endswith(".pgv") or os.path.isfile(src):
        if not os.path.isfile(src):
            raise PgError("%s: no such file" % src)
        path = src
    else:
        path = pull(args, src)
    exe = ensure_bounce_tool()
    cmd = [exe, path]
    if args.out:
        cmd += ["--out", args.out]
    for opt in ("pattern", "loops", "tail", "sr", "bits", "name"):
        v = getattr(args, opt)
        if v is not None:
            cmd += ["--" + opt, str(v)]
    if args.wet_stems:
        cmd.append("--wet-stems")
    if args.no_duck:
        cmd.append("--no-duck")
    return subprocess.run(cmd).returncode


def main(argv=None):
    ap = argparse.ArgumentParser(prog="tools/pg", description="Pocket Groove: projects over USB, stem bounces.")
    ap.add_argument("--port", help="serial port (default: auto-detect Espressif USB, or $PG_PORT)")
    ap.add_argument("--timeout", type=float, default=0.6, help=argparse.SUPPRESS)
    ap.add_argument("-v", "--verbose", action="store_true", help="show the device's log lines")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("ls", help="list the projects on the device")
    p = sub.add_parser("pull", help="copy a project from the device to ./projects/NAME.pgv")
    p.add_argument("name", help="project name, or 'session' for the autosave")
    p.add_argument("--out", help="output file (default projects/NAME.pgv)")
    g = p.add_mutually_exclusive_group()
    g.add_argument("--sd", action="store_true", help="the copy on the SD card")
    g.add_argument("--int", action="store_true", help="the copy on the internal flash")
    p = sub.add_parser("push", help="copy a .pgv file to the device")
    p.add_argument("file")
    p.add_argument("--name", help="name on the device (default: the name stored in the file)")
    p.add_argument("--sd", action="store_true", help="to the SD card (default: internal flash)")
    p.add_argument("--force", action="store_true", help="even while the sequencer plays (audio drops out)")
    p = sub.add_parser("bounce", help="render stems: a project on the device (pulled first) or a .pgv file")
    p.add_argument("what", help="project name on the device, 'session', or a .pgv file")
    p.add_argument("--out", help="output directory (default bounces/NAME)")
    p.add_argument("--name", help="file name prefix (default: the project name)")
    p.add_argument("--pattern", type=int, help="render pattern P (1-16) instead of the song")
    p.add_argument("--loops", type=int, help="with --pattern: how many times (default 1)")
    p.add_argument("--tail", type=float, help="seconds of tail after the end, max (default 8)")
    p.add_argument("--sr", type=int, choices=(44100, 32000))
    p.add_argument("--bits", choices=("16", "24", "32f"), help="default 32f")
    p.add_argument("--wet-stems", action="store_true", help="each stem with its own delay/reverb")
    p.add_argument("--no-duck", action="store_true", help="stems without sidechain ducking")
    g = p.add_mutually_exclusive_group()
    g.add_argument("--sd", action="store_true", help=argparse.SUPPRESS)
    g.add_argument("--int", action="store_true", help=argparse.SUPPRESS)
    args = ap.parse_args(argv)
    try:
        return {"ls": cmd_ls, "pull": cmd_pull, "push": cmd_push, "bounce": cmd_bounce}[args.cmd](args)
    except PgError as e:
        print("pg: %s" % e, file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("pg: interrupted (the device drops the half transfer after 5 s)", file=sys.stderr)
        return 130
    except OSError as e:
        print("pg: %s" % e, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
