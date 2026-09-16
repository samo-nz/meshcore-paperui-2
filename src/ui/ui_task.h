#pragma once

namespace ui::task {

// Reserve and start the UI task on the specified core.
// Must be called after board::init() (screen, touch initialized).
// Returns false if FreeRTOS cannot allocate the task, so boot cannot fail silently.
bool start(int core);

// True after LVGL, the display port, and all screens are initialized. MeshCore
// uses this to defer BLE allocation until the UI has finished claiming memory.
bool is_ready();

// Releases UI allocation after the boot transport has claimed its DRAM.
void allow_init();

} // namespace ui::task
