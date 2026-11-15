// The tape's files (tape_io.h).
#include "tape_io.h"

#if PG_TAPE

#include <cstring>

namespace pg {
namespace tape {

namespace {

char* cat(char* o, const char* s) {
  while (*s) *o++ = *s++;
  *o = 0;
  return o;
}
char* catU(char* o, uint32_t v) {
  char t[12];
  int n = 0;
  do { t[n++] = char('0' + v % 10); v /= 10; } while (v);
  while (n) *o++ = t[--n];
  *o = 0;
  return o;
}
void put32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); p[2] = uint8_t(v >> 16); p[3] = uint8_t(v >> 24); }
void put16(uint8_t* p, uint32_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); }
uint32_t get32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint32_t get16(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8; }
void trackPath(char* out, const char* dir, int t, const char* ext) {
  char* o = cat(cat(out, dir), "/track");
  *o++ = char('1' + t);
  *o = 0;
  cat(o, ext);
}
// The next number after a key on the line: "level 100" -> 100; -1 if absent.
long valueAfter(const char* line, const char* key) {
  const size_t k = std::strlen(key);
  for (const char* p = line; *p; ++p) {
    if ((p == line || p[-1] == ' ') && std::strncmp(p, key, k) == 0 && p[k] == ' ') {
      const char* q = p + k;
      while (*q == ' ') ++q;
      long v = 0;
      bool any = false;
      while (*q >= '0' && *q <= '9') { v = v * 10 + (*q++ - '0'); any = true; }
      return any ? v : -1;
    }
  }
  return -1;
}

}  // namespace

void folderOf(char* out, const char* mount, const char* name) { cat(cat(cat(out, mount), "/pg/"), name); }

int TapeFiles::percent() const { return total_ ? int(uint64_t(done_) * 100u / total_) : 100; }

bool TapeFiles::fail(const char* why) {
  if (h_ >= 0) fs_->close(h_);
  h_ = -1;
  why_ = why;
  st_ = State::Failed;
  return false;
}

void TapeFiles::cancel() {
  if (h_ >= 0 && fs_) fs_->close(h_);
  h_ = -1;
  st_ = State::Idle;
}

uint32_t TapeFiles::contentEnd(int t) const {
  uint32_t end = tape_->trackEnd(t);
  // trailing silence of the last blocks is left out
  int16_t piece[256];
  while (end > 0) {
    const uint32_t n = end >= 256 ? 256 : end;
    tape_->read(t, end - n, piece, n);
    for (uint32_t i = n; i-- > 0;)
      if (piece[i] != 0) return end - n + i + 1;
    end -= n;
  }
  return 0;
}

bool TapeFiles::startSave(FileSystem* fs, const char* folder, Tape* tape) {
  if (busy()) cancel();
  fs_ = fs;
  tape_ = tape;
  why_ = "";
  if (!fs || !tape || !tape->ready()) return fail("NO TAPE");
  if (!tape->idle()) return fail("TAPE BUSY");
  std::strncpy(dir_, folder, sizeof(dir_) - 1);
  dir_[sizeof(dir_) - 1] = 0;
  // the settings first (small: one write)
  char txt[512];
  char* o = cat(txt, "pgtape 1\nrate ");
  o = catU(o, tape->rate());
  o = catU(cat(o, "\nbpm "), tape->bpm());
  o = catU(cat(o, "\nloop "), tape->loopBars());
  o = cat(o, "\n");
  total_ = 0;
  for (int t = 0; t < kTracks; ++t) {
    lens_[t] = contentEnd(t);
    total_ += lens_[t];
    static const char* const kKey[kNumParams] = {"level", "pan", "delay", "reverb", "mute"};
    o = catU(cat(o, "track "), uint32_t(t + 1));
    for (int p = 0; p < kNumParams; ++p) o = catU(cat(cat(cat(o, " "), kKey[p]), " "), tape->param(t, p));
    o = cat(catU(cat(o, " samples "), lens_[t]), "\n");
  }
  char path[64];
  cat(cat(path, dir_), "/tape.txt");
  if (!fs->write(path, reinterpret_cast<const uint8_t*>(txt), size_t(o - txt))) return fail("TAPE: WRITE FAILED");
  done_ = 0;
  track_ = -1;
  h_ = -1;
  st_ = State::Saving;
  return true;
}

bool TapeFiles::startLoad(FileSystem* fs, const char* folder, Tape* tape) {
  if (busy()) cancel();
  fs_ = fs;
  tape_ = tape;
  why_ = "";
  loaded_ = false;
  if (!fs || !tape || !tape->ready()) return fail("NO TAPE");
  if (!tape->idle()) return fail("TAPE BUSY");
  std::strncpy(dir_, folder, sizeof(dir_) - 1);
  dir_[sizeof(dir_) - 1] = 0;
  tape->clear();
  char path[64];
  cat(cat(path, dir_), "/tape.txt");
  char txt[640];
  size_t n = 0;
  if (!fs->exists(path) || !fs->read(path, reinterpret_cast<uint8_t*>(txt), sizeof(txt) - 1, &n)) {
    st_ = State::Done;   // no tape folder: an empty tape
    why_ = "";
    total_ = done_ = 0;
    return true;
  }
  txt[n] = 0;
  total_ = 0;
  for (uint32_t& l : lens_) l = 0;
  uint32_t rate = 0;
  long bpm = 0, loop = kDefaultLoop;
  // line by line
  for (char* line = txt; line && *line;) {
    char* nl = std::strchr(line, '\n');
    if (nl) *nl = 0;
    if (char* hash = std::strchr(line, '#')) *hash = 0;
    if (std::strncmp(line, "rate ", 5) == 0) rate = uint32_t(valueAfter(line, "rate"));
    else if (std::strncmp(line, "bpm ", 4) == 0) bpm = valueAfter(line, "bpm");
    else if (std::strncmp(line, "loop ", 5) == 0) loop = valueAfter(line, "loop");
    else if (std::strncmp(line, "track ", 6) == 0) {
      const long t = valueAfter(line, "track") - 1;
      if (t >= 0 && t < kTracks) {
        static const char* const kKey[kNumParams] = {"level", "pan", "delay", "reverb", "mute"};
        for (int p = 0; p < kNumParams; ++p) {
          const long v = valueAfter(line, kKey[p]);
          if (v >= 0) tape->setParam(int(t), p, uint8_t(v > kParamMax[p] ? kParamMax[p] : v));
        }
        const long s = valueAfter(line, "samples");
        lens_[t] = s > 0 ? uint32_t(s) : 0;
      }
    }
    line = nl ? nl + 1 : nullptr;
  }
  if (rate != tape->rate()) {
    st_ = State::Failed;
    why_ = rate == 44100 ? "TAPE IS 44.1K: NOT LOADED" : rate == 48000 ? "TAPE IS 48K: NOT LOADED"
                                                        : rate == 32000 ? "TAPE IS 32K: NOT LOADED" : "TAPE RATE: NOT LOADED";
    tape->clear();
    return false;
  }
  tape->setLoop(uint8_t(loop > 0 && loop < 256 ? loop : kDefaultLoop));
  tape->setBpm(uint16_t(bpm > 0 && bpm < 1000 ? bpm : 0));
  for (int t = 0; t < kTracks; ++t) total_ += lens_[t];
  loaded_ = true;
  done_ = 0;
  track_ = -1;
  h_ = -1;
  st_ = State::Loading;
  return true;
}

// The next track to move: opens its file (and writes / checks its header).
bool TapeFiles::openTrack() {
  char path[64];
  while (++track_ < kTracks) {
    pos_ = 0;
    len_ = lens_[track_];
    if (st_ == State::Saving) {
      if (len_ == 0) {   // empty now: its old file goes
        trackPath(path, dir_, track_, ".wav");
        if (fs_->exists(path)) fs_->remove(path);
        continue;
      }
      trackPath(path, dir_, track_, ".tmp");
      h_ = fs_->open(path, true);
      if (h_ < 0) return fail("TAPE: WRITE FAILED");
      uint8_t hd[44];
      std::memcpy(hd, "RIFF", 4);
      put32(hd + 4, 36 + len_ * 2);
      std::memcpy(hd + 8, "WAVEfmt ", 8);
      put32(hd + 16, 16);
      put16(hd + 20, 1);   // PCM
      put16(hd + 22, 1);   // mono
      put32(hd + 24, tape_->rate());
      put32(hd + 28, tape_->rate() * 2);
      put16(hd + 32, 2);
      put16(hd + 34, 16);
      std::memcpy(hd + 36, "data", 4);
      put32(hd + 40, len_ * 2);
      if (!fs_->writeSome(h_, hd, sizeof(hd))) return fail("TAPE: WRITE FAILED");
      return true;
    }
    // loading
    trackPath(path, dir_, track_, ".wav");
    if (len_ == 0 && !fs_->exists(path)) continue;
    if (!fs_->exists(path)) continue;   // the settings name a track whose file is gone: silence
    h_ = fs_->open(path, false);
    if (h_ < 0) return fail("TAPE: READ FAILED");
    // RIFF header, then chunks up to "data"
    uint8_t hd[12];
    if (fs_->readSome(h_, hd, 12) != 12 || std::memcmp(hd, "RIFF", 4) != 0 || std::memcmp(hd + 8, "WAVE", 4) != 0)
      return fail("TAPE: NOT A WAV");
    bool fmtOk = false;
    for (int guard = 0; guard < 16; ++guard) {
      uint8_t ch[8];
      if (fs_->readSome(h_, ch, 8) != 8) return fail("TAPE: BAD WAV");
      uint32_t sz = get32(ch + 4);
      if (std::memcmp(ch, "fmt ", 4) == 0) {
        uint8_t f[40];
        const uint32_t want = sz < sizeof(f) ? sz : uint32_t(sizeof(f));
        if (fs_->readSome(h_, f, want) != int(want) || want < 16) return fail("TAPE: BAD WAV");
        if (get16(f) != 1 || get16(f + 2) != 1 || get16(f + 14) != 16) return fail("TAPE: WAV NOT 16-BIT MONO");
        if (get32(f + 4) != tape_->rate()) return fail("TAPE WAV RATE: NOT LOADED");
        fmtOk = true;
        sz -= want;
      } else if (std::memcmp(ch, "data", 4) == 0) {
        if (!fmtOk) return fail("TAPE: BAD WAV");
        len_ = sz / 2;
        lens_[track_] = len_;
        return true;
      }
      // skip the rest of the chunk (word aligned)
      uint32_t skip = sz + (sz & 1);
      uint8_t junk[64];
      while (skip) {
        const uint32_t m = skip < sizeof(junk) ? skip : uint32_t(sizeof(junk));
        if (fs_->readSome(h_, junk, m) != int(m)) return fail("TAPE: BAD WAV");
        skip -= m;
      }
    }
    return fail("TAPE: BAD WAV");
  }
  return true;   // no track left
}

TapeFiles::State TapeFiles::step(uint32_t budget) {
  if (busy()) move(budget);
  if (st_ == State::Done || st_ == State::Failed) {   // reported once
    const State s = st_;
    st_ = State::Idle;
    return s;
  }
  return st_;
}

void TapeFiles::move(uint32_t budget) {
  char path[64], tmp[64];
  while (budget > 0 && busy()) {
    if (h_ < 0) {
      if (!openTrack()) return;
      if (track_ >= kTracks) {
        why_ = st_ == State::Saving ? "TAPE SAVED" : "TAPE LOADED";
        st_ = State::Done;
        return;
      }
      if (h_ < 0) continue;
    }
    uint32_t n = len_ - pos_;
    if (n > kPiece) n = kPiece;
    if (n > budget) n = budget;
    uint8_t* b = reinterpret_cast<uint8_t*>(buf_);   // little-endian int16, as the WAV has it
    if (st_ == State::Saving) {
      tape_->read(track_, pos_, buf_, n);
      if (!fs_->writeSome(h_, b, n * 2)) { fail("TAPE: WRITE FAILED"); return; }
    } else {
      uint32_t got = 0;
      while (got < n * 2) {
        const int r = fs_->readSome(h_, b + got, n * 2 - got);
        if (r <= 0) break;
        got += uint32_t(r);
      }
      n = got / 2;
      if (n == 0) pos_ = len_;   // the file is shorter than its header says
      else if (!tape_->write(track_, pos_, buf_, n)) { fail("TAPE: MEMORY FULL"); return; }
    }
    pos_ += n;
    done_ += n;
    budget = budget > n ? budget - n : 0;
    if (pos_ >= len_) {
      const bool ok = fs_->close(h_);
      h_ = -1;
      if (st_ == State::Saving) {
        if (!ok) { fail("TAPE: WRITE FAILED"); return; }
        trackPath(tmp, dir_, track_, ".tmp");
        trackPath(path, dir_, track_, ".wav");
        if (!fs_->rename(tmp, path)) { fail("TAPE: WRITE FAILED"); return; }
      }
    }
  }
}

}  // namespace tape
}  // namespace pg

#endif  // PG_TAPE
