// Everything the portable code needs from the outside world. The firmware
// (src/) and the desktop host (host/) each implement these.
#pragma once
#include "config.h"

namespace pg {

// ---- Keyboard: physical key positions on the Cardputer's 4x14 matrix. -------
//   row 0:  `  1  2  3  4  5  6  7  8  9  0  -  =  del
//   row 1: tab q  w  e  r  t  y  u  i  o  p  [  ]  bksl
//   row 2: fn sft a  s  d  f  g  h  j  k  l  ;  '  ent
//   row 3: ctl opt alt z x  c  v  b  n  m  ,  .  /  spc
struct Key {
  uint8_t row, col;
  constexpr uint8_t id() const { return uint8_t(row * 14 + col); }
};
constexpr int kKeyRows = 4, kKeyCols = 14, kNumKeys = 56;

struct KeyEvent {
  uint8_t key;    // Key::id()
  bool    down;   // true = pressed, false = released
};

// ---- Display: 8-bit indexed framebuffer the UI draws into. -----------------
struct Framebuffer {
  uint8_t  px[kScreenW * kScreenH];
  uint16_t palette[256];   // RGB565
};

// ---- Storage ---------------------------------------------------------------
// Paths are absolute and start with a mount: "/int/..." (internal flash,
// always present) or "/sd/..." (microSD, may be absent).
class FileSystem {
 public:
  virtual ~FileSystem() = default;
  virtual bool mounted(const char* mount) = 0;                       // "/int" or "/sd"
  virtual bool read(const char* path, uint8_t* buf, size_t cap, size_t* len) = 0;
  virtual bool write(const char* path, const uint8_t* buf, size_t len) = 0;
  virtual bool remove(const char* path) = 0;
  virtual bool exists(const char* path) = 0;
  // Calls cb(name, size, user) for each file in dir; returns count or -1.
  virtual int  list(const char* dir, void (*cb)(const char* name, size_t size, void* user), void* user) = 0;
};

}  // namespace pg
