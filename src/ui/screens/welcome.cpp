#ifdef BOARD_EPAPER
#include <Arduino.h>
#include <Preferences.h>
#include <cstdio>
#include <cstdint>
#include "welcome.h"
#include "../screen_ids.h"
#include "../kit/ui_kit.h"
#include "../ui_screen_mgr.h"
#include "../../mesh/mesh_task.h"

namespace ui::screen::welcome {
using namespace ui::kit;

// Presets intentionally carry exact radio parameters. A frequency alone is
// not enough to join an existing mesh: bandwidth, SF and CR must also match.
struct RadioPreset { const char* label; float freq, bw; uint8_t sf, cr; const char* extra; };
static const RadioPreset choices[] = {
    {"Keep current", 0, 0, 0, 0, ""},
    {"Australia", 915.800f, 250, 10, 5, ""},
    {"Australia (Narrow)", 916.575f, 62.5f, 7, 8, ""},
    {"Australia (Mid)", 915.075f, 125, 9, 5, ""},
    {"Australia: SA, WA", 923.125f, 62.5f, 8, 8, ""},
    {"Australia: QLD", 923.125f, 62.5f, 8, 5, ""},
    {"Brazil", 923.125f, 62.5f, 8, 8, ""},
    {"Canada", 910.525f, 62.5f, 7, 5, "3B"},
    {"Costa Rica", 910.525f, 125, 11, 5, ""},
    {"EU/UK (Narrow)", 869.618f, 62.5f, 8, 8, ""},
    {"EU/UK (Deprecated)", 869.525f, 250, 11, 5, ""},
    {"Czech (Narrow)", 869.432f, 62.5f, 7, 5, ""},
    {"EU 433 (Long Range)", 433.650f, 250, 11, 5, ""},
    {"EU 433 (Narrow)", 433.650f, 62.5f, 8, 8, ""},
    {"Hungary", 869.618f, 62.5f, 7, 5, "2B"},
    {"Netherlands", 869.618f, 62.5f, 7, 5, ""},
    {"NL (Limburg)", 869.618f, 62.5f, 8, 8, "2B"},
    {"NZ (Narrow)", 917.375f, 62.5f, 7, 5, "2B"},
    {"NZ (Gisborne)", 917.375f, 250, 11, 5, "1B"},
    {"Portugal 433", 433.375f, 62.5f, 9, 6, ""},
    {"Portugal 868", 869.618f, 62.5f, 7, 6, ""},
    {"Slovakia", 869.618f, 62.5f, 7, 5, "2B"},
    {"Switzerland", 869.618f, 62.5f, 8, 8, ""},
    {"USA", 910.525f, 62.5f, 7, 5, ""},
    {"USA (PhillyMesh)", 902.250f, 500, 11, 5, "2B"},
    {"USA (SoCal)", 927.875f, 62.5f, 7, 5, "3B"},
    {"Vietnam (Narrow)", 920.250f, 62.5f, 8, 5, ""},
    {"Vietnam (Deprecated)", 920.250f, 250, 11, 5, ""},
};
static int selected = 0;
static Handle preset_value = nullptr;
static Handle name_value = nullptr;
static char entered_name[32] = {};
static char preset_summary[85];
static bool opened_from_settings = false;

static const char* preset_detail(int index) {
    const RadioPreset& r = choices[index];
    if (!index) return "No changes to existing radio settings";
    snprintf(preset_summary, sizeof(preset_summary), "%.3f MHz / SF%u / BW%g / CR%u%s%s",
             r.freq, r.sf, r.bw, r.cr, *r.extra ? " / " : "", r.extra);
    return preset_summary;
}

bool needed() {
    Preferences p;
    if (!p.begin("system", true)) return false;
    const bool done = p.getBool("setup_done", false);
    p.end();
    return !done;
}

void open() {
    opened_from_settings = true;
    if (!ui::screen_mgr::push(SCREEN_WELCOME, false)) opened_from_settings = false;
}

static void complete(bool apply) {
    bool radio_changed = false;
    if (apply) {
        if (entered_name[0]) mesh::task::set_node_name(entered_name);
        if (selected > 0) {
            const RadioPreset& r = choices[selected];
            mesh::task::set_freq(r.freq);
            mesh::task::set_bw(r.bw);
            mesh::task::set_sf(r.sf);
            mesh::task::set_cr(r.cr);
            // The app's 2B/3B suffix denotes the path hash byte count. Its
            // companion setting is zero-based; unmarked presets use 1B.
            mesh::task::set_path_hash_mode(*r.extra ? uint8_t(r.extra[0] - '1') : 0);
            radio_changed = true;
        }
    }
    Preferences p;
    if (p.begin("system", false)) {
        p.putBool("setup_done", true);
        p.end();
    }
    // MeshCore applies radio parameters on restart; boot adverts are disabled.
    // Restart only if they changed; skipping leaves the existing profile intact.
    if (radio_changed || (apply && entered_name[0])) {
        Serial.println("WELCOME: saved setup; restarting to apply identity/radio");
        delay(100);
        ESP.restart();
    }
    if (opened_from_settings) {
        opened_from_settings = false;
        ui::screen_mgr::pop(false);
    } else {
        ui::screen_mgr::switch_to(SCREEN_HOME, false);
    }
}

static void on_preset(void*) { ui::screen_mgr::push(SCREEN_WELCOME_PRESETS, false); }

static void choose_preset(void* arg) {
    selected = (int)(intptr_t)arg;
    ui::screen_mgr::pop(false);
}

static void create_presets(Handle parent) {
    Handle items = list(parent);
    for (unsigned i = 0; i < sizeof(choices) / sizeof(choices[0]); ++i) {
        // Separate the title from the small radio details, so the two cannot
        // overlap even for longer region names on the 540-pixel display.
        menu_row(items, choices[i].label, choose_preset, (void*)(intptr_t)i);
        Handle detail = label(items, preset_detail(i));
        font(detail, Font::Small);
        size(detail, pct(95), CONTENT);
        long_mode(detail, LongMode::Wrap);
    }
}

static void name_finished(const char* text, void*) {
    if (!text || !text[0]) return;
    snprintf(entered_name, sizeof(entered_name), "%s", text);
    if (name_value) set_text(name_value, entered_name);
}

static void on_name(void*) {
    keyboard_open(entered_name[0] ? entered_name : mesh::task::node_name(),
                  sizeof(entered_name) - 1, name_finished, nullptr);
}

static void on_continue(void*) { complete(true); }
static void on_skip(void*) { complete(false); }

static void create(Handle parent) {
    Handle list_area = list(parent);
    Handle title = label(list_area, "Welcome to PaperUI");
    font(title, Font::Title);
    Handle note = label(list_area, "Choose a preset and device name.\n"
                                  "Skip to keep current settings.");
    font(note, Font::Small);
    size(note, pct(95), CONTENT);
    long_mode(note, LongMode::Wrap);
    selected = 0;
    entered_name[0] = 0;
    menu_row(list_area, "Preset", on_preset, nullptr);
    preset_value = label(list_area, choices[selected].label);
    font(preset_value, Font::Small);
    name_value = toggle_item(list_area, "Device name", mesh::task::node_name(), on_name, nullptr);
    menu_row(list_area, "Save and continue", on_continue, nullptr);
    menu_row(list_area, "Skip", on_skip, nullptr);
}

static void entry() { if (preset_value) set_text(preset_value, choices[selected].label); }
static void exit_fn() {}
static void destroy() { preset_value = name_value = nullptr; }
screen_lifecycle_t lifecycle = {create, entry, exit_fn, destroy};
static void presets_noop() {}
screen_lifecycle_t presets_lifecycle = {create_presets, presets_noop, presets_noop, presets_noop};
} // namespace ui::screen::welcome
#endif
