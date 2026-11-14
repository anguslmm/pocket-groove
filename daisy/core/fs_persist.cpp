#include "fs_persist.h"

#include <cstddef>
#include <cstring>

#include "pg/io/project_io.h"

namespace pgd {

namespace {
constexpr char kMagic[4] = {'P', 'G', 'S', 'N'};
constexpr uint32_t kVersion = 1;
}  // namespace

void FsPersist::init(RamFs* fs, pg::sampler::FlashDriver* driver, uint32_t bankBytes, uint8_t* staging) {
  fs_ = fs;
  drv_ = driver;
  bank_ = bankBytes;
  stage_ = staging;
  st_ = St::Idle;
  savedRev_ = seenRev_ = fs ? fs->revision() : 0;
}

uint32_t FsPersist::headerCrc(const Header& h) {
  return pg::io::crc32(0, reinterpret_cast<const uint8_t*>(&h), offsetof(Header, hcrc));
}

bool FsPersist::readHeader(int bank, Header* h) {
  if (!drv_->read(uint32_t(bank) * bank_, h, sizeof(*h))) return false;
  return std::memcmp(h->magic, kMagic, 4) == 0 && h->version == kVersion && h->hcrc == headerCrc(*h) &&
         h->bytes <= payloadCapacity();
}

FsPersist::Load FsPersist::load() {
  if (!fs_ || !drv_ || !stage_) return Load::Empty;
  Header h[2];
  bool ok[2];
  for (int b = 0; b < 2; ++b) ok[b] = readHeader(b, &h[b]);
  if (!ok[0] && !ok[1]) {
    stats_.bank = -1;
    savedRev_ = seenRev_ = fs_->revision();
    return Load::Empty;
  }
  int order[2] = {0, 1};
  if (ok[0] && ok[1] && static_cast<int32_t>(h[1].seq - h[0].seq) > 0) {
    order[0] = 1;
    order[1] = 0;
  } else if (!ok[0]) {
    order[0] = 1;
    order[1] = 0;
  }
  for (int k = 0; k < 2; ++k) {
    const int b = order[k];
    if (!ok[b]) continue;
    if (!drv_->read(uint32_t(b) * bank_ + kHeaderSector, stage_, h[b].bytes)) continue;
    if (pg::io::crc32(0, stage_, h[b].bytes) != h[b].crc) continue;
    if (!fs_->deserialize(stage_, h[b].bytes)) continue;
    stats_.bank = b;
    stats_.seq = seq_ = h[b].seq;
    stats_.lastBytes = h[b].bytes;
    savedRev_ = seenRev_ = fs_->revision();
    return Load::Loaded;
  }
  fs_->clear();
  stats_.bank = -1;
  savedRev_ = seenRev_ = fs_->revision();
  return Load::Corrupt;
}

void FsPersist::fail(uint32_t nowMs, const char* why) {
  ++stats_.failures;
  stats_.lastError = why;
  st_ = St::Idle;
  retryAfter_ = (nowMs + kRetryMs) | 1u;
}

bool FsPersist::step(uint32_t nowMs) {
  if (!fs_ || !drv_ || !stage_) return false;
  switch (st_) {
    case St::Idle: {
      const uint32_t rev = fs_->revision();
      if (rev != seenRev_) {
        seenRev_ = rev;
        changedMs_ = nowMs;
      }
      if (rev == savedRev_) {
        now_ = false;
        return false;
      }
      if (retryAfter_ && static_cast<int32_t>(nowMs - retryAfter_) < 0 && !now_) return false;
      if (!now_ && nowMs - changedMs_ < kQuietMs) return false;
      now_ = false;
      retryAfter_ = 0;
      bytes_ = fs_->serialize(stage_, payloadCapacity());
      if (!bytes_) {
        fail(nowMs, "the file set is larger than a bank");
        return false;
      }
      savingRev_ = rev;
      crc_ = pg::io::crc32(0, stage_, bytes_);
      target_ = stats_.bank == 0 ? 1 : 0;
      eraseTo_ = 0;
      pos_ = 0;
      startMs_ = nowMs;
      st_ = St::Erase;
      return true;
    }
    case St::Erase: {
      const uint32_t need = kHeaderSector + bytes_;
      if (eraseTo_ >= need) {
        st_ = St::Program;
        return true;
      }
      if (!drv_->erase(uint32_t(target_) * bank_ + eraseTo_, kEraseChunk)) {
        fail(nowMs, "erase failed");
        return false;
      }
      eraseTo_ += kEraseChunk;
      return true;
    }
    case St::Program: {
      if (pos_ >= bytes_) {
        st_ = St::Header;
        return true;
      }
      uint32_t n = bytes_ - pos_;
      if (n > kProgramChunk) n = kProgramChunk;
      if (!drv_->write(uint32_t(target_) * bank_ + kHeaderSector + pos_, stage_ + pos_, n)) {
        fail(nowMs, "program failed");
        return false;
      }
      pos_ += n;
      return true;
    }
    case St::Header: {
      Header h;
      std::memcpy(h.magic, kMagic, 4);
      h.version = kVersion;
      h.seq = seq_ + 1;
      h.bytes = bytes_;
      h.crc = crc_;
      h.hcrc = headerCrc(h);
      if (!drv_->write(uint32_t(target_) * bank_, &h, sizeof(h))) {
        fail(nowMs, "header write failed");
        return false;
      }
      pos_ = 0;
      verifyCrc_ = 0;
      st_ = St::Verify;
      return true;
    }
    case St::Verify: {
      if (pos_ < bytes_) {
        uint8_t buf[512];
        uint32_t left = bytes_ - pos_;
        if (left > kVerifyChunk) left = kVerifyChunk;
        while (left) {
          const uint32_t n = left < sizeof(buf) ? left : uint32_t(sizeof(buf));
          if (!drv_->read(uint32_t(target_) * bank_ + kHeaderSector + pos_, buf, n)) {
            fail(nowMs, "read-back failed");
            return false;
          }
          verifyCrc_ = pg::io::crc32(verifyCrc_, buf, n);
          pos_ += n;
          left -= n;
        }
        return true;
      }
      Header back;
      if (verifyCrc_ != crc_ || !readHeader(target_, &back) || back.seq != seq_ + 1) {
        fail(nowMs, "read-back mismatch");
        return false;
      }
      seq_ = back.seq;
      stats_.seq = seq_;
      stats_.bank = target_;
      stats_.lastBytes = bytes_;
      stats_.lastSaveMs = nowMs;
      stats_.lastDurationMs = nowMs - startMs_;
      stats_.lastError = "";
      ++stats_.saves;
      savedRev_ = savingRev_;
      st_ = St::Idle;
      return false;
    }
  }
  return false;
}

}  // namespace pgd
