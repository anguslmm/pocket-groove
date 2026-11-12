// The strip under the device screen (docs/DESKTOP.md): the desktop app's own
// controls, drawn with the instrument's font and palette (pg::ui::Gfx on a
// spare pg::Framebuffer, of which the top kStripH rows are used), so it looks
// like part of the instrument without being part of its screen.
//
//   row 1-2  modifier legend: the Mac key above each Cardputer modifier; the
//            chip lights while the key mapper has that key down (so it shows
//            a modifier really registers)
//   row 3    controls (mouse or shortcut): LISTEN Cmd+L, STEMS Cmd+B,
//            REC Cmd+R, STATS Cmd+I
//   row 4    device status (a "Pocket Groove" audio input present) and
//            Listen's state or hint; with STATS on, the audio stats instead
#pragma once
#include <cstdint>
#include <string>

#include "pg/platform.h"

namespace desk {

constexpr int kStripH = 47;   // device pixels under the 135-row screen

enum StripButton { kBtnNone = -1, kBtnListen = 0, kBtnStems, kBtnRec, kBtnStats, kNumButtons };

struct StripState {
  uint64_t held = 0;            // Cardputer keys the key mapper holds down (bit = key id)
  bool devicePresent = false;   // the "Pocket Groove" audio input exists
  bool listenWanted = false, listenActive = false, listenWarn = false;
  bool stemsOpen = false, stemsBusy = false;
  bool rec = false;
  int recSec = 0;
  bool stats = false;
  std::string status;           // row 4 text
  bool operator==(const StripState& o) const {
    return held == o.held && devicePresent == o.devicePresent && listenWanted == o.listenWanted &&
           listenActive == o.listenActive && listenWarn == o.listenWarn && stemsOpen == o.stemsOpen &&
           stemsBusy == o.stemsBusy && rec == o.rec && recSec == o.recSec && stats == o.stats && status == o.status;
  }
  bool operator!=(const StripState& o) const { return !(*this == o); }
};

// The modifiers in the legend, left to right: Cardputer key id, its name, the Mac key.
struct StripModifier {
  int key;
  const char* card;
  const char* mac;
};
extern const StripModifier kStripModifiers[7];

void drawStrip(pg::Framebuffer& fb, const StripState& s);
// Which control is at strip coordinates (x, y), device pixels from the strip's top left.
int stripHit(int x, int y);
// Centre of a control or of a modifier chip (strip coordinates), for tests and scripts.
void stripButtonCentre(int button, int* x, int* y);
void stripModifierCentre(int index, int* x, int* y);

}  // namespace desk
