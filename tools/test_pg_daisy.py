#!/usr/bin/env python3
"""The computer side of the Daisy target against the firmware's own App code
(host/daisy/daisy_sim on a pseudo-terminal): tools/pg-daisy (keys, report, screen,
key scripts, pads hit from the computer) and tools/pg (projects and samples over the
same port), and /int and the sample pool surviving a restart through the simulated
QSPI flash.

    python3 tools/test_pg_daisy.py --sim build/host/daisy/daisy_sim

Exit 77 (skipped) without pyserial.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
try:
    import serial  # noqa: F401
except ImportError:
    print("test_pg_daisy: pyserial missing, skipped")
    sys.exit(77)
import pg  # noqa: E402
import pg_daisy  # noqa: E402

SIM = None


class Sim:
    def __init__(self, *args):
        self.proc = subprocess.Popen([SIM] + list(args), stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.port = self.proc.stdout.readline().decode().strip()
        if not self.port:
            raise RuntimeError("daisy_sim did not start: %s" % self.proc.stderr.read().decode())

    def stop(self):
        self.proc.terminate()
        self.proc.wait(5)
        self.proc.stdout.close()
        self.proc.stderr.close()


class DaisyTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="pgdaisy")
        self.sims = []

    def tearDown(self):
        for s in self.sims:
            if s.proc.poll() is None or not s.proc.stdout.closed:
                s.stop()
        shutil.rmtree(self.tmp, ignore_errors=True)

    def sim(self, *args):
        s = Sim(*args)
        self.sims.append(s)
        return s

    def report(self, dev):
        dev.send("r")
        out = []
        for line in dev.lines(5.0):
            if "REPORT BEGIN" in line:
                out = [line]
            elif out:
                out.append(line)
                if "REPORT END" in line:
                    return out
        self.fail("no report")

    def test_report_keys_screen(self):
        s = self.sim("--autoplay")
        dev = pg_daisy.Device(s.port)
        try:
            rep = self.report(dev)
            text = "\n".join(rep)
            for want in ("[audio] RUNNING, 48000 Hz, block 96 frames", "[engine]", "playing yes", "[task] serial",
                         "[fs] /int", "[samples]", "[xfer]", "[led]"):
                self.assertIn(want, text)
            # Keys: space stops the demo, as on the device's keyboard.
            dev.key("tap spc")
            self.assertTrue(dev.sync("a"))
            dev.pump(0.2)
            text = "\n".join(self.report(dev))
            self.assertIn("playing no", text)
            self.assertIn("[keys] from the computer 2", text)
            # Unknown key: reported, nothing queued.
            dev.key("tap nosuchkey")
            self.assertIsNotNone(dev.wait_for(lambda l: "unknown key" in l, 3.0))
            # The screen: one full frame, the UI's palette and pixels.
            self.assertTrue(dev.shot("t"))
            f = dev.frame
            self.assertEqual((f["w"], f["h"]), (240, 135))
            self.assertGreater(len(set(f["px"])), 3)            # something is drawn
            png = os.path.join(self.tmp, "s.png")
            pg_daisy.write_png(png, f, 2)
            with open(png, "rb") as fh:
                self.assertEqual(fh.read(8), b"\x89PNG\r\n\x1a\n")
            # A desktop key script, with a shot in it.
            script = os.path.join(self.tmp, "k.txt")
            with open(script, "w") as fh:
                fh.write("# test\nbattery 50\ntap 2\nwait 100\nshot after\ntap 1\n")
            pg_daisy.run_script(dev, script, os.path.join(self.tmp, "shots"), 1)
            self.assertTrue(os.path.exists(os.path.join(self.tmp, "shots", "after.png")))
            # Mirror: changed rows stream after a key.
            dev.send("m")
            dev.pump(0.3)
            before = dev.frames
            dev.key("tap 3")
            dev.pump(0.6)
            self.assertGreater(dev.frames, before)
            dev.send("m")
        finally:
            dev.close()

    def test_transfer_and_restart(self):
        flash = os.path.join(self.tmp, "qspi.bin")
        s = self.sim("--flash", flash)
        with pg.Link(s.port, timeout=0.4) as l:
            info = pg.hello(l)
            self.assertEqual(info["firmware"], "sim")
            ents, sd, _ = pg.list_projects(l)
            self.assertFalse(sd)                               # stage 0: no card
            # A project: the device's autosaved session (the UI writes it a few seconds
            # after boot), pushed back under a name and pulled again.
            self.assertEqual([e for e in ents if e["mount"] == "int" and e["name"] != "_last"], [])
            for _ in range(40):
                if any(e["name"] == "_last" for e in pg.list_projects(l)[0]):
                    break
                time.sleep(0.25)
            a = pg.get_file(l, "int", "_last")
            self.assertTrue(a.startswith(b"PG"))
            self.assertEqual(pg.put_file(l, "int", "TESTA", a), ["int", "TESTA"])
            b = pg.get_file(l, "int", "TESTA")
            self.assertEqual(pg.with_stem(b, pg.pgv_header(a)["stem"]), a)
            # A sample into the pool (RAM, then its flash copy).
            tone = [int(8000 * ((i % 50) - 25) / 25) for i in range(9000)]
            self.assertEqual(pg.put_sample(l, 4, tone, "SAW"), ["4", "SAW", "9000"])
            self.assertEqual(pg.get_sample(l, 4), (tone, "SAW"))
        # Wait until /int and the pool are on the (simulated) flash, then restart.
        dev = pg_daisy.Device(s.port)
        try:
            dev.send("S")
            ok = False
            for _ in range(40):
                text = "\n".join(self.report(dev))
                if "on flash" in text and "0 sectors to write" in text:
                    ok = True
                    break
                time.sleep(0.25)
            self.assertTrue(ok, "the flash copies never caught up")
        finally:
            dev.close()
        time.sleep(2.5)   # the sim writes its flash file every 2 s
        s.stop()
        s2 = self.sim("--flash", flash)
        with pg.Link(s2.port, timeout=0.4) as l:
            names = [e["name"] for e in pg.list_projects(l)[0]]
            self.assertIn("TESTA", names)
            self.assertEqual(pg.get_sample(l, 4), (tone, "SAW"))


    def test_line_in_recording_and_touch(self):
        """Stage 1's recording path in daisy_sim (a 440 Hz tone on the line input's L,
        R silent): the UI's gesture over serial (PERC's sampler page, hold L), the slot
        at 48 kHz, kept over a restart; a touch from the computer reaches the mirror."""
        flash = os.path.join(self.tmp, "qspi.bin")
        s = self.sim("--flash", flash, "--line-in", "mono")
        dev = pg_daisy.Device(s.port)
        try:
            text = "\n".join(self.report(dev))
            self.assertIn("[linein] codec line in, 48000 Hz in and stored", text)
            self.assertIn("no recording yet", text)
            # PERC selected silently, its SOUND page, two steps to a sampler instrument
            # (host/daisy/test_daisy_stage1.cpp counts them on the same demo project)
            for k in ("down sft", "tap h", "up sft", "tap 9"):
                dev.key(k)
            dev.pump(0.3)
            for _ in range(2):
                dev.key("tap .")
                dev.pump(0.5)
            dev.key("down l")
            self.assertTrue(dev.sync("hold"))
            dev.pump(2.0)          # the room, then the tone's next onset (0.8 s on, 0.7 s off)
            dev.key("up l")
            self.assertTrue(dev.sync("rel"))
            dev.pump(0.5)
            rep = "\n".join(self.report(dev))
            self.assertIn("[linein] last: slot 1 DONE (commit OK)", rep)
            self.assertIn("at 48000 Hz", rep)
            self.assertIn("mono L alone (R silent)", rep)
            self.assertRegex(rep, r"\[samples\] slot  1 REC01")
            # a touch from the computer, with the mirror on: down, then up after 60 ms
            dev.send("m")
            dev.pump(0.3)
            dev.key("touch 12 34 60")
            dev.pump(0.4)
            self.assertEqual(dev.touch, (False, 12, 34))
            self.assertEqual(dev.touches, 2)
            dev.send("m")
            # the pool's flash copy, then a restart: the recording is still there
            ok = False
            for _ in range(40):
                if "0 sectors to write" in "\n".join(self.report(dev)):
                    ok = True
                    break
                time.sleep(0.25)
            self.assertTrue(ok)
        finally:
            dev.close()
        time.sleep(2.5)
        s.stop()
        s2 = self.sim("--flash", flash, "--line-in", "mono")
        with pg.Link(s2.port, timeout=0.4) as l:
            data, name = pg.get_sample(l, 1)
            self.assertEqual(name, "REC01")
            self.assertGreater(len(data), 48000 // 2)


class BenchTest(unittest.TestCase):
    """tools/pg-daisy bench against daisy_sim (the firmware's App: the bench runs in its
    simulated interrupt, the host's clock as cycles), the table it prints, the restart
    afterwards keeping the session; the pots' setting; compare; the mirror's key filter."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="pgbench")
        self.flash = os.path.join(self.tmp, "qspi.bin")
        self.s = Sim("--autoplay", "--flash", self.flash)

    def tearDown(self):
        if self.s.proc.poll() is None:
            self.s.stop()
        shutil.rmtree(self.tmp, ignore_errors=True)

    def report(self, dev):
        dev.send("r")
        out = []
        for line in dev.lines(5.0):
            if "REPORT BEGIN" in line:
                out = [line]
            elif out:
                out.append(line)
                if "REPORT END" in line:
                    return "\n".join(out)
        self.fail("no report")

    def test_bench_quick(self):
        dev = pg_daisy.Device(self.s.port)
        try:
            dev.pump(1.0)
            rep = self.report(dev)
            self.assertIn("[audio] late blocks:", rep)   # (the sim runs on the host's clock: it may have some)
            self.assertIn("after the warm-up (the first 32 blocks", rep)
        finally:
            dev.close()
        out_file = os.path.join(self.tmp, "bench.txt")
        p = subprocess.run([sys.executable, os.path.join(HERE, "pg_daisy.py"), "--port", self.s.port, "--file",
                            out_file, "bench", "quick"], capture_output=True, text=True, timeout=120)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        with open(out_file) as fh:
            lines = fh.read().splitlines()
        r = pg_daisy.parse_bench(lines)
        self.assertTrue(r["complete"])
        self.assertEqual(r["order"], ["idle", "ACIDJAM", "BASSHAUS", "WAREHOUS", "busy"])
        for s in r["order"]:
            row = r["rows"][s]
            self.assertGreater(row["avg"], 0)
            self.assertLessEqual(row["min"], row["avg"])
            self.assertGreaterEqual(row["max"], row["avg"])
            self.assertIsNotNone(row["split"])
            self.assertIsNotNone(row["host"], "daisy/app/bench_ref.h has no count for %s" % s)
        self.assertGreater(r["rows"]["busy"]["host"], r["rows"]["ACIDJAM"]["host"])
        self.assertIn("QUICK", "\n".join(lines))
        self.assertIn("[bench] live before the bench", "\n".join(lines))
        # The board restarts at the end (the sim boots again): the session came back,
        # and it plays (stage 0's autoplay) its own project, not the bench's last scene.
        time.sleep(2.5)
        dev = pg_daisy.Device(self.s.port)
        try:
            dev.pump(0.5)
            rep = self.report(dev)
            self.assertIn("playing yes", rep)
            self.assertIn('project "ACIDJAM"', rep)
            # compare: two runs (the same one twice, the second relabelled) in one table
            r2 = pg_daisy.parse_bench(lines)
            for row in r2["rows"].values():
                row["avg"] = row["avg"] * 2
            table = pg_daisy.compare([("base", r), ("itcm", r2)])
            self.assertIn("busy", table)
            self.assertIn("(+100%)", table)
            # the pots: off by default, switched on, kept over a restart
            self.assertIn("setting off", rep)
            out = dev.ask("pots on")
            self.assertTrue(any("setting on" in l for l in out), out)
            self.assertTrue(any("? " in l for l in dev.ask("pots maybe")))
        finally:
            dev.close()
        for _ in range(40):
            time.sleep(0.25)
            d = pg_daisy.Device(self.s.port)
            try:
                if "on flash" in self.report(d):
                    break
            finally:
                d.close()
        time.sleep(2.5)
        self.s.stop()
        self.s = Sim("--flash", self.flash)
        dev = pg_daisy.Device(self.s.port)
        try:
            self.assertIn("setting on", self.report(dev))
        finally:
            dev.close()

    def test_clock_wrap(self):
        """daisy_sim with its clocks about to wrap (--clock-wrap-in): the report's task
        times stay sane across the wrap of micros() (2^32 us) and millis()."""
        self.s.stop()
        self.s = Sim("--autoplay", "--clock-wrap-in", "1")
        dev = pg_daisy.Device(self.s.port)
        try:
            dev.pump(3.0)
            rep = self.report(dev)
            self.assertNotIn("4294", rep)
            m = re.search(r"longest pass (\d+)us", rep)
            self.assertIsNotNone(m)
            self.assertLess(int(m.group(1)), 1000000)
        finally:
            dev.close()

    def test_key_filter(self):
        """The mirror window's keys: auto-repeat (presses while held, or release + press
        pairs) is one down and one up."""
        sent, timers = [], {}

        def later(ms, fn):
            tid = len(timers) + 1
            timers[tid] = fn
            return tid

        def cancel(tid):
            timers.pop(tid, None)

        def fire():
            for tid in list(timers):
                timers.pop(tid)()

        f = pg_daisy.KeyFilter(sent.append, later, cancel)
        f.press("q")
        for _ in range(10):        # macOS: repeated presses
            f.press("q")
        for _ in range(10):        # X11: release + press at once
            f.release("q")
            f.press("q")
        self.assertEqual(sent, ["down q"])
        f.release("q")
        fire()                     # no press followed: the release goes out
        self.assertEqual(sent, ["down q", "up q"])
        self.assertEqual(f.repeats, 20)
        f.press("w")
        f.release("w")
        f.release_all()
        self.assertEqual(sent[-2:], ["down w", "up w"])


class PadTest(unittest.TestCase):
    """Pads without sensors: ">hit" (tools/pg-daisy pad) through App::padEvent into the
    core's pad API, on PERC set to TAP (`vel HIT 0.5`, `press PRESS fast`)."""

    def setUp(self):
        self.s = Sim()
        self.dev = pg_daisy.Device(self.s.port)

    def tearDown(self):
        self.dev.close()
        self.s.stop()

    def peak(self):
        """The output's peak since the last report (the report's [audio] window line)."""
        self.dev.send("r")
        for line in self.dev.lines(5.0):
            m = re.search(r"output peak (\d+)", line)
            if m:
                self.dev.pump(0.2)   # the rest of the report
                return int(m.group(1))
        self.fail("no output peak in the report")

    def test_pads_from_the_computer(self):
        dev = self.dev
        p = dev.pads()
        self.assertEqual(p["job"], "drums")
        self.assertEqual([p[i]["track"] for i in (1, 2, 3, 4)], ["KICK", "SNAR", "CHH", "PERC"])
        # PERC selected silently, its SOUND page, its instrument stepped to TAP
        for k in ("down sft", "tap h", "up sft", "tap 9"):
            dev.key(k)
        self.assertTrue(dev.sync("perc"))
        for _ in range(10):
            if dev.pads()[4]["inst"] == "TAP":
                break
            dev.key("tap .")
            dev.pump(0.3)
        p = dev.pads()[4]
        self.assertEqual(p["inst"], "TAP")
        base = p["heard"]
        # velocity: a soft and a hard hit; TAP's vel line moves HIT (macro 1) for the note,
        # below its base under velocity 100, above it over; the hard one is louder
        heard, peaks = {}, {}
        for vel in (0.3, 1.0):
            dev.pump(0.6)                      # what rang before has died away
            self.peak()                        # the window starts now
            self.assertEqual(pg_daisy.hit(dev, 4, vel, 80), [])
            heard[vel] = dev.pads()[4]["heard"][0]
            dev.pump(0.4)
            peaks[vel] = self.peak()
        print("\n  TAP HIT base %d, heard at velocity 0.3: %d, at 1.0: %d; output peak %d / %d" % (
            base[0], heard[0.3], heard[1.0], peaks[0.3], peaks[1.0]))
        self.assertLess(heard[0.3], base[0])
        self.assertGreater(heard[1.0], base[0])
        self.assertGreater(peaks[1.0], peaks[0.3])
        self.assertGreater(peaks[0.3], 0)
        # pressure: a held pad squeezed moves PRESS (macro 2), the release brings it back
        pg_daisy.hit(dev, 4, 0.8, 0, press=1.0)
        dev.pump(0.1)
        held = dev.pads()[4]
        self.assertTrue(held["held"])
        self.assertEqual(held["press"], (1, 160))      # TAP: press PRESS fast, the full range
        self.assertGreater(held["heard"][1], base[1])
        dev.key("hit 4 press 0.3")
        dev.pump(0.1)
        softer = dev.pads()[4]
        self.assertTrue(0 < softer["press"][1] < 160)
        dev.key("hit 4 up")
        dev.pump(0.4)                                  # the glide back (120 ms)
        after = dev.pads()[4]
        self.assertFalse(after["held"])
        self.assertEqual(after["press"], (-1, 0))
        self.assertEqual(after["heard"][1], base[1])
        # the pads' job: the desktop's key, then the command; the CLI's pad
        dev.key("tap padjob")
        dev.pump(0.2)
        p = dev.pads()
        self.assertEqual(p["job"], "notes")
        self.assertEqual({p[i]["track"] for i in (1, 2, 3, 4)}, {"PERC"})   # the selected track
        self.assertEqual(len({p[i]["note"] for i in (1, 2, 3, 4)}), 4)      # up the scale
        dev.ask("pads job drums")
        self.assertEqual(dev.pads()["job"], "drums")
        dev.close()
        out = subprocess.run([sys.executable, os.path.join(HERE, "pg_daisy.py"), "--port", self.s.port, "pad", "1",
                              "0.786", "hold", "150"], capture_output=True, text=True, timeout=30).stdout
        self.dev = pg_daisy.Device(self.s.port)
        self.assertIn("held   pad 1 (job drums) plays KICK note 60", out)
        self.assertIn("HELD", out)
        self.assertIn("after  pad 1", out)
        self.assertIn("last hit pad 1 velocity 0.79 = 100", "\n".join(self.dev.ask("pads")))
        # nonsense is refused, nothing plays
        self.assertTrue(any(l.startswith("[pads] ?") for l in self.dev.ask("hit 17 0.5")))


def main():
    global SIM
    ap = argparse.ArgumentParser()
    ap.add_argument("--sim", default=os.path.join(HERE, "..", "build", "host", "daisy", "daisy_sim"))
    a, rest = ap.parse_known_args()
    SIM = os.path.abspath(a.sim)
    if not os.path.exists(SIM):
        print("test_pg_daisy: %s not built" % SIM)
        return 1
    return 0 if unittest.main(argv=[sys.argv[0]] + rest, exit=False).result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
