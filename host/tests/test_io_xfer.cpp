// pg::io::Xfer, the USB serial project-transfer protocol (pg/io/xfer.h), on an
// in-memory filesystem with a C++ copy of the host framing. The real Python
// client (tools/pg.py) is tested against the same module end to end by
// tools/test_pg.py through host/tools/xfer_sim (registered as test_pg_client).
//
// Covers: coexistence with single-letter commands, HELLO/LS/GET/PUT round
// trips, resends (lost replies), corrupted and overlong lines, a corrupted
// payload, a forged file, a wrong stem, truncated transfers and the session
// timeout, playing / force, the out-of-order NAK, DONE idempotence and the
// power-loss recovery of an interrupted install.
#include <string>
#include <vector>

#include "pg/io/xfer.h"
#include "tests/test_io_util.h"

using namespace pg;
using namespace pg::io;

namespace {

struct Out final : XferOut {
  std::vector<std::string> lines;
  void send(const char* line, size_t len) override { lines.emplace_back(line, len); }
};

struct Rig {
  pgtest::MemFs fs;
  Xfer x;
  Out out;
  XferEnv env;
  int nextId = 1;
  Rig() { x.init(&fs, "test"); }

  static std::string frame(const std::string& body) {
    char crc[16];
    std::snprintf(crc, sizeof(crc), "%08lX", static_cast<unsigned long>(crc32(0, reinterpret_cast<const uint8_t*>(body.data()), body.size())));
    return "~PGX " + body + " " + crc + "\n";
  }
  // Feeds raw bytes; returns how many were NOT consumed by the protocol.
  int feed(const std::string& bytes) {
    int notOurs = 0;
    for (char c : bytes) {
      if (!x.rx(uint8_t(c), env.nowMs)) ++notOurs;
      if (x.lineReady()) x.service(env, out);
    }
    return notOurs;
  }
  // Sends one request, returns the reply bodies (CRC checked, prefix and id stripped).
  std::vector<std::string> req(const std::string& cmd, int id = -1) {
    if (id < 0) id = nextId++;
    out.lines.clear();
    feed(frame(std::to_string(id) + " " + cmd));
    std::vector<std::string> r;
    for (const std::string& l : out.lines) {
      CHECK(l.size() > 16 && l.compare(0, 5, "~PGX ") == 0 && l.back() == '\n');
      const std::string body = l.substr(5, l.size() - 5 - 10);
      const std::string crc = l.substr(l.size() - 9, 8);
      char want[16];
      std::snprintf(want, sizeof(want), "%08lX", static_cast<unsigned long>(crc32(0, reinterpret_cast<const uint8_t*>(body.data()), body.size())));
      CHECK(crc == want);
      const std::string idp = std::to_string(id) + " ";
      CHECK(body.compare(0, idp.size(), idp) == 0);
      r.push_back(body.substr(idp.size()));
    }
    return r;
  }
  std::string one(const std::string& cmd, int id = -1) {
    const std::vector<std::string> r = req(cmd, id);
    CHECK(r.size() == 1);
    return r.empty() ? std::string() : r[0];
  }
};

std::vector<uint8_t> pgvBytes(const char* stem, uint32_t seed) {
  auto p = iot::newProject();
  iot::initProject(*p);
  iot::Rng r(seed);
  for (int s = 0; s < 16; s += 2) p->patterns[0].tracks[s % 8].steps[s] = iot::musicalStep(r, s % 8);
  p->globals[kGTempo] = uint8_t(100 + seed % 40);
  for (int pt = 0; pt < 4; ++pt)   // enough content for files of a few KB
    for (int t = 0; t < kNumTracks; ++t)
      for (int s = 0; s < 64; s += 1 + int(r.below(3))) p->patterns[pt].tracks[t].steps[s] = iot::musicalStep(r, t);
  std::vector<uint8_t> buf(80 * 1024);
  size_t len = 0;
  CHECK_ST(encodeToBuffer(*p, stem, buf.data(), buf.size(), &len), Status::Ok);
  buf.resize(len);
  return buf;
}

std::string hex8(uint32_t v) {
  char b[16];
  std::snprintf(b, sizeof(b), "%08lX", static_cast<unsigned long>(v));
  return b;
}
uint32_t crcOf(const std::vector<uint8_t>& d) { return crc32(0, d.data(), d.size()); }
std::string b64(const uint8_t* p, size_t n) {
  std::string s((n + 2) / 3 * 4 + 1, '\0');
  base64Encode(p, n, &s[0], s.size());
  s.resize(std::strlen(s.c_str()));
  return s;
}

// Full GET through the protocol; returns the bytes.
std::vector<uint8_t> get(Rig& g, const std::string& mount, const std::string& name) {
  const std::string f = g.one("GET " + mount + " " + name);
  unsigned long size = 0, crc = 0;
  if (std::sscanf(f.c_str(), "FILE %lu %lx", &size, &crc) != 2) { CHECK(false); return {}; }
  std::vector<uint8_t> data;
  while (data.size() < size) {
    const std::string d = g.one("RD " + std::to_string(data.size()));
    unsigned long off = 0;
    char payload[256] = {};
    CHECK(std::sscanf(d.c_str(), "DATA %lu %255s", &off, payload) == 2 && off == data.size());
    uint8_t buf[Xfer::kChunk];
    const int n = base64Decode(payload, buf, sizeof(buf));
    CHECK(n > 0);
    if (n <= 0) return {};
    data.insert(data.end(), buf, buf + n);
  }
  CHECK(crc32(0, data.data(), data.size()) == crc);
  CHECK(g.one("END") == "OK");
  return data;
}

// Full PUT; returns the DONE reply. stopAt: stop sending after this many bytes (truncated transfer).
std::string put(Rig& g, const std::string& mount, const std::string& name, const std::vector<uint8_t>& d,
                bool force = false, size_t stopAt = SIZE_MAX, uint32_t claimCrc = 0, bool useClaim = false) {
  const std::string r = g.one("PUT " + mount + " " + name + " " + std::to_string(d.size()) + " " +
                              hex8(useClaim ? claimCrc : crcOf(d)) + (force ? " 1" : " 0"));
  if (r.compare(0, 5, "READY") != 0) return r;
  size_t off = 0;
  while (off < d.size()) {
    if (off >= stopAt) return "STOPPED";
    const size_t n = std::min<size_t>(Xfer::kChunk, d.size() - off);
    const std::string a = g.one("WR " + std::to_string(off) + " " + b64(d.data() + off, n));
    if (a != "ACK " + std::to_string(off + n)) return a;
    off += n;
  }
  return g.one("DONE");
}

void testCoexist() {
  Rig g;
  CHECK(g.feed("rh?m\n\r x") == 8);              // single-letter commands stay with the caller
  CHECK(g.out.lines.empty());
  const std::string hi = g.one("HELLO");
  CHECK(hi == "HI PGX 1 test 0");
  // A '~' that never ends a line is given up after kLineTimeoutMs; then letters are the caller's again.
  g.feed("~PGX 5 HEL");
  CHECK(g.feed("r") == 0);
  g.env.nowMs += Xfer::kLineTimeoutMs + 1;
  g.x.service(g.env, g.out);
  CHECK(g.feed("r") == 1);
  // Log-like lines interleaved with requests are not touched.
  CHECK(g.feed("[stats] t=5s cpu 30.1%\n") > 10);
  CHECK(g.one("HELLO") == "HI PGX 1 test 0");
}

void testListGet() {
  Rig g;
  const std::vector<uint8_t> a = pgvBytes("ALPHA", 1), b = pgvBytes("BETA", 2), s = pgvBytes("_last", 3);
  g.fs.files["/int/pg/ALPHA.pgv"] = a;
  g.fs.files["/sd/pg/BETA.pgv"] = b;
  g.fs.files["/int/pg/_last.pgv"] = s;
  g.fs.files["/int/pg/_ui.cfg"] = {1, 2, 3};
  const std::vector<std::string> ls = g.req("LS");
  CHECK(ls.size() == 4);
  if (ls.size() == 4) {
    CHECK(ls[0].compare(0, 15, "ENT int ALPHA " + std::to_string(a.size()).substr(0, 1)) == 0);
    CHECK(ls[1].compare(0, 12, "ENT sd BETA ") == 0);
    CHECK(ls[2].compare(0, 14, "ENT int _last ") == 0);
    CHECK(ls[3] == "END 3 1 0");
  }
  CHECK(get(g, "int", "ALPHA") == a);
  CHECK(get(g, "sd", "BETA") == b);
  CHECK(get(g, "int", "_last") == s);
  CHECK(g.one("GET int NOPE") == "ERR NOT_FOUND NOPE");
  CHECK(g.one("GET int ../x") == "ERR BAD_NAME ../x");
  CHECK(g.one("GET sd _last") == "ERR BAD_NAME _last");
  g.fs.sdPresent = false;
  CHECK(g.one("GET sd BETA") == "ERR NO_MOUNT sd");
  CHECK(g.req("LS").back() == "END 2 0 0");
  // GET is allowed while playing; RD resends and seeks back give the same bytes.
  g.env.playing = true;
  const std::string f = g.one("GET int ALPHA");
  CHECK(f.compare(0, 5, "FILE ") == 0);
  const std::string d0 = g.one("RD 0"), d0b = g.one("RD 0"), d1 = g.one("RD 128"), d0c = g.one("RD 0");
  CHECK(d0 == d0b && d0 == d0c && d1 != d0 && d1.compare(0, 9, "DATA 128 ") == 0);
  CHECK(g.one("RD 999999") == "ERR BAD_OFFSET");
  // An abandoned GET times out.
  g.env.nowMs += Xfer::kSessionTimeoutMs + 1;
  g.x.service(g.env, g.out);
  CHECK(!g.x.busy() && g.x.stats().timeouts == 1);
  CHECK(g.one("RD 128") == "ERR NO_SESSION");
}

void testPut() {
  Rig g;
  const std::vector<uint8_t> old = pgvBytes("SONG", 7), neu = pgvBytes("SONG", 8);
  g.fs.files["/int/pg/SONG.pgv"] = old;
  CHECK(put(g, "int", "SONG", neu) == "SAVED int SONG");
  CHECK(*g.fs.get("/int/pg/SONG.pgv") == neu);
  CHECK(!g.fs.has("/int/pg/_xfer.tmp"));
  CHECK(put(g, "sd", "NEWONE", pgvBytes("NEWONE", 9)) == "SAVED sd NEWONE");
  CHECK(g.fs.has("/sd/pg/NEWONE.pgv"));
  // DONE resent with the same id (its reply was lost): same answer, nothing redone.
  {
    const std::vector<uint8_t> d = pgvBytes("AGAIN", 10);
    g.one("PUT int AGAIN " + std::to_string(d.size()) + " " + hex8(crcOf(d)) + " 0");
    for (size_t off = 0; off < d.size(); off += Xfer::kChunk)
      g.one("WR " + std::to_string(off) + " " + b64(d.data() + off, std::min<size_t>(Xfer::kChunk, d.size() - off)));
    CHECK(g.one("DONE", 500) == "SAVED int AGAIN");
    CHECK(g.one("DONE", 500) == "SAVED int AGAIN");
    CHECK(g.one("DONE", 501) == "ERR NO_SESSION");
  }
  // Lost ACK: the chunk is resent at the same offset and acknowledged without being written twice.
  {
    const std::vector<uint8_t> d = pgvBytes("RESEND", 11);
    g.one("PUT int RESEND " + std::to_string(d.size()) + " " + hex8(crcOf(d)) + " 0");
    const std::string c0 = b64(d.data(), Xfer::kChunk);
    CHECK(g.one("WR 0 " + c0) == "ACK 128");
    CHECK(g.one("WR 0 " + c0) == "ACK 128");
    CHECK(g.one("WR 256 " + c0) == "NAK 128");   // out of order: told where to resume
    for (size_t off = Xfer::kChunk; off < d.size(); off += Xfer::kChunk)
      g.one("WR " + std::to_string(off) + " " + b64(d.data() + off, std::min<size_t>(Xfer::kChunk, d.size() - off)));
    CHECK(g.one("DONE") == "SAVED int RESEND");
    CHECK(*g.fs.get("/int/pg/RESEND.pgv") == d);
  }
}

void testPutRefusals() {
  Rig g;
  const std::vector<uint8_t> old = pgvBytes("SONG", 7), d = pgvBytes("SONG", 8);
  g.fs.files["/int/pg/SONG.pgv"] = old;
  g.env.playing = true;
  CHECK(put(g, "int", "SONG", d) == "ERR PLAYING");
  CHECK(put(g, "int", "SONG", d, true) == "SAVED int SONG");      // forced
  g.fs.files["/int/pg/SONG.pgv"] = old;
  // Playback starts in the middle of a put: aborted, temp gone, old file intact.
  g.env.playing = false;
  CHECK(put(g, "int", "SONG", d, false, 256) == "STOPPED");
  g.env.playing = true;
  CHECK(g.one("WR 256 " + b64(d.data() + 256, Xfer::kChunk)) == "ERR PLAYING");
  g.env.playing = false;
  CHECK(*g.fs.get("/int/pg/SONG.pgv") == old && !g.fs.has("/int/pg/_xfer.tmp"));
  // A microphone recording streams into the sample pool: no flash write, forced or not.
  g.env.recording = true;
  CHECK(put(g, "int", "SONG", d) == "ERR RECORDING");
  CHECK(put(g, "int", "SONG", d, true) == "ERR RECORDING");
  g.env.recording = false;
  CHECK(put(g, "int", "SONG", d, false, 256) == "STOPPED");
  g.env.recording = true;
  CHECK(g.one("WR 256 " + b64(d.data() + 256, Xfer::kChunk)) == "ERR RECORDING");
  g.env.recording = false;
  CHECK(*g.fs.get("/int/pg/SONG.pgv") == old && !g.fs.has("/int/pg/_xfer.tmp"));
  // Names, sizes, mounts.
  CHECK(put(g, "int", "_last", d) == "ERR BAD_NAME _last");
  CHECK(put(g, "int", "lower", d) == "ERR BAD_NAME lower");
  CHECK(put(g, "xx", "SONG", d) == "ERR BAD_NAME xx");
  CHECK(g.one("PUT int SONG 10 00000000 0") == "ERR BAD_SIZE");
  CHECK(g.one("PUT int SONG 999999 00000000 0") == "ERR BAD_SIZE");
  g.fs.sdPresent = false;
  CHECK(put(g, "sd", "SONG", d) == "ERR NO_MOUNT sd");
  g.fs.spaceLimit[0] = long(old.size()) + 100;   // 100 bytes free
  CHECK(put(g, "int", "SONG", d) == "ERR NO_SPACE");
  g.fs.spaceLimit[0] = -1;
  // Truncated transfer: abandoned half way, the session times out, nothing installed.
  CHECK(put(g, "int", "HALF", pgvBytes("HALF", 3), false, 512) == "STOPPED");
  CHECK(g.x.busy());
  g.env.nowMs += Xfer::kSessionTimeoutMs + 1;
  g.x.service(g.env, g.out);
  CHECK(!g.x.busy() && !g.fs.has("/int/pg/_xfer.tmp") && !g.fs.has("/int/pg/HALF.pgv"));
  // DONE before all bytes arrived.
  CHECK(put(g, "int", "HALF", pgvBytes("HALF", 3), false, 512) == "STOPPED");
  CHECK(g.one("DONE") == "ERR SHORT");
  CHECK(!g.fs.has("/int/pg/HALF.pgv") && !g.fs.has("/int/pg/_xfer.tmp"));
}

void testCorruption() {
  Rig g;
  const std::vector<uint8_t> old = pgvBytes("SONG", 7);
  g.fs.files["/int/pg/SONG.pgv"] = old;
  // A corrupted line (one byte changed after framing) gets no reply at all.
  std::string l = Rig::frame("77 HELLO");
  l[7] = 'X';
  g.out.lines.clear();
  g.feed(l);
  CHECK(g.out.lines.empty() && g.x.stats().badCrc == 1);
  // A dropped byte inside a line: no reply either.
  l = Rig::frame("78 HELLO");
  l.erase(9, 1);
  g.feed(l);
  CHECK(g.out.lines.empty());
  // Overlong line: dropped, the next one works.
  g.feed("~" + std::string(400, 'A') + "\n");
  CHECK(g.out.lines.empty());
  CHECK(g.one("HELLO") == "HI PGX 1 test 0");
  // Payload that does not match the announced CRC (sent with valid line CRCs).
  std::vector<uint8_t> d = pgvBytes("SONG", 8);
  CHECK(put(g, "int", "SONG", d, false, SIZE_MAX, crcOf(d) ^ 1u, true) == "ERR CRC");
  CHECK(*g.fs.get("/int/pg/SONG.pgv") == old && !g.fs.has("/int/pg/_xfer.tmp"));
  // A file that is not a valid .pgv (consistent CRC, broken content): refused by the install check.
  std::vector<uint8_t> bad = d;
  bad[bad.size() / 2] ^= 0x55;
  const std::string r = put(g, "int", "SONG", bad);
  CHECK(r.compare(0, 4, "ERR ") == 0);
  CHECK(*g.fs.get("/int/pg/SONG.pgv") == old && !g.fs.has("/int/pg/_xfer.tmp"));
  // Truncated file content: refused.
  std::vector<uint8_t> shortF(d.begin(), d.begin() + long(d.size() - 20));
  CHECK(put(g, "int", "SONG", shortF).compare(0, 4, "ERR ") == 0);
  // Header stem must match the target name (what power-loss recovery would use).
  CHECK(put(g, "int", "OTHER", d) == "ERR BAD_NAME");
  CHECK(!g.fs.has("/int/pg/OTHER.pgv"));
  // Bad base64 aborts the session.
  g.one("PUT int SONG " + std::to_string(d.size()) + " " + hex8(crcOf(d)) + " 0");
  CHECK(g.one("WR 0 !!!!") == "ERR BAD_DATA");
  CHECK(!g.x.busy() && *g.fs.get("/int/pg/SONG.pgv") == old);
}

void testRecovery() {
  // An install interrupted after a FAT-style rename removed the target: the
  // verified _xfer.tmp is the only copy and the boot recovery puts it in place.
  pgtest::MemFs fs;
  const std::vector<uint8_t> d = pgvBytes("SAVED", 4);
  fs.files["/sd/pg/_xfer.tmp"] = d;
  CHECK_ST(recoverAfterPowerLoss(fs, kMountSd), Status::Ok);
  CHECK(fs.has("/sd/pg/SAVED.pgv") && !fs.has("/sd/pg/_xfer.tmp"));
  // A partial temp next to an intact target is deleted.
  fs.files["/sd/pg/_xfer.tmp"] = std::vector<uint8_t>(d.begin(), d.begin() + 100);
  CHECK_ST(recoverAfterPowerLoss(fs, kMountSd), Status::Ok);
  CHECK(!fs.has("/sd/pg/_xfer.tmp") && *fs.get("/sd/pg/SAVED.pgv") == d);
  // Non-atomic rename during an install: the target is missing afterwards only
  // if the temp stays for recovery.
  Rig g;
  g.fs.renameAtomic = false;
  g.fs.files["/int/pg/SONG.pgv"] = pgvBytes("SONG", 1);
  const std::vector<uint8_t> n = pgvBytes("SONG", 2);
  CHECK(put(g, "int", "SONG", n) == "SAVED int SONG");
  CHECK(*g.fs.get("/int/pg/SONG.pgv") == n);
}

void testBase64() {
  iot::Rng r(5);
  for (int len = 0; len <= 130; ++len) {
    std::vector<uint8_t> d(static_cast<size_t>(len));
    for (auto& b : d) b = r.byte();
    char enc[200];
    base64Encode(d.data(), d.size(), enc, sizeof(enc));
    uint8_t dec[140];
    CHECK(base64Decode(enc, dec, sizeof(dec)) == len && std::memcmp(dec, d.data(), d.size()) == 0);
  }
  uint8_t o[8];
  CHECK(base64Decode("QUJD", o, 8) == 3 && o[0] == 'A');
  CHECK(base64Decode("QUI=", o, 8) == 2);
  CHECK(base64Decode("QQ==", o, 8) == 1);
  CHECK(base64Decode("QQ=A", o, 8) == -1);
  CHECK(base64Decode("QUJ", o, 8) == -1);
  CHECK(base64Decode("QU*D", o, 8) == -1);
  CHECK(base64Decode("QUJDQUJD", o, 3) == -1);
  // Host request lines stay inside the device's 256-byte USB receive queue.
  CHECK(Rig::frame("65535 WR 999999 " + std::string((Xfer::kChunk + 2) / 3 * 4, 'A')).size() <= size_t(Xfer::kMaxHostLine));
}

}  // namespace

// Project versions through the transfer: a version-2 file (the step layout before
// the repack; the engine's sanitiser repacks it after a load, itest_format) and a
// version-3 file go in and come out byte for byte, each with its own version.
void testVersions() {
  Rig rig;
  for (int v : {2, 3}) {
    auto p = iot::newProject();
    iot::typicalProject(*p, uint32_t(40 + v));
    p->version = uint8_t(v);
    std::vector<uint8_t> d(80 * 1024);
    size_t len = 0;
    CHECK_ST(encodeToBuffer(*p, "VERS", d.data(), d.size(), &len), Status::Ok);
    d.resize(len);
    CHECK(put(rig, "int", "VERS", d) == "SAVED int VERS");
    CHECK(get(rig, "int", "VERS") == d);
    auto q = iot::newProject();
    HeaderInfo hi{};
    CHECK_ST(loadProject(rig.fs, kMountInt, "VERS", *q, &hi), Status::Ok);
    CHECK(hi.projectVersion == v && std::memcmp(p.get(), q.get(), sizeof(Project)) == 0);
  }
}

int main() {
  testCoexist();
  testVersions();
  testListGet();
  testPut();
  testPutRefusals();
  testCorruption();
  testRecovery();
  testBase64();
  return iot::finish("test_io_xfer");
}
