#include <Arduino.h>
#include "set_storage.h"
#include "../ui_theme.h"
#include "../ui_screen_mgr.h"
#include "../ui_port.h"
#include "../kit/ui_kit.h"
#include "../components/toast.h"
#include "../../model.h"
#include "../../board.h"
#include "../../mesh/mesh_task.h"
#include "../../sd_log.h"
#include <SPIFFS.h>
#include <SD.h>
#include <Preferences.h>
#ifdef BOARD_EPAPER
#include <epdiy.h>
#endif

namespace ui::screen::set_storage {

using namespace ui::kit;

static void on_clear_messages(void*) {
    model::message_count = 0;
    SD.remove("/messages.bin");
    ui::toast::show("Messages cleared");
}

static void on_clear_contacts(void*) {
    mesh::task::clear_contacts();
    ui::toast::show("Contacts cleared");
}

static void on_clear_channels(void*) {
    mesh::task::clear_channels();
    ui::toast::show("Channels cleared");
}

static void on_factory_reset(void*) {
    mesh::task::flush_storage();

    // SPIFFS contains MeshCore identity/data; NVS contains the welcome flag,
    // Bluetooth mode, radio-boot advert choice, and UI preferences. Both must
    // be cleared for an actual first-boot experience. No app/boot partition is
    // touched, so the same firmware restarts into the welcome screen.
    if (!SPIFFS.format()) {
        Serial.println("Factory reset: SPIFFS format failed; reset cancelled");
        ui::toast::show("Storage reset failed");
        return;
    }
    Preferences system;
    if (!system.begin("system", false) || !system.clear()) {
        Serial.println("Factory reset: NVS clear failed; welcome flag may remain");
        ui::toast::show("Settings reset failed");
        system.end();
        return;
    }
    system.end();

    // Shut down peripherals
    mesh::task::ble_disable();
    ui::port::set_backlight(2);  // Off
#ifdef BOARD_EPAPER
    board::touch.sleep();
    digitalWrite(BOARD_TOUCH_RST, LOW);
    digitalWrite(BOARD_LORA_RST, LOW);
    epd_poweroff();
#endif

    // SD content is independent of flash and may not be mounted on this board.
    SD.remove("/messages.bin");
    SD.remove("/telemetry.bin");
    SD.remove("/contacts3");
    SD.remove("/channels2");

    Serial.println("Factory reset — rebooting...");
    vTaskDelay(pdMS_TO_TICKS(500));
    ESP.restart();
}

static void create(Handle parent) {
    Handle lst = list(parent);
    toggle_item(lst, "Messages", "Clear", on_clear_messages, nullptr);
    toggle_item(lst, "Contacts", "Clear", on_clear_contacts, nullptr);
    toggle_item(lst, "Channels", "Clear", on_clear_channels, nullptr);
    toggle_item(lst, "Factory Reset", "Reset", on_factory_reset, nullptr);
}

static void entry() {}
static void exit_fn() {}
static void destroy() {}

screen_lifecycle_t lifecycle = { create, entry, exit_fn, destroy };

} // namespace ui::screen::set_storage
