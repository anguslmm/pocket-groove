// Encode/decode timing for a typical song, and peak stack of save/load
// measured by running them on a painted private stack (ucontext).
#include <ucontext.h>

#include "tests/test_io_util.h"

using namespace pg;
using namespace pg::io;
using namespace iot;
using pgtest::MemFs;

#if defined(__SANITIZE_ADDRESS__)
#define PG_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define PG_ASAN 1
#endif
#endif

namespace {
#ifndef PG_ASAN
constexpr size_t kStack = 64 * 1024;
alignas(16) uint8_t g_stack[kStack];
ucontext_t g_main, g_task;
void (*g_fn)() = nullptr;
void trampoline() { g_fn(); }

// Bytes of g_stack touched by fn (includes the fake FS's own frames).
size_t stackUse(void (*fn)()) {
  std::memset(g_stack, 0xCD, kStack);
  g_fn = fn;
  getcontext(&g_task);
  g_task.uc_stack.ss_sp = g_stack;
  g_task.uc_stack.ss_size = kStack;
  g_task.uc_link = &g_main;
  makecontext(&g_task, trampoline, 0);
  swapcontext(&g_main, &g_task);
  size_t i = 0;
  while (i < kStack && g_stack[i] == 0xCD) ++i;
  return kStack - i;
}

// Minimal FileSystem with no library calls on the stack, so the measured
// peak is the io code's own: static file slots, memcpy only.
struct LeanFs : pg::FileSystem {
  struct File { char path[48]; uint8_t data[72 * 1024]; size_t len; bool used; };
  static File f[3];
  int cur = -1; size_t pos = 0; bool wr = false;
  static int find(const char* p) {
    for (int i = 0; i < 3; ++i) if (f[i].used && std::strcmp(f[i].path, p) == 0) return i;
    return -1;
  }
  bool mounted(const char*) override { return true; }
  bool read(const char*, uint8_t*, size_t, size_t*) override { return false; }
  bool write(const char*, const uint8_t*, size_t) override { return false; }
  bool remove(const char* p) override { int i = find(p); if (i < 0) return false; f[i].used = false; return true; }
  bool exists(const char* p) override { return find(p) >= 0; }
  int list(const char* dir, void (*cb)(const char*, size_t, void*), void* u) override {
    const size_t dl = std::strlen(dir);
    int n = 0;
    for (auto& x : f)
      if (x.used && std::strncmp(x.path, dir, dl) == 0) { cb(x.path + dl + 1, x.len, u); ++n; }
    return n;
  }
  int open(const char* p, bool w) override {
    int i = find(p);
    if (w) {
      if (i < 0) for (int k = 0; k < 3; ++k) if (!f[k].used) { i = k; break; }
      if (i < 0) return -1;
      f[i].used = true; std::strcpy(f[i].path, p); f[i].len = 0;
    } else if (i < 0) return -1;
    cur = i; pos = 0; wr = w;
    return 0;
  }
  int readSome(int, uint8_t* b, size_t cap) override {
    size_t n = f[cur].len - pos; if (n > cap) n = cap;
    std::memcpy(b, f[cur].data + pos, n); pos += n; return int(n);
  }
  bool writeSome(int, const uint8_t* b, size_t n) override {
    if (f[cur].len + n > sizeof(f[cur].data)) return false;
    std::memcpy(f[cur].data + f[cur].len, b, n); f[cur].len += n; return true;
  }
  bool close(int) override { cur = -1; return true; }
  bool rename(const char* from, const char* to) override {
    int i = find(from); if (i < 0) return false;
    int j = find(to); if (j >= 0) f[j].used = false;
    std::strcpy(f[i].path, to); return true;
  }
  long freeBytes(const char*) override { return 1 << 20; }
};
LeanFs::File LeanFs::f[3];

pg::FileSystem* g_fs;
Project* g_p;
Project* g_d;
Status g_st;
void doNothing() {}
void doSave() { g_st = saveProject(*g_fs, kMountInt, "BENCH", *g_p); }
void doLoad() { g_st = loadProject(*g_fs, kMountInt, "BENCH", *g_d); }
void doList() {
  ProjectEntry e[4];
  g_st = listProjects(*g_fs, kMountInt, e, 4) == 1 ? Status::Ok : Status::Corrupt;
}
#endif
}  // namespace

int main() {
  auto p = newProject(), d = newProject();
  typicalProject(*p);
  MemFs fs;
#ifndef PG_ASAN
  static LeanFs lean;
  g_fs = &lean; g_p = p.get(); g_d = d.get();
#endif

  // timing
  std::vector<uint8_t> buf(sizeof(Project) + 2048);
  const int iters = 200;
  size_t n = 0;
  double t0 = nowMs();
  for (int i = 0; i < iters; ++i) encodeToBuffer(*p, "BENCH", buf.data(), buf.size(), &n);
  const double encMs = (nowMs() - t0) / iters;
  t0 = nowMs();
  for (int i = 0; i < iters; ++i) decodeFromBuffer(buf.data(), n, *d);
  const double decMs = (nowMs() - t0) / iters;
  CHECK(same(*p, *d));
  t0 = nowMs();
  for (int i = 0; i < iters / 4; ++i) saveProject(fs, kMountInt, "BENCH", *p);
  const double saveMs = (nowMs() - t0) / (iters / 4);
  t0 = nowMs();
  for (int i = 0; i < iters / 4; ++i) loadProject(fs, kMountInt, "BENCH", *d);
  const double loadMs = (nowMs() - t0) / (iters / 4);
  std::printf("typical song: %zu bytes\n", n);
  std::printf("  encode (2 passes: dry + write) %.3f ms, decode (2 passes: verify + decode) %.3f ms\n", encMs, decMs);
  std::printf("  full save via memfs %.3f ms (encode x2 + verify), full load %.3f ms\n", saveMs, loadMs);
  auto r = newProject();
  randomProject(*r);
  t0 = nowMs();
  for (int i = 0; i < 20; ++i) encodeToBuffer(*r, "BENCH", buf.data(), buf.size(), &n);
  std::printf("  random worst case encode %.3f ms (%zu bytes)\n", (nowMs() - t0) / 20, n);

#ifndef PG_ASAN
  const size_t base = stackUse(doNothing);
  const size_t s = stackUse(doSave);
  CHECK(g_st == Status::Ok);
  const size_t l = stackUse(doLoad);
  CHECK(g_st == Status::Ok && same(*p, *d));
  const size_t li = stackUse(doList);
  CHECK(g_st == Status::Ok);
  std::printf("peak stack (lean FS, minus %zu B trampoline): save %zu B, load %zu B, list(4) %zu B\n", base,
              s - base, l - base, li - base);
  CHECK(s - base <= 1024);
  CHECK(l - base <= 1024);
#else
  std::printf("stack measurement skipped under ASan\n");
#endif
  return finish("test_io_bench");
}
