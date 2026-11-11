#include "../platform/display.h"
#include "../platform/system.h"
#include "../platform/storage.h"
#include "pg/instrument/instrument.h"
#include "pg/ui/ui.h"
static pg::Instrument g_inst;
static pg::ui::Ui g_ui;
static pg::Framebuffer g_fb;
void setup() {
  plat::sys::begin(115200);
  g_inst.init(44100.0f, true);
  g_ui.init(g_inst.engine(), g_inst.project(), &plat::storage::fs());
  g_ui.boot(0);
}
void loop() { int16_t b[256]; g_inst.process(b, 128); pg::ui::UiEnv e{0,0,false,0}; g_ui.update(e, g_fb); }
