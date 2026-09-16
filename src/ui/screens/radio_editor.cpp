#ifdef BOARD_EPAPER
#include <Arduino.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include "radio_editor.h"
#include "../screen_ids.h"
#include "../kit/ui_kit.h"
#include "../components/toast.h"
#include "../../mesh/mesh_task.h"

namespace ui::screen::radio_editor {
using namespace ui::kit;

// This draft changes no radio settings until Save & Restart. In particular,
// tapping a list entry cannot silently move the running mesh to another band.
struct Draft {
    float freq, bw;
    uint8_t sf, cr, hash_mode;
    int8_t power;
};
static Draft draft = {};
static Handle values[6] = {};
enum ChoiceField : uint8_t { BW, SF, CR, PATH_BYTES, POWER };
static ChoiceField choice_field = BW;

static constexpr float bw_values[] = {31.25f, 62.5f, 125, 250, 500};
static constexpr uint8_t sf_values[] = {5, 6, 7, 8, 9, 10, 11, 12};
static constexpr uint8_t cr_values[] = {5, 6, 7, 8};
static constexpr int8_t power_values[] = {-9, 2, 5, 10, 14, 17, 20, 22};

static void refresh_values() {
    char buf[48];
    if (values[0]) { snprintf(buf, sizeof(buf), "%.3f MHz", draft.freq); set_text(values[0], buf); }
    if (values[1]) { snprintf(buf, sizeof(buf), "%g kHz", draft.bw); set_text(values[1], buf); }
    if (values[2]) { snprintf(buf, sizeof(buf), "SF%u", draft.sf); set_text(values[2], buf); }
    if (values[3]) { snprintf(buf, sizeof(buf), "4/%u", draft.cr); set_text(values[3], buf); }
    if (values[4]) { snprintf(buf, sizeof(buf), "%uB", draft.hash_mode + 1); set_text(values[4], buf); }
    if (values[5]) { snprintf(buf, sizeof(buf), "%d dBm", draft.power); set_text(values[5], buf); }
}

void open() {
    draft = {mesh::task::get_freq(), mesh::task::get_bw(), mesh::task::get_sf(),
             mesh::task::get_cr(), mesh::task::get_path_hash_mode(), mesh::task::get_tx_power()};
    ui::screen_mgr::push(SCREEN_RADIO_EDITOR, false);
}

static void frequency_finished(const char* text, void*) {
    if (!text) return;  // Cancel leaves the draft unchanged.
    char* end = nullptr;
    float value = strtof(text, &end);
    // The companion app encodes MHz to integer kHz, so accept at most three
    // meaningful decimal places rather than silently rounding another value.
    if (end == text || *end || !std::isfinite(value) || value < 150 || value > 960 ||
        fabsf(value * 1000 - roundf(value * 1000)) > 0.01f) {
        ui::toast::show("Use 150-960 MHz, 3 decimals");
        return;
    }
    draft.freq = roundf(value * 1000) / 1000;
    refresh_values();
}

static void edit_frequency(void*) {
    char buf[24];
    snprintf(buf, sizeof(buf), "%.3f", draft.freq);
    keyboard_open_numeric(buf, 12, frequency_finished, nullptr);
}

static void open_choices(void* user) {
    choice_field = (ChoiceField)(intptr_t)user;
    ui::screen_mgr::push(SCREEN_RADIO_CHOICE, false);
}

static void save_and_restart(void*) {
    if (!mesh::task::save_radio_profile(draft.freq, draft.bw, draft.sf, draft.cr,
                                        draft.hash_mode, draft.power)) {
        ui::toast::show("Radio settings not saved");
        return;
    }
    Serial.printf("RADIO: saving %.3f/BW%g/SF%u/CR%u/%uB/%ddBm; restarting\n",
                  draft.freq, draft.bw, draft.sf, draft.cr, draft.hash_mode + 1, draft.power);
    delay(100);
    ESP.restart();
}

static void add_field(Handle list_area, const char* label_text, int slot,
                      ChoiceField field) {
    menu_row(list_area, label_text, open_choices, (void*)(intptr_t)field);
    values[slot] = label(list_area, "");
    font(values[slot], Font::Small);
}

static void create_editor(Handle parent) {
    Handle items = list(parent);
    Handle note = label(items, "Edit values, then Save & Restart.\n"
                               "Cancel keeps the current radio profile.");
    font(note, Font::Small);
    size(note, pct(95), CONTENT);
    long_mode(note, LongMode::Wrap);

    menu_row(items, "Freq", edit_frequency, nullptr);
    values[0] = label(items, "");
    font(values[0], Font::Small);
    add_field(items, "BW", 1, BW);
    add_field(items, "SF", 2, SF);
    add_field(items, "CR", 3, CR);
    add_field(items, "Path", 4, PATH_BYTES);
    add_field(items, "TX power", 5, POWER);
    menu_row(items, "Save & Restart", save_and_restart, nullptr);
    refresh_values();
}

static void chosen(void* user) {
    int index = (int)(intptr_t)user;
    switch (choice_field) {
        case BW:         draft.bw = bw_values[index]; break;
        case SF:         draft.sf = sf_values[index]; break;
        case CR:         draft.cr = cr_values[index]; break;
        case PATH_BYTES: draft.hash_mode = (uint8_t)index; break;
        case POWER:      draft.power = power_values[index]; break;
    }
    ui::screen_mgr::pop(false);
}

static void create_choices(Handle parent) {
    Handle items = list(parent);
    int count = 0;
    switch (choice_field) {
        case BW: count = sizeof(bw_values) / sizeof(bw_values[0]); break;
        case SF: count = sizeof(sf_values) / sizeof(sf_values[0]); break;
        case CR: count = sizeof(cr_values) / sizeof(cr_values[0]); break;
        case PATH_BYTES: count = 3; break;
        case POWER: count = sizeof(power_values) / sizeof(power_values[0]); break;
    }
    for (int i = 0; i < count; ++i) {
        char text[32];
        switch (choice_field) {
            case BW: snprintf(text, sizeof(text), "%g kHz", bw_values[i]); break;
            case SF: snprintf(text, sizeof(text), "SF%u", sf_values[i]); break;
            case CR: snprintf(text, sizeof(text), "4/%u", cr_values[i]); break;
            case PATH_BYTES: snprintf(text, sizeof(text), "%dB path", i + 1); break;
            case POWER: snprintf(text, sizeof(text), "%d dBm", power_values[i]); break;
        }
        menu_row(items, text, chosen, (void*)(intptr_t)i);
    }
}

static void editor_entry() { refresh_values(); }
static void noop() {}
static void editor_destroy() { for (Handle& value : values) value = nullptr; }
screen_lifecycle_t lifecycle = {create_editor, editor_entry, noop, editor_destroy};
screen_lifecycle_t choice_lifecycle = {create_choices, noop, noop, noop};
} // namespace ui::screen::radio_editor
#endif
