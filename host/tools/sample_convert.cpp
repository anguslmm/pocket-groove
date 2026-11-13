// sample_convert: an audio file -> a sample pool slot's audio (mono int16 at
// 22,050 Hz, trimmed, normalised; host/common/sample_convert.h).
//
//   build/host/sample_convert IN.wav OUT.raw     raw int16 little-endian (tools/test_sample_convert.py)
//   build/host/sample_convert IN.wav OUT.wav     a mono 16-bit WAV at 22,050 Hz
//
// Prints the length, the peak and the slot header's name for IN's stem.
#include <cstdio>
#include <string>
#include <vector>

#include "common/sample_convert.h"
#include "common/wav.h"

int main(int argc, char** argv) {
  if (argc != 3) {
    std::fprintf(stderr, "usage: sample_convert IN.wav OUT.raw|OUT.wav\n");
    return 2;
  }
  std::vector<int16_t> s;
  std::string err;
  if (!sconv::convertFile(argv[1], &s, &err)) {
    std::fprintf(stderr, "sample_convert: %s: %s\n", argv[1], err.c_str());
    return 1;
  }
  const std::string out = argv[2];
  if (out.size() > 4 && out.compare(out.size() - 4, 4, ".wav") == 0) {
    if (!wav::write(out.c_str(), s, sconv::kRate, 1)) return 1;
  } else {
    FILE* f = std::fopen(out.c_str(), "wb");
    if (!f) return 1;
    std::fwrite(s.data(), 2, s.size(), f);
    std::fclose(f);
  }
  int peak = 0;
  for (int16_t v : s) peak = std::max(peak, std::abs(int(v)));
  std::printf("%s: %zu samples (%.2f s), peak %d\n", out.c_str(), s.size(), double(s.size()) / sconv::kRate, peak);
  return 0;
}
