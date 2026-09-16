#pragma once

namespace ble_companion {
// Paint once before BLE/GATT initializes; no panel work runs during a session.
void draw_notice();
void poll_exit_button();
}
