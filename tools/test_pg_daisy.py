#!/usr/bin/env python3
"""The computer side of the Daisy target against the firmware's own App code
(host/daisy/daisy_sim on a pseudo-terminal): tools/pg-daisy (keys, report, screen,
key scripts) and tools/pg (projects and samples over the same port), and /int and the
sample pool surviving a restart through the simulated QSPI flash.

    python3 tools/test_pg_daisy.py --sim build/host/daisy/daisy_sim

Exit 77 (skipped) without pyserial.
"""
import argparse
import os
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
