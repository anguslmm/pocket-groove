// Project storage: compact, CRC-checked .pgv files over pg::FileSystem.
//
// FILE FORMAT (all integers little-endian)
//   Header (v1: 64 bytes)
//     0  'P','G','V','F'      file magic (distinct from Project::magic 'PGRV')
//     4  u8  formatVersion    version of the writer (1)
//     5  u8  compatVersion    oldest reader version that can read this file (1).
//                             A reader rejects compatVersion > kFormatVersion
//                             (Status::NewerFormat). A later writer that only
//                             appends header fields keeps compatVersion = 1.
//     6  u16 headerSize       >= 64; later versions append fields before the CRC
//     8  u8  projectVersion   Project::version of the payload
//     9  u8  codec            1 = PGLZ (below)
//    10  u8  tempo            Project::globals[kGTempo] (for the file list)
//    11  u8  songMode
//    12  u32 rawSize          uncompressed payload size (sizeof(Project))
//    16  u32 bodySize         compressed payload size
//    20  u32 rawCrc           CRC32 of the uncompressed Project bytes
//    24  u32 bodyCrc          CRC32 of the compressed body bytes
//    28  char[16] stem        file stem the file was saved under, NUL padded
//    44  u8  swing, scaleRoot, scaleType; 13 reserved zero bytes
//    hs-4 u32 headerCrc       CRC32 of header bytes [0, headerSize-4)
//   Body (PGLZ, byte-oriented LZ77; the window is the destination Project itself)
//     c < 0x80 : literal run, c+1 bytes follow
//     c >= 0x80: match, len = 4 + (c & 0x7F), if (c & 0x7F) == 0x7F a varint is
//                added; then varint offset (1..pos). Copy is byte-wise (overlap OK).
//     varint = LEB128, at most 3 bytes. The body must decode to exactly rawSize
//     bytes, consume exactly bodySize bytes, and be followed by EOF.
//
// MEMORY: encode and decode stream through a <= 64-byte I/O buffer with a
// 128-byte match hash; no heap. Encode reads the Project in place, decode
// writes straight into the destination Project.
//
// LOAD SAFETY: load is two passes. Pass 1 validates the header, the body CRC,
// the full token structure (bounds, lengths, EOF) and Project magic/version
// without touching the destination. Only then pass 2 decodes into the
// destination and checks rawCrc. If pass 2 fails (the file changed or a read
// failed between the passes, or a deliberately forged file with a valid body
// CRC but wrong content) the destination is ZEROED and Status::DestClobbered
// is returned: the caller must initProject() or load something else. Any
// other failure leaves the destination untouched. Structural integrity only:
// the caller runs the engine's range sanitizer afterwards.
//
// SAVE SAFETY: encode is run dry first (sizes and CRCs for the header), the
// free space is checked, the file is written to <dir>/_save.tmp, re-read and
// verified (header + body CRC + structure), then renamed over the target. Any
// failure removes the temp file and leaves the old target intact. The Project
// may be edited by the engine while saving (1-byte races, see project.h):
// edits during the save are detected and the save is retried (Status::Busy
// after several attempts).
//
// Call recoverAfterPowerLoss() once at boot (per mount) before listing.
#pragma once
#include <cstddef>
#include <cstdint>

#include "pg/platform.h"
#include "pg/project.h"

namespace pg::io {

enum class Status : uint8_t {
  Ok = 0,
  BadName,        // name / path invalid
  NoMount,        // mount absent (no SD card)
  NotFound,       // file does not exist
  NoSpace,        // freeBytes says it won't fit, or a write failed with the disk full
  OpenFailed,     // open() failed on a mounted FS
  WriteFailed,    // writeSome()/close() failed
  VerifyFailed,   // read-back after write did not match
  RenameFailed,   // rename(tmp, target) failed; old file intact
  RemoveFailed,   // delete failed
  ReadFailed,     // readSome() returned an error
  NotPgFile,      // wrong file magic
  NewerFormat,    // written by a newer firmware (compatVersion too high)
  ProjectVersion, // Project::version not supported by this firmware
  Corrupt,        // CRC or structure check failed
  Truncated,      // file ends early
  BadProject,     // the Project to save has a bad magic/version
  Busy,           // the Project kept changing during save; retry later
  DestClobbered,  // load failed after the destination was overwritten: it is zeroed
  BufferTooSmall,
};
const char* statusName(Status s);

constexpr uint8_t kFormatVersion = 1;
constexpr int kHeaderSize = 64;      // v1 header size
constexpr int kMaxHeaderSize = 1024; // reader limit for future headers
constexpr int kNameMax = 8;          // user-visible project names: 1..8 of A-Z 0-9 - _
constexpr int kNameBuf = kNameMax + 1;
constexpr int kStemMax = 15;         // header stem field (16 bytes incl. NUL)
constexpr int kPathMax = 48;
constexpr long kSaveSpaceMargin = 4096;  // extra free bytes required (FS metadata, block rounding)

constexpr const char* kMountInt = "/int";
constexpr const char* kMountSd = "/sd";
constexpr const char* kSessionPath = "/int/pg/_last.pgv";
constexpr const char* kUiSettingsPath = "/int/pg/_ui.cfg";

// Optional tuning, mainly for tests (chunk sizes of readSome/writeSome calls).
struct IoOptions {
  uint16_t readChunk = 64;   // 1..64
  uint16_t writeChunk = 64;  // 1..64
};

struct HeaderInfo {
  uint8_t formatVersion, compatVersion, projectVersion, codec;
  uint8_t tempo, songMode, swing, scaleRoot, scaleType;
  uint16_t headerSize;
  uint32_t rawSize, bodySize, rawCrc, bodyCrc, headerCrc;
  char stem[kStemMax + 1];
  uint32_t fileSize() const { return uint32_t(headerSize) + bodySize; }
};

// ---- names and paths -------------------------------------------------------
bool isValidName(const char* name);                     // strict: 1..8 of A-Z 0-9 - _
// Upper-cases a-z, drops characters that are not allowed, truncates to 8.
// Returns false if nothing usable is left.
bool normalizeName(const char* in, char out[kNameBuf]);
bool isMount(const char* mount);                        // "/int" or "/sd"
// "<mount>/pg/<NAME>.pgv". false if mount or name invalid or cap too small.
bool buildProjectPath(const char* mount, const char* name, char* out, size_t cap);

// ---- projects by mount + name ---------------------------------------------
Status saveProject(FileSystem& fs, const char* mount, const char* name, const Project& p, const IoOptions* o = nullptr);
Status loadProject(FileSystem& fs, const char* mount, const char* name, Project& dest, HeaderInfo* info = nullptr, const IoOptions* o = nullptr);
bool   projectExists(FileSystem& fs, const char* mount, const char* name);
Status deleteProject(FileSystem& fs, const char* mount, const char* name);
// First free GROOVE01..GROOVE99 not present on any mounted mount. false if all taken.
bool   nextFreeName(FileSystem& fs, char out[kNameBuf]);

struct ProjectEntry {
  char name[kNameBuf];
  uint8_t tempo;      // 0 if !valid
  bool valid;         // header readable and supported
  Status status;      // header status (Ok if valid)
  uint32_t size;      // file size as listed
};
// Lists "<mount>/pg/*.pgv" whose stem is a valid name (system files such as
// _last.pgv are excluded), sorted A-Z, reading only the 64-byte headers.
// Returns the count written (<= cap; extra files are dropped), -1 if unmounted.
int listProjects(FileSystem& fs, const char* mount, ProjectEntry* out, int cap);

// ---- any path ---------------------------------------------------------------
Status saveFile(FileSystem& fs, const char* path, const char* stem, const Project& p, const IoOptions* o = nullptr);
Status loadFile(FileSystem& fs, const char* path, Project& dest, HeaderInfo* info = nullptr, const IoOptions* o = nullptr);
Status readHeader(FileSystem& fs, const char* path, HeaderInfo* info);           // header only
Status verifyFile(FileSystem& fs, const char* path, HeaderInfo* info = nullptr); // full pass-1 check, no decode

// ---- session (autosave) and UI settings --------------------------------------
Status saveSession(FileSystem& fs, const Project& p);
Status loadSession(FileSystem& fs, Project& dest);  // power-on resume; on failure use the demo
// Opaque UI blob: "PGUI", u8 version, u8 len, u16 0, u32 crc32(first 8 bytes + data), data.
constexpr size_t kUiSettingsMax = 240;
Status saveUiSettings(FileSystem& fs, uint8_t version, const uint8_t* data, size_t len);
Status loadUiSettings(FileSystem& fs, uint8_t* version, uint8_t* buf, size_t cap, size_t* len);

// Boot-time cleanup of an interrupted save on `mount`: a complete, valid
// <mount>/pg/_save.tmp (or _xfer.tmp, see installReceived) whose target is
// missing (non-atomic rename interrupted) is renamed into place; any other
// temp file is removed.
Status recoverAfterPowerLoss(FileSystem& fs, const char* mount);

// ---- files received from outside (USB transfer, pg/io/xfer.h) -----------------
// A .pgv streamed byte for byte into xferTempPath(mount) is installed as
// <mount>/pg/<name>.pgv with the save path's guarantees: the temp is re-read
// and must have exactly `size` bytes with whole-file CRC32 `crc`; then the
// full first-pass check of a load (header, version, body CRC, structure,
// Project magic/version) and header stem == name (what the power-loss
// recovery installs it under); then the same rename as saveFile. On any
// failure the temp is removed (or, after a split FAT rename, kept for
// recoverAfterPowerLoss) and an existing target is untouched.
constexpr const char* kXferTmpName = "_xfer.tmp";
bool xferTempPath(const char* mount, char* out, size_t cap);    // "<mount>/pg/_xfer.tmp"
Status installReceived(FileSystem& fs, const char* tmpPath, const char* mount, const char* name, uint32_t size,
                       uint32_t crc, HeaderInfo* info = nullptr);

// ---- in-memory codec (host tools, tests) --------------------------------------
Status encodeToBuffer(const Project& p, const char* stem, uint8_t* out, size_t cap, size_t* len);
Status decodeFromBuffer(const uint8_t* in, size_t len, Project& dest, HeaderInfo* info = nullptr);

uint32_t crc32(uint32_t crc, const uint8_t* data, size_t len);  // zlib convention, start with 0

}  // namespace pg::io
