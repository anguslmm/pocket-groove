// See qspi_flash.h.
#include "qspi_flash.h"

#include <cstring>

#include "daisy_seed.h"

extern daisy::DaisySeed g_hw;   // main.cpp

namespace pgdev {

namespace {
bool g_mapped = true;
constexpr uint32_t kBlock = 65536, kSector = 4096;
}  // namespace

void qspiMarkUnmapped() { g_mapped = false; }
bool qspiMapped() { return g_mapped; }

void QspiRegion::attach(daisy::QSPIHandle* q, uint32_t baseAddr, uint32_t bytes) {
  q_ = q;
  base_ = baseAddr;
  bytes_ = bytes;
}

bool QspiRegion::erase(uint32_t offset, uint32_t bytes) {
  if (!q_ || (offset % kSector) || (bytes % kSector) || !inRange(offset, bytes)) return false;
  const uint32_t t0 = daisy::System::GetUs();
  const uint32_t a = base_ + offset;
  bool ok;
  if ((a % kBlock) == 0 && (bytes % kBlock) == 0) {
    ok = q_->Erase(a, a + bytes) == daisy::QSPIHandle::Result::OK;
    g_mapped = false;   // a block erase leaves the indirect mode on
  } else {
    ok = true;
    for (uint32_t s = 0; s < bytes && ok; s += kSector) ok = q_->EraseSector(a + s) == daisy::QSPIHandle::Result::OK;
    if (ok) g_mapped = true;
  }
  const uint32_t us = daisy::System::GetUs() - t0;
  if (us > st_.maxEraseUs) st_.maxEraseUs = us;
  ++st_.erases;
  if (!ok) ++st_.failures;
  return ok;
}

bool QspiRegion::write(uint32_t offset, const void* data, uint32_t bytes) {
  if (!q_ || !inRange(offset, bytes)) return false;
  if (!bytes) return true;
  const uint32_t t0 = daisy::System::GetUs();
  // libDaisy's Write takes a non-const buffer and only reads it.
  const bool ok = q_->Write(base_ + offset, bytes, const_cast<uint8_t*>(static_cast<const uint8_t*>(data))) ==
                  daisy::QSPIHandle::Result::OK;
  g_mapped = ok;
  const uint32_t us = daisy::System::GetUs() - t0;
  if (us > st_.maxWriteUs) st_.maxWriteUs = us;
  ++st_.writes;
  if (!ok) ++st_.failures;
  return ok;
}

bool QspiRegion::read(uint32_t offset, void* data, uint32_t bytes) {
  if (!q_ || !inRange(offset, bytes)) return false;
  if (!g_mapped) {
    // Back to memory-mapped mode: libDaisy's Init configures the chip and maps it.
    if (q_->Init(g_hw.qspi_config) != daisy::QSPIHandle::Result::OK) {
      ++st_.failures;
      return false;
    }
    g_mapped = true;
    ++st_.remaps;
  }
  const uint32_t a = base_ + offset;   // the window maps 0x90000000 + chip address
  const uint32_t lo = a & ~31u, hi = (a + bytes + 31u) & ~31u;
  SCB_InvalidateDCache_by_Addr(reinterpret_cast<uint32_t*>(lo), static_cast<int32_t>(hi - lo));
  std::memcpy(data, reinterpret_cast<const void*>(a), bytes);
  ++st_.reads;
  return true;
}

}  // namespace pgdev
