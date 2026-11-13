// Project transfer over a line-based serial link (the device's USB serial
// port), shared with log output and single-letter commands. Portable: the
// firmware feeds it bytes and gives it a way to send lines; the host tests
// drive it the same way (host/tests/test_io_xfer.cpp, host/tools/xfer_sim.cpp
// against the real Python client tools/pg.py).
//
// FRAMING. Every protocol line, both directions, is
//     ~PGX <body> <crc>\n
// where <crc> is the CRC32 (zlib) of <body> as 8 upper-case hex digits and
// <body> is space-separated ASCII tokens, the first being the request id
// (decimal, chosen by the host, echoed by the device). Lines that do not start
// with "~PGX " are not protocol (the device's log lines all start otherwise);
// a line with a wrong CRC is dropped without a reply, so a corrupted or
// truncated line can only cause a timeout and a retry, never wrong data.
// Host lines are at most kMaxHostLine bytes so one always fits the device's
// 256-byte USB receive queue; the host sends one line and waits for the reply.
//
// REQUESTS (host -> device) and replies:
//   id HELLO                         -> id HI PGX <protoVersion> <firmware> <playing 0/1>
//   id LS                            -> id ENT <int|sd> <NAME> <bytes> <bpm> <valid 0/1>  (0..n lines)
//                                       id END <n> <sd mounted 0/1> <truncated 0/1>
//                                       (the autosaved session is listed as int _last)
//   id GET <int|sd> <NAME|_last>     -> id FILE <bytes> <crc32 hex>      opens a read session
//   id RD <offset>                   -> id DATA <offset> <base64 of up to kChunk bytes>
//   id PUT <int|sd> <NAME> <bytes> <crc32 hex> <force 0/1>
//                                    -> id READY <chunk>                 opens a write session
//   id WR <offset> <base64>          -> id ACK <next offset> | id NAK <expected offset>
//   id DONE                          -> id SAVED <int|sd> <NAME>         verified and installed
//   id END                           -> id OK                            closes any session
//   any failure                      -> id ERR <CODE> [detail...]
// SAMPLE POOL (stage F1, pg/sampler/flash_pool.h; ERR UNKNOWN from a firmware without it,
// ERR NO_POOL from one whose flash has no pool partition). Slots are 1..12 here:
//   id SLS                           -> id SLOT <slot> <samples> <crc32 hex> <NAME|->  (0..12 lines)
//                                       id SEND <n>
//   id SPUT <slot> <samples> <crc32 hex of the audio bytes> <NAME|->
//                                    -> id READY <chunk>                 opens a write session: the
//                                       slot is empty from here until DONE (its chops stop)
//   id WR <offset> <base64>          -> as for PUT (the audio: mono int16 little-endian, 22,050 Hz)
//   id DONE                          -> id SSAVED <slot> <NAME|-> <samples>   verified against the CRC
//                                       through the flash driver AND as the CPU reads it
//   id SGET <slot>                   -> id SFILE <bytes> <crc32 hex> <NAME|->   then RD as for GET
//   id SRM <slot>                    -> id OK                            the slot is empty
// SPUT and SRM write flash: refused while the sequencer plays (ERR PLAYING, no force),
// and a running SPUT is aborted if playback starts (the slot stays empty). The device
// converts nothing: the host sends the slot's format (tools/pg_sample.py).
// GET and RD are read-only and allowed while the sequencer plays. PUT writes
// flash, which stalls the audio, so it is refused while playing (ERR PLAYING)
// unless force is 1, and a running put is aborted if playback starts. A put
// streams into <mount>/pg/_xfer.tmp and is installed with
// pg::io::installReceived (the save path's verify + rename); the old file is
// untouched until then. A session with no request for kSessionTimeoutMs is
// aborted (a put's temp file is deleted), so a half transfer never blocks
// anything. RD and WR are idempotent by offset, DONE by id, so the host may
// resend any request whose reply it did not get.
//
// MEMORY: one line buffer (kMaxHostLine) and two paths; replies are built on
// the caller's stack (< 300 bytes). No heap.
#pragma once
#include <cstddef>
#include <cstdint>

#include "pg/io/project_io.h"
#include "pg/platform.h"
#include "pg/sampler/flash_pool.h"

namespace pg::io {

class XferOut {
 public:
  virtual void send(const char* line, size_t len) = 0;   // one complete line, '\n' included
 protected:
  ~XferOut() = default;
};

struct XferEnv {
  uint32_t nowMs = 0;
  bool playing = false;
  // A microphone recording is streaming into the sample pool (stage F2): every request
  // that writes flash (PUT, its WRs, SPUT, SRM) is refused with ERR RECORDING, force or not.
  bool recording = false;
};

class Xfer {
 public:
  static constexpr int kProtoVersion = 1;
  static constexpr int kChunk = 128;               // payload bytes per RD / WR
  static constexpr int kMaxHostLine = 240;         // bytes incl. "\n"
  static constexpr uint32_t kSessionTimeoutMs = 5000;
  static constexpr uint32_t kLineTimeoutMs = 1000; // a started line must end within this
  static constexpr uint32_t kMaxPutBytes = uint32_t(kMaxHeaderSize) + (uint32_t(sizeof(Project)) + uint32_t(sizeof(Project)) / 128u + 16u);

  void init(FileSystem* fs, const char* firmwareVersion);
  // The sample pool's write side (nullptr: the S* requests answer ERR NO_POOL).
  void setSamples(sampler::SampleWriter* w) { samples_ = w; }
  // A sample put is running: its slot (0-based) and progress, for the screen.
  bool sampleProgress(int* slot, uint32_t* done, uint32_t* total) const;
  // Counts SSAVED / SRM successes (wraps): the screen's "saved" toast.
  uint32_t samplesChanged() const { return samplesChanged_; }

  // Feeds one received byte. Returns true if it belongs to a protocol line
  // (then the caller must not interpret it), false if the caller should
  // handle it (single-letter commands). A protocol line starts with '~' at
  // the start of a line and ends at '\n'.
  bool rx(uint8_t b, uint32_t nowMs);
  // A complete line is waiting for service().
  bool lineReady() const { return ready_; }
  // Handles the waiting line (if any) and the timeouts; replies through out.
  // Returns true if a line was handled.
  bool service(const XferEnv& env, XferOut& out);
  // A transfer session is open or a line is being received: the caller may
  // poll the port more often until it ends.
  bool busy() const { return mode_ != Mode::None || inLine_; }
  // Aborts any session (closes the file, deletes a put's temp).
  void abort();

  struct Stats {
    uint32_t lines, badCrc, badLines, overflows, timeouts, gets, puts, errors;
  };
  const Stats& stats() const { return stats_; }

 private:
  enum class Mode : uint8_t { None, Get, Put, SampleGet, SamplePut };
  void handle(char* body, const XferEnv& env, XferOut& out);
  void reply(XferOut& out, const char* fmt, ...) __attribute__((format(printf, 3, 4)));
  void err(XferOut& out, const char* code, const char* detail = nullptr);
  void doList(XferOut& out);
  void doGet(const char* mount, const char* name, XferOut& out);
  void doRead(uint32_t offset, XferOut& out);
  void doPut(const char* const* tok, int n, const XferEnv& env, XferOut& out);
  void doWrite(uint32_t offset, const char* b64, const XferEnv& env, XferOut& out);
  void doDone(XferOut& out);
  void doSampleList(XferOut& out);
  void doSamplePut(const char* const* tok, const XferEnv& env, XferOut& out);
  void doSampleGet(const char* slotTok, XferOut& out);
  void doSampleRemove(const char* slotTok, const XferEnv& env, XferOut& out);
  void doSampleDone(XferOut& out);
  bool reopenAt(uint32_t offset);
  void closeFile();

  FileSystem* fs_ = nullptr;
  const char* fw_ = "?";
  char line_[kMaxHostLine + 1] = {};
  int len_ = 0;
  bool inLine_ = false, ready_ = false, overflow_ = false, atLineStart_ = true;
  uint32_t lineStartMs_ = 0;
  char id_[8] = {};

  Mode mode_ = Mode::None;
  int h_ = -1;
  char mount_[4] = {};
  char name_[kNameBuf] = {};
  char path_[kPathMax] = {};
  uint32_t size_ = 0, crc_ = 0, pos_ = 0, runCrc_ = 0, lastMs_ = 0;
  bool force_ = false;
  sampler::SampleWriter* samples_ = nullptr;
  int slot_ = -1;                      // the sample session's slot (0-based)
  char sname_[sampler::kNameLen + 1] = {};
  uint32_t samplesChanged_ = 0;
  // DONE is idempotent: the last result, for a resent DONE with the same id.
  char doneId_[8] = {};
  char doneReply_[48] = {};
  Stats stats_ = {};
};

// Base64 (RFC 4648, with padding). encode writes 4*ceil(n/3) chars + NUL;
// decode returns the byte count or -1 on malformed input / overflow.
size_t base64Encode(const uint8_t* in, size_t n, char* out, size_t cap);
int base64Decode(const char* in, uint8_t* out, size_t cap);

}  // namespace pg::io
