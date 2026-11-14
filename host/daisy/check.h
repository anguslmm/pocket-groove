// A tiny check harness for the Daisy target's host tests (exit code = failures).
#pragma once
#include <cstdio>

namespace pgdt {
inline int& fails() {
  static int f = 0;
  return f;
}
inline int& passes() {
  static int p = 0;
  return p;
}
inline void check(bool ok, const char* what, long got = 0, long want = 0) {
  if (ok) {
    ++passes();
    return;
  }
  ++fails();
  std::printf("FAIL: %s (got %ld, want %ld)\n", what, got, want);
}
inline int finish(const char* name) {
  std::printf("%s: %d passed, %d failed\n", name, passes(), fails());
  return fails() ? 1 : 0;
}
}  // namespace pgdt

#define CHECK(c) pgdt::check((c), #c)
#define CHECK_EQ(a, b)                                                                     \
  do {                                                                                     \
    const auto pgdt_a = (a);                                                               \
    const auto pgdt_b = (b);                                                               \
    pgdt::check(pgdt_a == pgdt_b, #a " == " #b, static_cast<long>(pgdt_a), static_cast<long>(pgdt_b)); \
  } while (0)
