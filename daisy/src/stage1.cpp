// See stage1.h.
#include "stage1.h"

namespace pgdev {
namespace stage1 {

namespace {
bool g_on = false;
}

void begin(bool enabled) { g_on = enabled; }
void service(uint32_t) {}
bool pollKey(pg::KeyEvent*) { return false; }
bool displayPresent() { return false; }
void present(const pg::Framebuffer&, int, int) {}
bool imuPresent() { return false; }
void tiltPress(int, int) {}
void tiltRelease() {}
bool tiltRead(float*, float*) { return false; }
pg::sampler::MicInput* mic() { return nullptr; }
pg::FileSystem* sdCard() { return nullptr; }
void audioInput(const float*, size_t) {}
void reportLines(LineFn fn, void* user) { fn(user, g_on ? "[stage1] on" : "[stage1] off (stage 0 build)"); }

}  // namespace stage1
}  // namespace pgdev
