// Minimal 16-bit PCM WAV writer/reader for host tools and tests.
#pragma once
#include <cstdint>
#include <cstdio>
#include <vector>

namespace wav {
inline bool write(const char* path, const std::vector<int16_t>& interleaved, int sampleRate, int channels) {
  FILE* f = std::fopen(path, "wb");
  if (!f) return false;
  uint32_t dataBytes = uint32_t(interleaved.size() * 2), riff = 36 + dataBytes;
  uint32_t byteRate = uint32_t(sampleRate * channels * 2), fmtLen = 16, sr = uint32_t(sampleRate);
  uint16_t pcm = 1, ch = uint16_t(channels), align = uint16_t(channels * 2), bits = 16;
  std::fwrite("RIFF", 1, 4, f); std::fwrite(&riff, 4, 1, f); std::fwrite("WAVEfmt ", 1, 8, f);
  std::fwrite(&fmtLen, 4, 1, f); std::fwrite(&pcm, 2, 1, f); std::fwrite(&ch, 2, 1, f);
  std::fwrite(&sr, 4, 1, f); std::fwrite(&byteRate, 4, 1, f); std::fwrite(&align, 2, 1, f);
  std::fwrite(&bits, 2, 1, f); std::fwrite("data", 1, 4, f); std::fwrite(&dataBytes, 4, 1, f);
  std::fwrite(interleaved.data(), 2, interleaved.size(), f);
  std::fclose(f);
  return true;
}
// Convenience for float buffers at +-1.0 full scale (clips).
inline bool writeFloat(const char* path, const std::vector<float>& interleaved, int sampleRate, int channels) {
  std::vector<int16_t> v(interleaved.size());
  for (size_t i = 0; i < v.size(); ++i) {
    float x = interleaved[i] * 32767.0f;
    v[i] = int16_t(x > 32767.0f ? 32767.0f : (x < -32768.0f ? -32768.0f : x));
  }
  return write(path, v, sampleRate, channels);
}
}  // namespace wav
