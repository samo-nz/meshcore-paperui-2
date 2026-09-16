#pragma once
#include "../ui_screen_mgr.h"
namespace ui::screen::welcome {
extern screen_lifecycle_t lifecycle;
extern screen_lifecycle_t presets_lifecycle;
bool needed();
// Reopen setup from Mesh Settings without changing the first-boot flag.
void open();
}
