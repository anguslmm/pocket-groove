// Where things live on the Daisy Seed: the QSPI flash map and the RAM buffers'
// sizes. One place for the firmware (daisy/src), the linker script (its numbers
// are checked against these by host/daisy/test_daisy_core.cpp) and the host
// simulation. docs/DAISY.md "Memory map" explains it.
//
// QSPI flash (IS25LP064A, 8 MB at 0x90000000, 4 KB sectors, 64 KB blocks):
//   0x90000000  256 KB  left alone (the Daisy bootloader's, "may be used in the future")
//   0x90040000  480 KB  the app image: the bootloader copies exactly 0x78000 bytes from
//                       here to AXI SRAM 0x24000000 and jumps there (BOOT_SRAM; read
//                       from the bootloader binary, docs/DAISY.md)
//   0x900B8000  288 KB  ROM: the big constant tables (the sampler's built-in sounds),
//                       the same .bin's tail; copied to SDRAM at boot
//   0x90100000    2 MB  /int: two 1 MB snapshot banks (fs_persist.h)
//   0x90300000  3.2 MB  the sample pool's copy (pool_mirror.h), 0x330000 bytes
//   0x90630000  1.8 MB  free
// A firmware update (DFU) writes only the image and ROM, so /int and the pool survive it.
#pragma once
#include <cstdint>

#include "pg/sampler/sample_format.h"

namespace pgd {
namespace layout {

constexpr uint32_t kQspiBase = 0x90000000u;
constexpr uint32_t kQspiBytes = 8u * 1024u * 1024u;
constexpr uint32_t kAppAddr = 0x90040000u;
constexpr uint32_t kAppMax = 0x78000u;                   // what the bootloader copies
constexpr uint32_t kRomAddr = kAppAddr + kAppMax;        // 0x900B8000
constexpr uint32_t kRomMax = 0x90100000u - kRomAddr;     // 288 KB
constexpr uint32_t kFsAddr = 0x90100000u;
constexpr uint32_t kFsBankBytes = 1024u * 1024u;
constexpr uint32_t kPoolAddr = kFsAddr + 2u * kFsBankBytes;   // 0x90300000
constexpr uint32_t kPoolBytes = pg::sampler::kPoolBytes;
constexpr uint32_t kFreeAddr = kPoolAddr + kPoolBytes;        // 0x90630000

// /int in SDRAM: kMaxFiles slots of this size (a project is at most ~69 KB).
constexpr uint32_t kFsSlotBytes = 96u * 1024u;

// The USB serial port's buffers (SDRAM): a report is ~4 KB, a screen frame ~44 KB.
constexpr uint32_t kTxRing = 128u * 1024u;
constexpr uint32_t kRxRing = 8u * 1024u;

static_assert(kRomAddr == 0x900B8000u, "ROM follows the 480 KB image");
static_assert(kFsAddr % 65536u == 0 && kPoolAddr % 65536u == 0, "regions on 64 KB blocks");
static_assert(kFreeAddr <= kQspiBase + kQspiBytes, "fits the 8 MB flash");

}  // namespace layout
}  // namespace pgd
