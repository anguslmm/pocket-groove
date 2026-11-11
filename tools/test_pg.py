"""End-to-end test of the USB project transfer: the real client (tools/pg.py)
against the device's protocol module (pg::io::Xfer) running in
host/tools/xfer_sim on a pseudo-terminal, with log lines interleaved and
faults injected. Registered with ctest as test_pg_client.

    python3 tools/test_pg.py --sim build/host/xfer_sim

Exit code 77 (ctest: skipped) when pyserial is not installed.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
try:
    import serial  # noqa: F401
except ImportError:
    print("test_pg: pyserial not installed, skipping")
    sys.exit(77)
import pg  # noqa: E402

SIM = None


class Sim:
    def __init__(self, *opts, demos=True):
        self.root = tempfile.mkdtemp(prefix="pgsim")
        args = [SIM, "--root", self.root] + (["--demos"] if demos else []) + list(opts)
        self.proc = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.port = self.proc.stdout.readline().decode().strip()
        if not self.port.startswith("/dev/"):
            raise RuntimeError("xfer_sim did not start: %s" % self.proc.stderr.read().decode())

    def path(self, mount, name):
        return os.path.join(self.root, mount, "pg", name)

    def read(self, mount, name):
        with open(self.path(mount, name), "rb") as f:
            return f.read()

    def close(self):
        self.proc.kill()
        self.proc.wait()
        self.proc.stdout.close()
        self.proc.stderr.close()
        shutil.rmtree(self.root, ignore_errors=True)


class Args:
    def __init__(self, port, **kw):
        self.port = port
        self.timeout = 0.3
        self.verbose = False
        self.sd = False
        self.int = False
        self.force = False
        self.name = None
        self.out = None
        self.__dict__.update(kw)


class TransferTest(unittest.TestCase):
    def setUp(self):
        self.sims = []
        self.tmp = tempfile.mkdtemp(prefix="pgtest")

    def tearDown(self):
        for s in self.sims:
            s.close()
        shutil.rmtree(self.tmp, ignore_errors=True)

    def sim(self, *opts, **kw):
        s = Sim(*opts, **kw)
        self.sims.append(s)
        return s

    def link(self, s, timeout=0.3):
        return pg.Link(s.port, timeout=timeout)

    def test_list_pull_push(self):
        s = self.sim("--log-ms", "3")    # log lines in between everything
        with self.link(s) as l:
            info = pg.hello(l)
            self.assertEqual(info["firmware"], "sim-1.0")
            ents, sd, trunc = pg.list_projects(l)
            names = [(e["mount"], e["name"], e["bpm"]) for e in ents]
            self.assertIn(("int", "ACIDJAM", 124), names)
            self.assertIn(("sd", "BASSHAUS", 126), names)
            self.assertIn(("int", "_last", 133), names)
            self.assertTrue(sd)
            self.assertFalse(trunc)
            a = pg.get_file(l, "int", "ACIDJAM")
            self.assertEqual(a, s.read("int", "ACIDJAM.pgv"))
            sess = pg.get_file(l, "int", "_last")
            self.assertEqual(sess, s.read("int", "_last.pgv"))
            # push under a new name: the header stem is rewritten to it, everything else identical
            self.assertEqual(pg.put_file(l, "sd", "NEWNAME", a), ["sd", "NEWNAME"])
            b = s.read("sd", "NEWNAME.pgv")
            self.assertEqual(pg.pgv_header(b)["stem"], "NEWNAME")
            self.assertEqual(b[64:], a[64:])
            self.assertEqual(pg.with_stem(b, "ACIDJAM"), a)
            # replace an existing project
            self.assertEqual(pg.put_file(l, "int", "ACIDJAM", pg.get_file(l, "sd", "BASSHAUS")), ["int", "ACIDJAM"])
            self.assertEqual(pg.pgv_header(s.read("int", "ACIDJAM.pgv"))["tempo"], 126)
            self.assertFalse(os.path.exists(s.path("int", "_xfer.tmp")))

    def test_cli(self):
        s = self.sim()
        out = os.path.join(self.tmp, "x", "A.pgv")
        self.assertEqual(pg.main(["--port", s.port, "pull", "acidjam", "--out", out]), 0)
        with open(out, "rb") as f:
            self.assertEqual(f.read(), s.read("int", "ACIDJAM.pgv"))
        self.assertEqual(pg.main(["--port", s.port, "push", out, "--name", "pushed"]), 0)
        self.assertTrue(os.path.exists(s.path("int", "PUSHED.pgv")))
        self.assertEqual(pg.main(["--port", s.port, "pull", "NOPE"]), 1)
        self.assertEqual(pg.main(["--port", s.port, "ls"]), 0)
        bad = os.path.join(self.tmp, "bad.pgv")
        with open(bad, "wb") as f:
            f.write(b"not a project")
        self.assertEqual(pg.main(["--port", s.port, "push", bad]), 1)

    def test_faults(self):
        # 10% of the lines in each direction flipped, cut, shortened or lost.
        s = self.sim("--fault", "0.1", "--seed", "7", "--log-ms", "5")
        with self.link(s, timeout=0.15) as l:
            l.retries = 30
            for _ in range(3):
                a = pg.get_file(l, "int", "ACIDJAM")
                self.assertEqual(a, s.read("int", "ACIDJAM.pgv"))
                pg.put_file(l, "int", "FAULTY", a)
                self.assertEqual(s.read("int", "FAULTY.pgv")[64:], a[64:])
                ents, _, _ = pg.list_projects(l)
                self.assertIn("FAULTY", [e["name"] for e in ents])
            self.assertGreater(l.retried, 0)

    def test_playing(self):
        s = self.sim("--playing")
        with self.link(s) as l:
            self.assertTrue(pg.hello(l)["playing"])
            a = pg.get_file(l, "int", "ACIDJAM")          # reading is allowed while playing
            with self.assertRaises(pg.PgError) as e:
                pg.put_file(l, "int", "X", a)
            self.assertIn("playing", str(e.exception))
            self.assertFalse(os.path.exists(s.path("int", "X.pgv")))
            pg.put_file(l, "int", "X", a, force=True)
            self.assertTrue(os.path.exists(s.path("int", "X.pgv")))

    def test_truncated_transfer(self):
        # The device stops answering in the middle of a push: the client gives
        # up with an error (no hang), the device drops the half file after its
        # 5 s session timeout, and the old project is untouched.
        s = self.sim("--stall-after", "8", "--stall-ms", "1500")
        old = s.read("int", "ACIDJAM.pgv")
        new = pg.with_stem(s.read("sd", "BASSHAUS.pgv"), "ACIDJAM")
        with self.link(s, timeout=0.1) as l:
            l.retries = 3
            t0 = time.monotonic()
            with self.assertRaises(pg.PgError) as e:
                pg.put_file(l, "int", "ACIDJAM", new)
            self.assertIn("did not answer", str(e.exception))
            self.assertLess(time.monotonic() - t0, 5.0)
            self.assertTrue(os.path.exists(s.path("int", "_xfer.tmp")))   # half written
            time.sleep(6.5)
            self.assertFalse(os.path.exists(s.path("int", "_xfer.tmp")))  # timed out and removed
            self.assertEqual(s.read("int", "ACIDJAM.pgv"), old)
            # and the link works again afterwards
            pg.put_file(l, "int", "ACIDJAM", new)
            self.assertEqual(s.read("int", "ACIDJAM.pgv"), new)

    def test_device_gone(self):
        s = self.sim()
        l = self.link(s, timeout=0.1)
        l.retries = 2
        s.proc.kill()
        s.proc.wait()
        with self.assertRaises(pg.PgError):
            pg.hello(l)
        l.close()

    def test_framing(self):
        f = pg.frame("12 HELLO")
        self.assertEqual(f, b"~PGX 12 HELLO %08X\n" % (zlib.crc32(b"12 HELLO") & 0xFFFFFFFF))
        self.assertEqual(pg.parse_line(f), "12 HELLO")
        self.assertIsNone(pg.parse_line(f.replace(b"HELLO", b"HELLP")))
        self.assertIsNone(pg.parse_line(b"[stats] cpu 30%\n"))
        self.assertEqual(pg.parse_line(b"[log] interleaved " + f), "12 HELLO")
        self.assertEqual(pg.normalize_name("my-song1"), "MY-SONG1")
        with self.assertRaises(pg.PgError):
            pg.normalize_name("!!!")


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--sim", default=os.path.join(HERE, "..", "build", "host", "xfer_sim"))
    a, rest = ap.parse_known_args()
    SIM = os.path.abspath(a.sim)
    if not os.path.exists(SIM):
        print("test_pg: %s not built" % SIM)
        sys.exit(1)
    unittest.main(argv=[sys.argv[0]] + rest, verbosity=2)
