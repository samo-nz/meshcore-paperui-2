#ifdef BOARD_EPAPER
#include <Arduino.h>
#include <epdiy.h>
#include "board.h"
#include "ble_companion.h"
#include "nvs_param.h"
#include "../lib/epdiy/examples/demo/main/firasans_20.h"

namespace ble_companion {
static constexpr uint32_t EXIT_HOLD_MS = 2000;

static void line(const char* text, int baseline_y, uint8_t* fb) {
    int x = 35, y = baseline_y;
    epd_write_default(&FiraSans_20, text, &x, &y, fb);
    // A second one-pixel-offset impression emboldens the existing font. This
    // screen is drawn only once, before BLE starts, so runtime is unaffected.
    x = 36; y = baseline_y;
    epd_write_default(&FiraSans_20, text, &x, &y, fb);
}

void draw_notice() {
    pinMode(BOARD_BOOT_BTN, INPUT_PULLUP);
    uint8_t* fb = epd_hl_get_framebuffer(&board::hl);
    if (!fb) return;
    epd_hl_set_all_white(&board::hl);
    line("MeshCore companion", 100, fb);
    line("Bluetooth mode", 170, fb);
    line("Open MeshCore app", 270, fb);
    line("Hold BOOT for PaperUI", 440, fb);

    // Draw and turn the panel fully off BEFORE BLE initializes/advertises.
    // No waveform jobs or renderer work may steal time from an app session.
    if (board::i2c_mutex) xSemaphoreTake(board::i2c_mutex, portMAX_DELAY);
    epd_poweron();
    int temperature = epd_ambient_temperature();
    if (board::i2c_mutex) xSemaphoreGive(board::i2c_mutex);
    EpdDrawError result = epd_hl_update_screen(&board::hl, MODE_GL16, temperature);
    if (board::i2c_mutex) xSemaphoreTake(board::i2c_mutex, portMAX_DELAY);
    epd_poweroff();
    if (board::i2c_mutex) xSemaphoreGive(board::i2c_mutex);
    Serial.printf("BLE screen: static notice draw=%x\n", result);
}

void poll_exit_button() {
    static uint32_t pressed_at = 0;
    static bool handled = false;
    if (digitalRead(BOARD_BOOT_BTN) != LOW) {
        pressed_at = 0;
        handled = false;
        return;
    }
    if (!pressed_at) pressed_at = millis();
    if (!handled && millis() - pressed_at >= EXIT_HOLD_MS) {
        handled = true;
        // NVS is committed before restart. UI-mode boot never starts BLE and
        // the controller is fully powered off rather than merely hidden.
        nvs_param_set_u8(NVS_ID_BLE_ENABLED, 0);
        Serial.println("MODE: BOOT held; restarting into PaperUI");
        delay(100);
        ESP.restart();
    }
}
} // namespace ble_companion
#endif
