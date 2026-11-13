// The instrument library (macro.h "instruments"): the built-in sets compiled
// into the firmware (builtin_data.cpp, generated) and the host's overlay.
#include <atomic>

#include "builtin_data.h"
#include "macro.h"

namespace pg {
namespace macro {

namespace {
std::atomic<const LibraryOverlay*> g_overlay{nullptr};
}  // namespace

void setLibraryOverlay(const LibraryOverlay* overlay) { g_overlay.store(overlay); }

int builtinCount() { return detail::kBuiltinCount; }

const MacroSet& builtinAt(int i) {
  if (i < 0 || i >= detail::kBuiltinCount) i = 0;
  return detail::kBuiltin[i];
}

const MacroSet* findBuiltin(uint16_t id) {
  if (id == kNoInstrument) return nullptr;
  for (int i = 0; i < detail::kBuiltinCount; ++i)
    if (detail::kBuiltin[i].id == id) return &detail::kBuiltin[i];
  return nullptr;
}

const MacroSet* findInstrument(uint16_t id) {
  if (id == kNoInstrument) return nullptr;
  if (const LibraryOverlay* o = g_overlay.load(std::memory_order_acquire))
    if (const MacroSet* s = o->find(id)) return s;
  return findBuiltin(id);
}

int instrumentCount(int track) {
  int n = 0;
  for (int i = 0; i < detail::kBuiltinCount; ++i) n += detail::kBuiltin[i].track == track;
  if (const LibraryOverlay* o = g_overlay.load(std::memory_order_acquire)) n += o->extraCount(track);
  return n;
}

const MacroSet* instrumentAt(int track, int i) {
  if (i < 0) return nullptr;
  for (int k = 0; k < detail::kBuiltinCount; ++k) {
    if (detail::kBuiltin[k].track != track) continue;
    if (i-- == 0) return findInstrument(detail::kBuiltin[k].id);   // a replacement wins
  }
  const LibraryOverlay* o = g_overlay.load(std::memory_order_acquire);
  return o && i < o->extraCount(track) ? o->extra(track, i) : nullptr;
}

int instrumentIndex(int track, uint16_t id) {
  if (id == kNoInstrument) return -1;
  const int n = instrumentCount(track);
  for (int i = 0; i < n; ++i) {
    const MacroSet* s = instrumentAt(track, i);
    if (s && s->id == id) return i;
  }
  return -1;
}

}  // namespace macro
}  // namespace pg
