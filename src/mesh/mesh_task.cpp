#include <Arduino.h>
#include <esp_heap_caps.h>
#include <Mesh.h>
#include <SPIFFS.h>
#include <SD.h>
#include <Preferences.h>
#include <helpers/ArduinoHelpers.h>
#include <helpers/SimpleMeshTables.h>

#include "mesh_task.h"
#include "mesh_bridge.h"
#include "../util/text_filter.h"
#include "../board.h"
#include "../nvs_param.h"
#include "companion/target.h"
#include "companion/DataStore.h"
#include "companion/NodePrefs.h"
#include "companion/MyMesh.h"
#include <helpers/esp32/SerialBLEInterface.h>
#include "companion/NullSerialInterface.h"
#include "companion/BridgeUITask.h"
#include "../ui/ui_task.h"
#include "../board.h"
#include "../sd_log.h"

// ---------- Globals ----------

static SerialBLEInterface ble_serial;
static NullSerialInterface null_serial;
static BridgeUITask bridge_ui(&mc_board, &null_serial);

static StdRNG mc_rng;
static SimpleMeshTables mc_tables;

// DataStore: SPIFFS for identity/prefs, SD card for contacts/channels/blobs
static DataStore* store = NULL;
static MyMesh* the_mesh_ptr = NULL;
static SemaphoreHandle_t mesh_mutex = NULL;
static bool ble_active = false;
static bool ble_initialized = false;
static volatile bool ble_requested = false;
static volatile bool mesh_ready = false;

// BLEDevice::init() is a one-time operation. Run all later ON/OFF transitions
// from the mesh task so MyMesh::loop() never races an interface pointer swap.
static void update_ble_state() {
    if (ble_requested == ble_active) return;

    if (ble_requested) {
        if (ble_initialized) {
            // OFF releases the ESP32 controller memory, which cannot be
            // reacquired without a chip reset. NVS already contains ON, so a
            // controlled reboot returns directly with BLE enabled.
            Serial.println("BLE: restarting to power controller back on");
            delay(100);
            ESP.restart();
        } else {
            ble_serial.begin(BLE_NAME_PREFIX, the_mesh_ptr->getNodePrefs()->node_name,
                             the_mesh_ptr->getBLEPin());
            ble_initialized = true;
        }
        the_mesh_ptr->startInterface(ble_serial);
        ble_active = true;
        Serial.println("BLE: GATT service enabled and advertising");
    } else {
        ble_serial.disable();
        the_mesh_ptr->startInterface(null_serial);
        // Allow a disconnect event to drain before tearing down Bluedroid.
        vTaskDelay(pdMS_TO_TICKS(250));
        // This disables/deinitializes the controller and releases its memory.
        // Re-enabling later uses the controlled reboot path above because ESP-IDF
        // cannot reacquire memory released by esp_bt_controller_mem_release().
        BLEDevice::deinit(true);
        ble_active = false;
        Serial.println("BLE: controller powered down");
    }
}

// ---------- SD flush on silence ----------

static uint32_t last_rx_count = 0;
static uint32_t last_tx_count = 0;
static uint32_t last_activity_tick = 0;
static const uint32_t SILENCE_MS = 5000;  // flush after 5s of radio silence

// ---------- FreeRTOS task ----------

static void mesh_loop_once() {
    if (xSemaphoreTake(mesh_mutex, pdMS_TO_TICKS(50))) {
        update_ble_state();
        the_mesh_ptr->loop();

        sensors.loop();  // GPS NMEA parsing (serial only, no I2C)

        // Update bridge status
        mesh::bridge::MeshStatus s = {};
        s.peer_count = the_mesh_ptr->getNumContacts();
        s.last_rssi = radio_driver.getLastRSSI();
        s.last_snr = radio_driver.getLastSNR();
        s.radio_ok = true;
        mesh::bridge::update_status(s);

        // Detect radio silence and flush messages to SD
        uint32_t rx = radio_driver.getPacketsRecv();
        uint32_t tx = radio_driver.getPacketsSent();
        if (rx != last_rx_count || tx != last_tx_count) {
            last_rx_count = rx;
            last_tx_count = tx;
            last_activity_tick = millis();
        } else if (sd_log::is_dirty() && (millis() - last_activity_tick) > SILENCE_MS) {
            sd_log::flush();
            last_activity_tick = millis();  // avoid re-checking immediately
        }

        // rtc_clock.tick() is a no-op for ESP32RTCClock (uses system time())
        xSemaphoreGive(mesh_mutex);
    }
}

// ---------- Public API ----------

namespace mesh::task {

void start(int core) {
    mesh::bridge::init();

    // board::init() already set up SPI, Wire, GPIO.
    // Ensure SD CS is deselected before LoRa init (shared SPI bus)
    digitalWrite(BOARD_SD_CS, HIGH);

    if (!radio_init()) {
        Serial.println("MESH: radio init FAILED — continuing without mesh");
        // Don't return — still start the task so UI doesn't crash on null mesh
    }
    mc_rng.begin(radio_get_rng_seed());

    // SPIFFS for identity/prefs, SD for contacts/channels (if available)
    if (board::peri_status[E_PERI_SD_CARD]) {
        void* sm = heap_caps_malloc(sizeof(DataStore), MALLOC_CAP_SPIRAM);
        store = new (sm) DataStore(SPIFFS, SD, rtc_clock);
        Serial.println("MESH: using SD for contacts/channels");
    } else {
        void* sm = heap_caps_malloc(sizeof(DataStore), MALLOC_CAP_SPIRAM);
        store = new (sm) DataStore(SPIFFS, rtc_clock);
        Serial.println("MESH: using SPIFFS only (no SD)");
    }
    store->begin();

    // Load saved messages from SD card (if available)
    if (board::peri_status[E_PERI_SD_CARD]) {
        sd_log::load();
    }

    // Create mesh — allocate in PSRAM to save DRAM (MyMesh contains contacts[] array ~16KB)
    void* mesh_mem = heap_caps_malloc(sizeof(MyMesh), MALLOC_CAP_SPIRAM);
    the_mesh_ptr = new (mesh_mem) MyMesh(radio_driver, mc_rng, rtc_clock, mc_tables, *store, &bridge_ui);
    the_mesh_ptr->begin(false); // no display (we handle UI separately)

    // Disable auto-add — contacts are added explicitly from Discovery screen
    if (the_mesh_ptr->getNodePrefs()->manual_add_contacts == 0) {
        the_mesh_ptr->getNodePrefs()->manual_add_contacts = 1;
        the_mesh_ptr->savePrefs();
    }

    mesh_mutex = xSemaphoreCreateMutex();
    if (!mesh_mutex) {
        Serial.println("FATAL: mesh mutex allocation failed");
        delay(1000);
        ESP.restart();
    }

    // The UI task stack is already reserved, but its heap-heavy initialization
    // is paused. Claim BLE controller/GATT memory now, then release the UI. This
    // prevents both a missing UI task and a half-created BLE service.
    ble_requested = nvs_param_get_u8(NVS_ID_BLE_ENABLED) != 0;
    if (ble_requested) {
        Serial.printf("BLE: init heap free=%u largest=%u\n",
            heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
            heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        ble_serial.begin(BLE_NAME_PREFIX, the_mesh_ptr->getNodePrefs()->node_name,
                         the_mesh_ptr->getBLEPin());
        ble_initialized = true;
        the_mesh_ptr->startInterface(ble_serial);
        ble_active = true;
        Serial.printf("BLE: GATT/advertising requested; heap free=%u largest=%u\n",
            heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
            heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    } else {
        the_mesh_ptr->startInterface(null_serial);
    }
    if (!ble_requested) ui::task::allow_init();

    // Apply radio params from prefs
    NodePrefs* prefs = the_mesh_ptr->getNodePrefs();
    radio_set_params(prefs->freq, prefs->bw, prefs->sf, prefs->cr);
    radio_set_tx_power(prefs->tx_power_dbm);

    sensors.begin();
#if ENV_INCLUDE_GPS == 1
    the_mesh_ptr->applyGpsPrefs();
#endif

    Serial.printf("MESH: node '%s' started on %.3f MHz\n", the_mesh_ptr->getNodeName(), prefs->freq);

    // Never announce automatically unless the user opted in under Mesh Settings.
    // ENABLE_ADVERT_ON_BOOT is only the default on a clean installation (OFF).
    if (get_advert_on_boot()) the_mesh_ptr->advert();

    last_activity_tick = millis();
    mesh_ready = true;
}

bool is_ready() { return mesh_ready; }
bool get_advert_on_boot() {
    Preferences settings;
    if (!settings.begin("system", true)) return ENABLE_ADVERT_ON_BOOT == 1;
    bool enabled = settings.getBool("advert_boot", ENABLE_ADVERT_ON_BOOT == 1);
    settings.end();
    return enabled;
}
void set_advert_on_boot(bool enabled) {
    Preferences settings;
    if (!settings.begin("system", false)) return;
    settings.putBool("advert_boot", enabled);
    settings.end();
}
bool ble_is_connected() { return ble_active && ble_serial.isConnected(); }

void run_once() {
    if (!mesh_ready || !mesh_mutex) {
        vTaskDelay(pdMS_TO_TICKS(10));
        return;
    }
    static bool announced = false;
    if (!announced) {
        Serial.println("MESH: protocol loop running on Arduino task");
        announced = true;
    }
    mesh_loop_once();
    vTaskDelay(pdMS_TO_TICKS(10));
}

bool send_message(const char* recipient_prefix, const char* text) {
    if (!the_mesh_ptr || !mesh_mutex) return false;
    char clean[160]; util::strip_emoji_copy(clean, sizeof(clean), text);
    bool ok = false;
    if (xSemaphoreTake(mesh_mutex, pdMS_TO_TICKS(500))) {
        ContactInfo* r = the_mesh_ptr->searchContactsByPrefix(recipient_prefix);
        if (r) {
            uint32_t ack, timeout;
            int result = the_mesh_ptr->sendMessage(*r, rtc_clock.getCurrentTime(), 0, clean, ack, timeout);
            ok = (result != MSG_SEND_FAILED);
        }
        xSemaphoreGive(mesh_mutex);
    }
    return ok;
}

bool send_to_name(const char* name, const char* text) {
    if (!the_mesh_ptr || !mesh_mutex || !name || !text) return false;
    char clean[160]; util::strip_emoji_copy(clean, sizeof(clean), text);
    bool ok = false;
    if (xSemaphoreTake(mesh_mutex, pdMS_TO_TICKS(500))) {
        ContactInfo* r = the_mesh_ptr->searchContactsByPrefix(name);
        if (r) {
            uint32_t ack, timeout;
            int result = the_mesh_ptr->sendMessage(*r, rtc_clock.getCurrentTime(), 0, clean, ack, timeout);
            ok = (result != MSG_SEND_FAILED);
        }
        xSemaphoreGive(mesh_mutex);
    }
    return ok;
}

bool send_public(const char* text) {
    return send_channel(0, text);
}

bool send_channel(uint8_t channel_idx, const char* text) {
    if (!the_mesh_ptr || !mesh_mutex || !text) return false;
    char clean[160]; util::strip_emoji_copy(clean, sizeof(clean), text);
    bool ok = false;
    if (xSemaphoreTake(mesh_mutex, pdMS_TO_TICKS(500))) {
        ChannelDetails ch;
        if (the_mesh_ptr->getChannel(channel_idx, ch) && ch.name[0]) {
            ok = the_mesh_ptr->sendGroupMessage(
                rtc_clock.getCurrentTime(), ch.channel,
                the_mesh_ptr->getNodeName(), clean, strlen(clean));
        }
        xSemaphoreGive(mesh_mutex);
    }
    return ok;
}

const char* node_name() {
    if (!the_mesh_ptr) return "LilyGo T5 ePaper S3 Pro";
    return the_mesh_ptr->getNodeName();
}

double get_node_lat() {
    return sensors.node_lat;
}

double get_node_lon() {
    return sensors.node_lon;
}

float get_freq() {
    if (!the_mesh_ptr) return LORA_FREQ;
    return the_mesh_ptr->getNodePrefs()->freq;
}

float get_bw() {
    if (!the_mesh_ptr) return LORA_BW;
    return the_mesh_ptr->getNodePrefs()->bw;
}

uint8_t get_sf() {
    if (!the_mesh_ptr) return LORA_SF;
    return the_mesh_ptr->getNodePrefs()->sf;
}

uint8_t get_cr() {
    if (!the_mesh_ptr) return LORA_CR;
    return the_mesh_ptr->getNodePrefs()->cr;
}

int8_t get_tx_power() {
    if (!the_mesh_ptr) return LORA_TX_POWER;
    return the_mesh_ptr->getNodePrefs()->tx_power_dbm;
}

uint32_t get_packets_recv() {
    return radio_driver.getPacketsRecv();
}

uint32_t get_packets_sent() {
    return radio_driver.getPacketsSent();
}

// ---------- Param setters (save prefs, need reboot for radio params) ----------

void set_node_name(const char* name) {
    if (!the_mesh_ptr) return;
    NodePrefs* p = the_mesh_ptr->getNodePrefs();
    util::strip_emoji_copy(p->node_name, sizeof(p->node_name), name);
    the_mesh_ptr->savePrefs();
}

void set_freq(float freq_mhz) {
    if (!the_mesh_ptr) return;
    the_mesh_ptr->getNodePrefs()->freq = freq_mhz;
    the_mesh_ptr->savePrefs();
}

void set_bw(float bw_khz) {
    if (!the_mesh_ptr) return;
    the_mesh_ptr->getNodePrefs()->bw = bw_khz;
    the_mesh_ptr->savePrefs();
}

void set_sf(uint8_t sf) {
    if (!the_mesh_ptr) return;
    the_mesh_ptr->getNodePrefs()->sf = sf;
    the_mesh_ptr->savePrefs();
}

void set_cr(uint8_t cr) {
    if (!the_mesh_ptr) return;
    the_mesh_ptr->getNodePrefs()->cr = cr;
    the_mesh_ptr->savePrefs();
}

void set_tx_power(int8_t dbm) {
    if (!the_mesh_ptr) return;
    the_mesh_ptr->getNodePrefs()->tx_power_dbm = dbm;
    the_mesh_ptr->savePrefs();
}

void set_path_hash_mode(uint8_t mode) {
    if (!the_mesh_ptr || mode > 2) return;
    the_mesh_ptr->getNodePrefs()->path_hash_mode = mode;
    the_mesh_ptr->savePrefs();
}

uint8_t get_path_hash_mode() {
    return the_mesh_ptr ? the_mesh_ptr->getNodePrefs()->path_hash_mode : 0;
}

bool save_radio_profile(float freq_mhz, float bw_khz, uint8_t sf, uint8_t cr,
                        uint8_t path_hash_mode, int8_t tx_power_dbm) {
    if (!the_mesh_ptr || !mesh_mutex) return false;
    // Match the companion protocol's parameter limits. Regional legality is
    // deliberately the user's choice; this checks only valid radio inputs.
    if (!(freq_mhz >= 150.0f && freq_mhz <= 960.0f) ||
        !(bw_khz >= 7.0f && bw_khz <= 500.0f) || sf < 5 || sf > 12 ||
        cr < 5 || cr > 8 || path_hash_mode > 2 ||
        tx_power_dbm < -9 || tx_power_dbm > MAX_LORA_TX_POWER) return false;
    if (xSemaphoreTake(mesh_mutex, pdMS_TO_TICKS(500)) != pdTRUE) return false;
    NodePrefs* p = the_mesh_ptr->getNodePrefs();
    p->freq = freq_mhz;
    p->bw = bw_khz;
    p->sf = sf;
    p->cr = cr;
    p->path_hash_mode = path_hash_mode;
    p->tx_power_dbm = tx_power_dbm;
    the_mesh_ptr->savePrefs();
    xSemaphoreGive(mesh_mutex);
    return true;
}

void send_advert(bool flood) {
    if (!the_mesh_ptr || !mesh_mutex) return;
    if (xSemaphoreTake(mesh_mutex, pdMS_TO_TICKS(500))) {
        the_mesh_ptr->advert(flood);
        xSemaphoreGive(mesh_mutex);
    }
}

void set_gps_enabled(bool enabled) {
    if (!the_mesh_ptr) return;
#if ENV_INCLUDE_GPS == 1
    if (!mesh_mutex) {
        the_mesh_ptr->getNodePrefs()->gps_enabled = enabled ? 1 : 0;
        the_mesh_ptr->applyGpsPrefs();
        the_mesh_ptr->savePrefs();
        return;
    }
    if (xSemaphoreTake(mesh_mutex, pdMS_TO_TICKS(500))) {
        the_mesh_ptr->getNodePrefs()->gps_enabled = enabled ? 1 : 0;
        the_mesh_ptr->applyGpsPrefs();
        the_mesh_ptr->savePrefs();
        xSemaphoreGive(mesh_mutex);
    }
#else
    the_mesh_ptr->getNodePrefs()->gps_enabled = enabled ? 1 : 0;
    the_mesh_ptr->savePrefs();
#endif
}

bool get_gps_enabled() {
    if (!the_mesh_ptr) return false;
#if ENV_INCLUDE_GPS == 1
    if (mesh_mutex && xSemaphoreTake(mesh_mutex, pdMS_TO_TICKS(500))) {
        const char* gps_enabled = sensors.getSettingByKey("gps");
        bool enabled = gps_enabled ? strcmp(gps_enabled, "1") == 0
                                   : the_mesh_ptr->getNodePrefs()->gps_enabled != 0;
        xSemaphoreGive(mesh_mutex);
        return enabled;
    }
#endif
    return the_mesh_ptr->getNodePrefs()->gps_enabled != 0;
}

void set_advert_location(bool share) {
    if (!the_mesh_ptr) return;
    the_mesh_ptr->getNodePrefs()->advert_loc_policy = share ? 1 : 0;
    the_mesh_ptr->savePrefs();
}

bool get_advert_location() {
    if (!the_mesh_ptr) return false;
    return the_mesh_ptr->getNodePrefs()->advert_loc_policy != 0;
}

void set_client_repeat(bool enabled) {
    if (!the_mesh_ptr) return;
    the_mesh_ptr->getNodePrefs()->setRepeatEn(enabled);
    the_mesh_ptr->savePrefs();
}

bool get_client_repeat() {
    if (!the_mesh_ptr) return false;
    return the_mesh_ptr->getNodePrefs()->isRepeatEn();
}

// ---------- BLE ----------



void ble_enable() {
    if (!the_mesh_ptr) return;
    ble_requested = true;
    Serial.println("BLE: enable requested");
}

void ble_disable() {
    ble_requested = false;
    Serial.println("BLE: disable requested");
}

bool ble_is_enabled() { return ble_active; }

bool ble_is_requested() { return ble_requested; }

void set_ble_pin(uint32_t pin) {
    if (!the_mesh_ptr) return;
    the_mesh_ptr->getNodePrefs()->ble_pin = pin;
    the_mesh_ptr->savePrefs();
}

uint32_t get_ble_pin() {
    if (!the_mesh_ptr) return BLE_PIN_CODE;
    uint32_t pin = the_mesh_ptr->getNodePrefs()->ble_pin;
    return pin > 0 ? pin : BLE_PIN_CODE;
}

// ---------- Sleep ----------

void enter_sleep(uint32_t wake_secs) {
    if (!mesh_mutex) return;

    // Grab the mesh mutex so mesh task stops looping
    if (xSemaphoreTake(mesh_mutex, pdMS_TO_TICKS(2000))) {
        // Wait for any active radio transaction to finish
        int wait = 0;
        while (radio_driver.isReceiving() && wait < 100) {
            vTaskDelay(pdMS_TO_TICKS(10));
            wait++;
        }

        Serial.println("MESH: entering light sleep");
        Serial.flush();
        vTaskDelay(pdMS_TO_TICKS(50));

        // Configure wake on LoRa DIO1 (incoming packet) + touch INT
        esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
        uint64_t wake_mask = (1ULL << P_LORA_DIO_1) | (1ULL << BOARD_TOUCH_INT);
        esp_sleep_enable_ext1_wakeup(wake_mask, ESP_EXT1_WAKEUP_ANY_HIGH);

        if (wake_secs > 0) {
            esp_sleep_enable_timer_wakeup((uint64_t)wake_secs * 1000000ULL);
        }

        esp_light_sleep_start();

        // Woke up
        Serial.println("MESH: woke from sleep");
        xSemaphoreGive(mesh_mutex);
    }
}

// ---------- Discovery ----------

int get_discovered(DiscoveredNode* dest, int max_num) {
    if (!the_mesh_ptr || !mesh_mutex) return 0;
    int count = 0;
    if (xSemaphoreTake(mesh_mutex, pdMS_TO_TICKS(500))) {
        AdvertPath paths[16];
        int n = the_mesh_ptr->getRecentlyHeard(paths, max_num < 16 ? max_num : 16);
        for (int i = 0; i < n; i++) {
            if (paths[i].recv_timestamp == 0) continue; // empty slot
            strncpy(dest[count].name, paths[i].name, sizeof(dest[count].name) - 1);
            dest[count].name[31] = 0;
            memcpy(dest[count].pubkey_prefix, paths[i].pubkey_prefix, 7);
            dest[count].path_len = paths[i].path_len;
            dest[count].recv_timestamp = paths[i].recv_timestamp;
            count++;
        }
        xSemaphoreGive(mesh_mutex);
    }
    return count;
}

bool add_contact_by_prefix(const uint8_t* pubkey_prefix) {
    if (!the_mesh_ptr || !mesh_mutex || !store) return false;
    bool ok = false;
    Serial.printf("MESH: add_contact_by_prefix [%02X%02X%02X%02X%02X%02X%02X]\n",
        pubkey_prefix[0], pubkey_prefix[1], pubkey_prefix[2], pubkey_prefix[3],
        pubkey_prefix[4], pubkey_prefix[5], pubkey_prefix[6]);

    if (xSemaphoreTake(mesh_mutex, pdMS_TO_TICKS(500))) {
        // Check if already a contact
        ContactInfo* existing = the_mesh_ptr->lookupContactByPubKey(pubkey_prefix, 7);
        if (existing) {
            Serial.printf("MESH: already a contact: %s\n", existing->name);
            ok = true;
        } else {
            // Reprocess the saved signed advert with auto-add enabled only
            // inside this guarded call. Never alter the global auto-add choice.
            uint8_t blob[256];
            uint8_t len = store->getBlobByKey(pubkey_prefix, 7, blob);
            Serial.printf("MESH: blob lookup len=%d\n", len);
            if (len > 0) {
                uint8_t saved_manual = the_mesh_ptr->getNodePrefs()->manual_add_contacts;
                the_mesh_ptr->getNodePrefs()->manual_add_contacts &= ~uint8_t(1);
                bool imported = the_mesh_ptr->importContact(blob, len);
                Serial.printf("MESH: importContact result=%d\n", imported);
                if (imported) {
                    the_mesh_ptr->loop();
                }
                the_mesh_ptr->getNodePrefs()->manual_add_contacts = saved_manual;
                // A parsed packet alone is not success: a real contact must
                // exist before updating the UI or persisting the contact list.
                ok = the_mesh_ptr->lookupContactByPubKey(pubkey_prefix, 7) != nullptr;
                Serial.printf("MESH: after loop, contact found=%d\n", ok);
                if (ok) store->saveContacts(the_mesh_ptr);
            }
        }
        xSemaphoreGive(mesh_mutex);
    }
    return ok;
}

bool is_contact(const uint8_t* pubkey_prefix) {
    if (!the_mesh_ptr || !mesh_mutex) return false;
    bool found = false;
    if (xSemaphoreTake(mesh_mutex, pdMS_TO_TICKS(500))) {
        found = (the_mesh_ptr->lookupContactByPubKey(pubkey_prefix, 7) != NULL);
        xSemaphoreGive(mesh_mutex);
    }
    return found;
}

bool remove_contact_by_prefix(const uint8_t* pubkey_prefix) {
    if (!the_mesh_ptr || !mesh_mutex) return false;
    bool ok = false;
    if (xSemaphoreTake(mesh_mutex, pdMS_TO_TICKS(500))) {
        ContactInfo* c = the_mesh_ptr->lookupContactByPubKey(pubkey_prefix, 7);
        if (c) {
            ok = the_mesh_ptr->removeContact(*c);
            if (ok) Serial.println("MESH: contact removed");
        }
        xSemaphoreGive(mesh_mutex);
    }
    return ok;
}

bool is_favorite(const uint8_t* pubkey_prefix) {
    if (!the_mesh_ptr || !mesh_mutex) return false;
    bool is_fav = false;
    if (xSemaphoreTake(mesh_mutex, pdMS_TO_TICKS(500))) {
        ContactInfo* c = the_mesh_ptr->lookupContactByPubKey(pubkey_prefix, 7);
        if (c) {
            is_fav = (c->flags & 0x01) != 0;
        }
        xSemaphoreGive(mesh_mutex);
    }
    return is_fav;
}

bool toggle_favorite(const uint8_t* pubkey_prefix) {
    if (!the_mesh_ptr || !mesh_mutex) return false;
    bool is_fav = false;
    if (xSemaphoreTake(mesh_mutex, pdMS_TO_TICKS(500))) {
        ContactInfo* c = the_mesh_ptr->lookupContactByPubKey(pubkey_prefix, 7);
        if (c) {
            c->flags ^= 0x01;  // toggle bit 0
            is_fav = (c->flags & 0x01) != 0;
            if (store) store->saveContacts(the_mesh_ptr);
        }
        xSemaphoreGive(mesh_mutex);
    }
    return is_fav;
}

bool request_telemetry(const uint8_t* pubkey_prefix, bool force_flood) {
    if (!the_mesh_ptr || !mesh_mutex || !pubkey_prefix) return false;
    bool ok = false;
    if (xSemaphoreTake(mesh_mutex, pdMS_TO_TICKS(500))) {
        ContactInfo* c = the_mesh_ptr->lookupContactByPubKey(pubkey_prefix, 7);
        if (c) {
            uint32_t est_timeout = 0;
            ok = the_mesh_ptr->requestTelemetry(*c, est_timeout, force_flood);
        }
        xSemaphoreGive(mesh_mutex);
    }
    return ok;
}

bool request_trace_ping(const uint8_t* pubkey_prefix, uint32_t* out_tag) {
    if (!the_mesh_ptr || !mesh_mutex || !pubkey_prefix) return false;
    bool ok = false;
    if (xSemaphoreTake(mesh_mutex, pdMS_TO_TICKS(500))) {
        ContactInfo* c = the_mesh_ptr->lookupContactByPubKey(pubkey_prefix, 7);
        if (c) {
            uint32_t tag = 0;
            uint32_t est_timeout = 0;
            ok = the_mesh_ptr->requestTrace(*c, tag, est_timeout);
            if (ok && out_tag) {
                *out_tag = tag;
            }
        }
        xSemaphoreGive(mesh_mutex);
    }
    return ok;
}

void clear_contacts() {
    if (!the_mesh_ptr || !mesh_mutex || !store) return;
    if (xSemaphoreTake(mesh_mutex, pdMS_TO_TICKS(500))) {
        // Remove all contacts one by one
        ContactsIterator iter = the_mesh_ptr->startContactsIterator();
        ContactInfo c;
        while (iter.hasNext(the_mesh_ptr, c)) {
            the_mesh_ptr->removeContact(c);
            iter = the_mesh_ptr->startContactsIterator(); // restart after removal
        }
        store->saveContacts(the_mesh_ptr);
        Serial.println("MESH: all contacts cleared");
        xSemaphoreGive(mesh_mutex);
    }
}

void clear_channels() {
    if (!the_mesh_ptr || !mesh_mutex || !store) return;
    if (xSemaphoreTake(mesh_mutex, pdMS_TO_TICKS(500))) {
        // Clear all channel slots
        ChannelDetails empty = {};
        for (int i = 0; i < 8; i++) {
            the_mesh_ptr->setChannel(i, empty);
        }
        store->saveChannels(the_mesh_ptr);
        Serial.println("MESH: all channels cleared");
        xSemaphoreGive(mesh_mutex);
    }
}

void flush_storage() {
    if (!mesh_mutex) return;
    if (xSemaphoreTake(mesh_mutex, pdMS_TO_TICKS(2000))) {
        sd_log::flush();
        xSemaphoreGive(mesh_mutex);
    }
}

// ---------- Contact sync ----------

void push_all_contacts() {
    if (!the_mesh_ptr || !mesh_mutex) return;
    if (xSemaphoreTake(mesh_mutex, pdMS_TO_TICKS(500))) {
        ContactsIterator iter = the_mesh_ptr->startContactsIterator();
        ContactInfo c;
        while (iter.hasNext(the_mesh_ptr, c)) {
            mesh::bridge::ContactUpdate cu = {};
            strncpy(cu.name, c.name, sizeof(cu.name) - 1);
            memcpy(cu.pub_key, c.id.pub_key, 32);
            cu.type = c.type;
            cu.flags = c.flags;
            cu.gps_lat = c.gps_lat;
            cu.gps_lon = c.gps_lon;
            cu.path_len = c.out_path_len;
            cu.is_new = false;
            mesh::bridge::push_contact(cu);
        }
        xSemaphoreGive(mesh_mutex);
    }
}

int get_channels(ChannelEntry* dest, int max_num) {
    if (!dest || max_num <= 0 || !the_mesh_ptr || !mesh_mutex) return 0;
    int count = 0;
    if (xSemaphoreTake(mesh_mutex, pdMS_TO_TICKS(500))) {
        for (int i = 0; i < MAX_GROUP_CHANNELS && count < max_num; i++) {
            ChannelDetails ch;
            if (!the_mesh_ptr->getChannel(i, ch) || !ch.name[0]) {
                continue;
            }
            dest[count].idx = (uint8_t)i;
            strncpy(dest[count].name, ch.name, sizeof(dest[count].name) - 1);
            dest[count].name[sizeof(dest[count].name) - 1] = 0;
            count++;
        }
        xSemaphoreGive(mesh_mutex);
    }
    return count;
}

} // namespace mesh::task
