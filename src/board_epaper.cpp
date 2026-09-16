#ifdef BOARD_EPAPER

#include "board.h"
#include "model.h"
#include "nvs_param.h"
#include "mesh/companion/target.h"
#include <Preferences.h>
extern "C" int epd_prep_task_priority;

namespace board {

SemaphoreHandle_t i2c_mutex = NULL;

// ---------- Global peripheral instances ----------

bool peri_status[E_PERI_MAX] = {0};
XPowersPPM ppm;
BQ27220 bq27220;
TouchDrvGT911 touch;
SensorPCF8563 rtc;
static constexpr uint8_t CARDKB_I2C_ADDR = 0x5F;
static constexpr uint8_t CARDKB_KEY_UP = 0xB5;
static constexpr uint8_t CARDKB_KEY_DOWN = 0xB6;
static constexpr uint8_t CARDKB_KEY_LEFT = 0xB4;
static constexpr uint8_t CARDKB_KEY_RIGHT = 0xB7;
static constexpr uint8_t CARDKB_KEY_TAB = 0x09;
static constexpr uint8_t CARDKB_KEY_ESC = 0x1B;
static constexpr uint8_t CARDKB_KEY_BS = 0x08;
static constexpr uint8_t CARDKB_KEY_ENTER = 0x0D;
static constexpr uint8_t CARDKB_KEY_DEL = 0x7F;
static constexpr uint8_t CARDKB_KEY_PREV = 0xF2;
static constexpr uint8_t CARDKB_KEY_NEXT = 0xF1;
static constexpr uint8_t CARDKB_KEY_LEFT_NAV = 0xF3;
static constexpr uint8_t CARDKB_KEY_RIGHT_NAV = 0xF4;
static uint32_t cardkb_last_poll = 0;
static uint32_t cardkb_poll_interval = 50;
static uint8_t cardkb_error_count = 0;

// E-paper
#define WAVEFORM EPD_BUILTIN_WAVEFORM
#define DEMO_BOARD epd_board_v7
EpdiyHighlevelState hl;

// Home button
volatile bool home_button_pressed = false;

// Aurora-style single-owner sampling. GT911's status register must be cleared
// after a frame is read; consequently only this sampler may call the driver.
// LVGL, wake detection and diagnostics consume this cached snapshot instead.
static bool touch_cached_pressed = false;
static int16_t touch_cached_x = 0;
static int16_t touch_cached_y = 0;
static uint32_t touch_next_sample_ms = 0;
struct TouchEvent {
    int16_t x;
    int16_t y;
    bool pressed;
};
static constexpr uint8_t TOUCH_EVENT_CAPACITY = 32;
static TouchEvent touch_events[TOUCH_EVENT_CAPACITY];
static uint8_t touch_event_head = 0;
static uint8_t touch_event_count = 0;
static portMUX_TYPE touch_state_mux = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t touch_sampler_task_handle = nullptr;
static volatile bool touch_sampling_enabled = true;

class I2cGuard {
public:
    explicit I2cGuard(TickType_t timeout = pdMS_TO_TICKS(20))
        : locked(!i2c_mutex || xSemaphoreTake(i2c_mutex, timeout) == pdTRUE) {}
    ~I2cGuard() { if (i2c_mutex && locked) xSemaphoreGive(i2c_mutex); }
    explicit operator bool() const { return locked; }
private:
    bool locked;
};

static void queue_touch_event(bool pressed, int16_t x, int16_t y) {
    // The queue is a small static ring: no heap allocation or fragmentation.
    // If LVGL ever falls more than 640 ms behind the 50 Hz sampler, retain the
    // newest motion/release information rather than freezing on an old state.
    portENTER_CRITICAL(&touch_state_mux);
    if (touch_event_count == TOUCH_EVENT_CAPACITY) {
        touch_event_head = (uint8_t)((touch_event_head + 1) % TOUCH_EVENT_CAPACITY);
        touch_event_count--;
    }
    const uint8_t tail = (uint8_t)((touch_event_head + touch_event_count) % TOUCH_EVENT_CAPACITY);
    touch_events[tail] = {.x = x, .y = y, .pressed = pressed};
    touch_event_count++;
    portEXIT_CRITICAL(&touch_state_mux);
}

// GPS is now handled by MeshCore's EnvironmentSensorManager + MicroNMEALocationProvider
// (configured in target.cpp, polled by sensors.loop() in mesh_task.cpp)

// ---------- Detail init functions ----------

namespace detail {

bool screen_init() {
    epd_init(&DEMO_BOARD, &ED047TC1, EPD_LUT_64K);
    epd_set_vcom(nvs_param_get_u16(NVS_ID_EPD_VCOM));
    hl = epd_hl_init(WAVEFORM);
    epd_set_rotation(EPD_ROT_INVERTED_PORTRAIT);

    Serial.printf("Display: %d x %d\n", epd_rotated_display_width(), epd_rotated_display_height());

    epd_set_lcd_pixel_clock_MHz(17);
    epd_poweron();
    epd_clear();
    epd_poweroff();
    return true;
}

bool bq25896_init() {
    bool result = ppm.init(Wire, BOARD_SDA, BOARD_SCL, BQ25896_SLAVE_ADDRESS);
    if (!result) return false;

    ppm.setSysPowerDownVoltage(3300);
    ppm.setInputCurrentLimit(3250);
    ppm.disableCurrentLimitPin();
    ppm.setChargeTargetVoltage(4208);
    ppm.setPrechargeCurr(64);
    ppm.setChargerConstantCurr(1024);
    ppm.enableMeasure();
    ppm.enableCharge();
    ppm.enableOTG();
    ppm.disableOTG();
    return true;
}

bool bq27220_init() {
    return bq27220.init();
}

bool rtc_init() {
    pinMode(BOARD_RTC_IRQ, INPUT_PULLUP);
    if (!rtc.begin(Wire, PCF8563_SLAVE_ADDRESS, BOARD_RTC_SDA, BOARD_RTC_SCL)) {
        Serial.println("Failed to find PCF8563");
        return false;
    }
    return true;
}

bool touch_init() {
    touch.setPins(BOARD_TOUCH_RST, BOARD_TOUCH_INT);
    if (!touch.begin(Wire, GT911_SLAVE_ADDRESS_L, BOARD_SDA, BOARD_SCL)) {
        Serial.println("Failed to find GT911");
        return false;
    }
    Serial.println("GT911 touch init OK");

    // Home button (center touch button on GT911) — sets flag for UI task
    touch.setHomeButtonCallback([](void* user_data) {
        Serial.println("Home button pressed");
        home_button_pressed = true;
    }, NULL);

    touch.setInterruptMode(TouchDrvGT911::LOW_LEVEL_QUERY);
    return true;
}

bool sd_init() {
    if (!SD.begin(BOARD_SD_CS)) {
        Serial.println("SD card mount failed");
        return false;
    }
    if (SD.cardType() == CARD_NONE) {
        Serial.println("No SD card");
        return false;
    }
    return true;
}

bool gps_init() {
    // GPS is now initialized by MeshCore's EnvironmentSensorManager (sensors.begin())
    // via MicroNMEALocationProvider which uses Serial1 with PIN_GPS_TX/RX
    // Just return true — GPS detection happens in sensors.begin()
    return true;
}

bool keyboard_init() {
    Wire.beginTransmission(CARDKB_I2C_ADDR);
    bool ok = (Wire.endTransmission() == 0);
    if (ok) {
        Serial.println("CardKB keyboard found");
    } else {
        Serial.println("CardKB keyboard not found");
    }
    return ok;
}

} // namespace detail

void touch_sample() {
    if (!touch_sampling_enabled) return;
    if (!peri_status[E_PERI_TOUCH]) {
        portENTER_CRITICAL(&touch_state_mux);
        touch_cached_pressed = false;
        portEXIT_CRITICAL(&touch_state_mux);
        return;
    }

    const uint32_t now = millis();
    if ((int32_t)(now - touch_next_sample_ms) < 0) return;
    touch_next_sample_ms = now + 8;  // Aurora/FreeInk cadence: do not miss short taps

    I2cGuard guard(pdMS_TO_TICKS(5));
    if (!guard) return;

    // Poll the status register directly, as Aurora's FreeInk driver does. The
    // SensorLib path clears 0x814E before reading coordinates, allowing the
    // controller to replace a rapid frame in the middle of the transaction.
    auto read_reg = [](uint16_t reg, uint8_t* data, uint8_t len) {
        Wire.beginTransmission(GT911_SLAVE_ADDRESS_L);
        Wire.write((uint8_t)(reg >> 8));
        Wire.write((uint8_t)reg);
        if (Wire.endTransmission(false) != 0) return false;
        if (Wire.requestFrom((uint8_t)GT911_SLAVE_ADDRESS_L, len, (uint8_t)true) != len) {
            while (Wire.available()) Wire.read();
            return false;
        }
        for (uint8_t i = 0; i < len; ++i) data[i] = Wire.read();
        return true;
    };
    auto clear_status = []() {
        Wire.beginTransmission(GT911_SLAVE_ADDRESS_L);
        Wire.write((uint8_t)0x81);
        Wire.write((uint8_t)0x4E);
        Wire.write((uint8_t)0x00);
        return Wire.endTransmission() == 0;
    };

    uint8_t status = 0;
    if (!read_reg(0x814E, &status, 1) || !(status & 0x80)) return;

    if (status & 0x10) home_button_pressed = true;
    const uint8_t count = status & 0x0F;
    if (count == 0 || count > 5) {
        portENTER_CRITICAL(&touch_state_mux);
        const bool was_pressed = touch_cached_pressed;
        touch_cached_pressed = false;
        const int16_t last_x = touch_cached_x;
        const int16_t last_y = touch_cached_y;
        portEXIT_CRITICAL(&touch_state_mux);
        if (was_pressed) queue_touch_event(false, last_x, last_y);
        clear_status();
        return;
    }

    uint8_t point[8] = {};
    // Standard GT911 layout: track id at 0x814F, then little-endian X/Y.
    // Clear data-ready only after the complete record has been captured.
    if (!read_reg(0x814F, point, sizeof(point))) return;
    const int16_t sample_x = (int16_t)(point[1] | ((uint16_t)point[2] << 8));
    const int16_t sample_y = (int16_t)(point[3] | ((uint16_t)point[4] << 8));
    portENTER_CRITICAL(&touch_state_mux);
    touch_cached_x = sample_x;
    touch_cached_y = sample_y;
    touch_cached_pressed = true;
    portEXIT_CRITICAL(&touch_state_mux);
    queue_touch_event(true, sample_x, sample_y);
    clear_status();
}

bool touch_snapshot(int16_t* x, int16_t* y) {
    portENTER_CRITICAL(&touch_state_mux);
    if (x) *x = touch_cached_x;
    if (y) *y = touch_cached_y;
    const bool pressed = touch_cached_pressed;
    portEXIT_CRITICAL(&touch_state_mux);
    return pressed;
}

bool touch_pop_event(bool* pressed, int16_t* x, int16_t* y) {
    portENTER_CRITICAL(&touch_state_mux);
    if (touch_event_count == 0) {
        portEXIT_CRITICAL(&touch_state_mux);
        return false;
    }
    const TouchEvent event = touch_events[touch_event_head];
    touch_event_head = (uint8_t)((touch_event_head + 1) % TOUCH_EVENT_CAPACITY);
    touch_event_count--;
    if (pressed) *pressed = event.pressed;
    if (x) *x = event.x;
    if (y) *y = event.y;
    portEXIT_CRITICAL(&touch_state_mux);
    return true;
}

bool touch_events_pending() {
    portENTER_CRITICAL(&touch_state_mux);
    const bool pending = touch_event_count != 0;
    portEXIT_CRITICAL(&touch_state_mux);
    return pending;
}

static void touch_sampler_task(void*) {
    // Keep digitizer acquisition independent of LVGL rendering. A complete
    // press/release can otherwise occur while LVGL converts a large dirty area.
    TickType_t wake = xTaskGetTickCount();
    for (;;) {
        touch_sample();
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(8));
    }
}

void touch_sampling_enable(bool enabled) {
    portENTER_CRITICAL(&touch_state_mux);
    // Drop queued taps and the held state before switching modes. Otherwise
    // an old press could be replayed when the UI becomes active again.
    touch_event_head = 0;
    touch_event_count = 0;
    touch_cached_pressed = false;
    portEXIT_CRITICAL(&touch_state_mux);
    touch_sampling_enabled = enabled;
}

bool touch_sampler_running() { return touch_sampler_task_handle != nullptr; }

void seed_clock_from_rtc() {
    if (!peri_status[E_PERI_RTC]) return;
    RTC_DateTime dt = rtc.getDateTime();
    Serial.printf("RTC raw: %04d-%02d-%02d %02d:%02d:%02d\n",
        dt.getYear(), dt.getMonth(), dt.getDay(), dt.getHour(), dt.getMinute(), dt.getSecond());
    if (dt.getYear() >= 2020 && dt.getYear() <= 2099) {
        setenv("TZ", "UTC0", 1);
        tzset();
        struct tm t = {};
        t.tm_year = dt.getYear() - 1900;
        t.tm_mon  = dt.getMonth() - 1;
        t.tm_mday = dt.getDay();
        t.tm_hour = dt.getHour();
        t.tm_min  = dt.getMinute();
        t.tm_sec  = dt.getSecond();
        t.tm_isdst = 0;
        time_t epoch = mktime(&t);
        struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
        settimeofday(&tv, NULL);
        Serial.printf("System clock seeded: epoch=%ld\n", (long)epoch);
    }
}

// ---------- Main init ----------

void init() {
    gpio_hold_dis((gpio_num_t)BOARD_TOUCH_RST);
    gpio_hold_dis((gpio_num_t)BOARD_LORA_RST);
    gpio_deep_sleep_hold_dis();

    // Pull all SPI CS lines high before init
    pinMode(BOARD_LORA_CS, OUTPUT);
    digitalWrite(BOARD_LORA_CS, HIGH);
    pinMode(BOARD_SD_CS, OUTPUT);
    digitalWrite(BOARD_SD_CS, HIGH);

    if (BOARD_PCA9535_INT > 0) {
        pinMode(BOARD_PCA9535_INT, INPUT_PULLUP);
    }

    Serial.begin(115200);
    SPI.begin(BOARD_SPI_SCLK, BOARD_SPI_MISO, BOARD_SPI_MOSI);
    pinMode(BOARD_BL_EN, OUTPUT);

    nsv_param_init();
    // When migrating a previously BLE-enabled device, show onboarding in
    // PaperUI first. This changes only the transport selection, never the
    // existing MeshCore identity/radio preferences.
    Preferences onboarding;
    if (onboarding.begin("system", true)) {
        const bool needs_setup = !onboarding.getBool("setup_done", false);
        onboarding.end();
        if (needs_setup && nvs_param_get_u8(NVS_ID_BLE_ENABLED)) {
            nvs_param_set_u8(NVS_ID_BLE_ENABLED, 0);
            Serial.println("WELCOME: first launch selects PaperUI; stored mesh settings kept");
        }
    }
    model::init_messages();

    // I2C first — epdiy reuses the existing driver
    Wire.begin(BOARD_SDA, BOARD_SCL);
    Wire.setTimeOut(50);

    const bool companion_mode = nvs_param_get_u8(NVS_ID_BLE_ENABLED) != 0;
    // Low-priority prep produced EPD_DRAW_EMPTY_LINE_QUEUE on the welcome
    // screen. BLE mode paints its static notice before advertising begins;
    // PaperUI's independent priority-8 sampler still pre-empts priority-7
    // panel preparation, even under heavy LVGL rendering.
    epd_prep_task_priority = 7;
    peri_status[E_PERI_BQ27220] = detail::bq27220_init();
    peri_status[E_PERI_INK_POWER] = detail::screen_init();
    io_extend_lora_gps_power_on(true);
    peri_status[E_PERI_BQ25896] = detail::bq25896_init();
    peri_status[E_PERI_RTC] = detail::rtc_init();

    seed_clock_from_rtc();

    // BLE companion mode has no touchscreen UI. Park GT911 in reset instead
    // of paying for its scanning current and the dedicated sampler stack.
    if (!companion_mode) peri_status[E_PERI_TOUCH] = detail::touch_init();
    else {
        pinMode(BOARD_TOUCH_RST, OUTPUT);
        digitalWrite(BOARD_TOUCH_RST, LOW);
    }
    peri_status[E_PERI_KEYBOARD] = detail::keyboard_init();
    peri_status[E_PERI_SD_CARD] = detail::sd_init();
    peri_status[E_PERI_GPS] = detail::gps_init();

    // I2C mutex for cross-core safety (epdiy on Core 1, mesh/RTC on Core 0)
    i2c_mutex = xSemaphoreCreateMutex();

    // Reserve this small internal-DRAM stack before BLE starts. Priority 8
    // pre-empts both the priority-7 panel prep and priority-5 LVGL work.
    if (!companion_mode && peri_status[E_PERI_TOUCH] &&
        xTaskCreatePinnedToCore(touch_sampler_task, "gt911", 2048, nullptr, 8,
                                &touch_sampler_task_handle, 1) != pdPASS) {
        touch_sampler_task_handle = nullptr;
        Serial.println("TOUCH: sampler task allocation failed; using UI polling");
    }

    Serial.println("Board init complete");
    for (int i = 0; i < E_PERI_MAX; i++) {
        Serial.printf("  %s = %s\n", peri_name(i), peri_status[i] ? "OK" : "FAIL");
    }
    Serial.printf("Free DRAM: %u, Free PSRAM: %u\n",
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

// ---------- Battery ----------

bool battery_is_charging() {
    static bool last = false;
    I2cGuard guard;
    if (guard) last = ppm.isCharging();
    return last;
}

uint16_t battery_percent() {
    static uint16_t last = 0;
    if (!peri_status[E_PERI_BQ27220]) return 0;
    I2cGuard guard;
    if (!guard) return last;
    uint16_t pct = bq27220.getStateOfCharge();
    if (pct <= 100) last = pct;
    return last;
}

uint16_t battery_voltage_mv() {
    static uint16_t last = 0;
    if (!peri_status[E_PERI_BQ27220]) return 0;
    I2cGuard guard;
    if (guard) last = bq27220.getVoltage();
    return last;
}

int16_t battery_current_ma() {
    static int16_t last = 0;
    if (!peri_status[E_PERI_BQ27220]) return 0;
    I2cGuard guard;
    if (guard) last = bq27220.getCurrent();
    return last;
}

uint16_t battery_temperature() {
    static uint16_t last = 0;
    if (!peri_status[E_PERI_BQ27220]) return 0;
    I2cGuard guard;
    if (guard) last = bq27220.getTemperature();
    return last;
}

uint16_t battery_full_capacity() {
    static uint16_t last = 0;
    if (!peri_status[E_PERI_BQ27220]) return 0;
    I2cGuard guard;
    if (guard) last = bq27220.getFullChargeCapacity();
    return last;
}

uint16_t battery_design_capacity() {
    static uint16_t last = 0;
    if (!peri_status[E_PERI_BQ27220]) return 0;
    I2cGuard guard;
    if (guard) last = bq27220.getDesignCapacity();
    return last;
}

uint16_t battery_remain_capacity() {
    static uint16_t last = 0;
    if (!peri_status[E_PERI_BQ27220]) return 0;
    I2cGuard guard;
    if (guard) last = bq27220.getRemainingCapacity();
    return last;
}

uint16_t battery_health() {
    static uint16_t last = 0;
    if (!peri_status[E_PERI_BQ27220]) return 0;
    I2cGuard guard;
    if (guard) last = bq27220.getStateOfHealth();
    return last;
}

// BQ25896 charger
bool charger_is_valid() { return peri_status[E_PERI_BQ25896]; }
bool charger_vbus_in() { I2cGuard g; return g ? ppm.isVbusIn() : false; }
const char* charger_status_str() { I2cGuard g; return g ? ppm.getChargeStatusString() : "Unavailable"; }
const char* charger_bus_status_str() { I2cGuard g; return g ? ppm.getBusStatusString() : "Unavailable"; }
const char* charger_ntc_status_str() { I2cGuard g; return g ? ppm.getNTCStatusString() : "Unavailable"; }
float charger_vbus_v() { I2cGuard g; return g ? ppm.getVbusVoltage() / 1000.0f : 0; }
float charger_vsys_v() { I2cGuard g; return g ? ppm.getSystemVoltage() / 1000.0f : 0; }
float charger_vbat_v() { I2cGuard g; return g ? ppm.getBattVoltage() / 1000.0f : 0; }
float charger_target_v() { I2cGuard g; return g ? ppm.getChargeTargetVoltage() / 1000.0f : 0; }
float charger_current_ma() { I2cGuard g; return g ? ppm.getChargeCurrent() : 0; }
float charger_prechrg_ma() { I2cGuard g; return g ? ppm.getPrechargeCurr() : 0; }

// ---------- GPS ----------

void gps_get_coord(double* lat, double* lng) {
    *lat = 0; *lng = 0; // GPS now via MeshCore sensors
}

uint32_t gps_satellites() {
    return 0; // GPS now via MeshCore sensors
}

// ---------- RTC ----------

void rtc_get_time(uint8_t* h, uint8_t* m, uint8_t* s) {
    I2cGuard guard;
    if (!guard) return;
    RTC_DateTime dt = rtc.getDateTime();
    *h = dt.getHour();
    *m = dt.getMinute();
    *s = dt.getSecond();
}

void rtc_get_date(uint8_t* year, uint8_t* month, uint8_t* day, uint8_t* week) {
    I2cGuard guard;
    if (!guard) return;
    RTC_DateTime dt = rtc.getDateTime();
    if (year) *year = dt.getYear() % 100;
    if (month) *month = dt.getMonth();
    if (day) *day = dt.getDay();
    if (week) *week = 0;
}

void rtc_set_datetime(uint16_t year, uint8_t month, uint8_t day,
                      uint8_t hour, uint8_t minute, uint8_t second) {
    I2cGuard guard;
    if (guard) rtc.setDateTime(year, month, day, hour, minute, second);
}

// ---------- Keyboard ----------

int keyboard_read_char() {
    if (!peri_status[E_PERI_KEYBOARD]) return -1;

    uint32_t now = millis();
    if (now - cardkb_last_poll < cardkb_poll_interval) return -1;
    cardkb_last_poll = now;

    if (i2c_mutex && xSemaphoreTake(i2c_mutex, pdMS_TO_TICKS(5)) != pdTRUE) {
        return -1;
    }

    Wire.requestFrom((uint8_t)CARDKB_I2C_ADDR, (uint8_t)1);
    bool available = Wire.available();
    uint8_t raw = available ? Wire.read() : 0;

    if (i2c_mutex) {
        xSemaphoreGive(i2c_mutex);
    }

    if (!available) {
        cardkb_error_count++;
        if (cardkb_error_count >= 3) {
            Wire.begin(BOARD_SDA, BOARD_SCL);
            Wire.setTimeOut(50);
            cardkb_poll_interval = 500;
            cardkb_error_count = 0;
            Serial.println("CardKB I2C recovery");
        }
        return -1;
    }

    cardkb_error_count = 0;
    cardkb_poll_interval = 50;

    if (raw == 0) return -1;

    switch (raw) {
        case CARDKB_KEY_UP: return CARDKB_KEY_PREV;
        case CARDKB_KEY_DOWN: return CARDKB_KEY_NEXT;
        case CARDKB_KEY_LEFT: return CARDKB_KEY_LEFT_NAV;
        case CARDKB_KEY_RIGHT: return CARDKB_KEY_RIGHT_NAV;
        case CARDKB_KEY_ENTER: return '\r';
        case CARDKB_KEY_BS: return '\b';
        case CARDKB_KEY_DEL: return '\b';
        case CARDKB_KEY_ESC: return 0x1B;
        case CARDKB_KEY_TAB: return 0x09;
        default:
            if (raw >= 0x20 && raw <= 0x7E) {
                return (int)raw;
            }
            return -1;
    }
}

void keyboard_set_backlight(uint8_t level) {
    (void)level;
}

uint8_t keyboard_get_backlight() {
    return 0;
}

} // namespace board

#endif // BOARD_EPAPER
