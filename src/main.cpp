#include <Arduino.h>
#include <esp_partition.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <SPIFFS.h>
#include "board.h"
#include "mesh/mesh_task.h"
#include "ui/ui_task.h"
#include "nvs_param.h"
#include "ble_companion.h"

static bool companion_mode = false;

static bool erase_spiffs_partition() {
    const esp_partition_t* partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, NULL);
    if (!partition) {
        Serial.println("BOOT: SPIFFS partition not found");
        return false;
    }

    uint32_t start = millis();
    Serial.printf("BOOT: erasing SPIFFS partition at 0x%lx (%lu bytes)\n", partition->address, partition->size);
    esp_err_t err = esp_partition_erase_range(partition, 0, partition->size);
    Serial.printf("BOOT: SPIFFS erase finished in %lu ms (err=%d)\n", millis() - start, err);
    return err == ESP_OK;
}

static void init_spiffs() {
    uint32_t start = millis();
    bool spiffs_ok = SPIFFS.begin(false);
    Serial.printf("BOOT: SPIFFS initial mount=%d in %lu ms\n", spiffs_ok, millis() - start);
    if (!spiffs_ok) {
        if (erase_spiffs_partition()) {
            start = millis();
            spiffs_ok = SPIFFS.begin(true);
            Serial.printf("BOOT: SPIFFS remount-after-erase=%d in %lu ms\n", spiffs_ok, millis() - start);
        }
    }
    if (!spiffs_ok) {
        Serial.println("BOOT: SPIFFS unavailable");
    }
}

void setup() {
    // Disable WiFi at driver level to free DRAM (~40KB)
    esp_wifi_stop();
    esp_wifi_deinit();
    // Initialize all hardware (serial, SPI, I2C, screen, touch, PMU, GPS, SD)
    board::init();
    init_spiffs();

    // Reserve the UI stack before BLE starts. ESP32 task stacks must live in
    // internal DRAM; starting MeshCore first can leave too little contiguous
    // memory and xTaskCreate would otherwise fail with no visible UI error.
    companion_mode = nvs_param_get_u8(NVS_ID_BLE_ENABLED) != 0;
    if (companion_mode) ble_companion::draw_notice();
    if (!companion_mode && !ui::task::start(1)) {
        delay(1000);  // Leave time for the fatal message to reach serial.
        ESP.restart();
    }

    // Initialize MeshCore on Arduino's existing setup task. A separate 8K-word
    // task consumed 32 KB of scarce internal DRAM exactly while ESP-IDF was
    // allocating BLE advertising and scan-response buffers, causing HCI
    // commands 0x2008/0x2009 to fail. The UI task is already allocated and
    // waits for MeshCore to release its initialization gate.
    mesh::task::start(-1);

    if (companion_mode) {
        Serial.println("MODE: BLE companion (PaperUI/LVGL not started)");
    }

    Serial.printf("t-paper ready %s\n", T_PAPER_FW_VERSION);
}

void loop() {
    // Reuse Arduino's already-allocated loop task for MeshCore. Creating a
    // second task here would consume the contiguous 8 KB block BLE needs to
    // finish advertising setup.
    mesh::task::run_once();
    if (companion_mode) ble_companion::poll_exit_button();
}
