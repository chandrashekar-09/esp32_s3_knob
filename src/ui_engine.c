#include "ui_engine.h"

#include <stdio.h>
#include <string.h>

#if defined(__has_include)
#if __has_include("lvgl.h")
#include "lvgl.h"
#define UI_HAS_LVGL 1
#else
#define UI_HAS_LVGL 0
typedef struct _lv_obj_t lv_obj_t;
#endif
#else
#define UI_HAS_LVGL 0
typedef struct _lv_obj_t lv_obj_t;
#endif

#define UI_ACCENT_HEX 0xFF7520
#define UI_TRACK_HEX 0x181818
#define UI_TEXT_DIM_HEX 0x555555
#define UI_TEXT_FAINT_HEX 0x222222

typedef struct {
    phase_t phase;
    bool mesh_connected;
    bool mesh_root;
    int mesh_layer;
    char ota_status[32];
    bool dirty;
} ui_state_t;

static ui_state_t s_ui = {
    .phase = PH_BOOT,
    .mesh_connected = false,
    .mesh_root = false,
    .mesh_layer = -1,
    .ota_status = "OTA WAITING",
    .dirty = true,
};

static portMUX_TYPE s_ui_mux = portMUX_INITIALIZER_UNLOCKED;
SemaphoreHandle_t xGuiSemaphore = NULL;

static void copy_state(ui_state_t *out)
{
    portENTER_CRITICAL(&s_ui_mux);
    *out = s_ui;
    s_ui.dirty = false;
    portEXIT_CRITICAL(&s_ui_mux);
}

static bool consume_dirty(void)
{
    bool dirty = false;
    portENTER_CRITICAL(&s_ui_mux);
    dirty = s_ui.dirty;
    portEXIT_CRITICAL(&s_ui_mux);
    return dirty;
}

#if UI_HAS_LVGL
static lv_obj_t *s_screen = NULL;
static lv_obj_t *s_wordmark = NULL;
static lv_obj_t *s_status = NULL;
static lv_obj_t *s_center = NULL;
static lv_obj_t *s_level = NULL;
static lv_obj_t *s_hint = NULL;
static lv_obj_t *s_ota = NULL;
static lv_obj_t *s_rings[4] = {};

static uint32_t color_for_level(uint8_t level)
{
    const taxonomy_entry_t *entry = phase_manager_get_taxonomy(level);
    return entry ? entry->color_hex : UI_ACCENT_HEX;
}

static const char *label_for_level(uint8_t level)
{
    const taxonomy_entry_t *entry = phase_manager_get_taxonomy(level);
    return entry ? entry->label : "QUEUE";
}

static void arc_setup(lv_obj_t *arc, int size, int width)
{
    lv_obj_set_size(arc, size, size);
    lv_obj_center(arc);
    lv_arc_set_range(arc, 0, 8);
    lv_arc_set_value(arc, 0);
    lv_arc_set_bg_angles(arc, 0, 360);
    lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(arc, width, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, width, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, lv_color_hex(UI_TRACK_HEX), LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, lv_color_hex(UI_ACCENT_HEX), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_pad_all(arc, 0, LV_PART_KNOB);
}

static lv_obj_t *make_label(const char *text, lv_align_t align, int x, int y, uint32_t color, int letter_space)
{
    lv_obj_t *label = lv_label_create(s_screen);
    lv_label_set_text(label, text);
    lv_obj_align(label, align, x, y);
    lv_obj_set_style_text_color(label, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(label, letter_space, LV_PART_MAIN);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    return label;
}

static void ui_build(void)
{
    s_screen = lv_scr_act();
    lv_obj_clean(s_screen);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, LV_PART_MAIN);

    s_rings[0] = lv_arc_create(s_screen);
    s_rings[1] = lv_arc_create(s_screen);
    s_rings[2] = lv_arc_create(s_screen);
    s_rings[3] = lv_arc_create(s_screen);
    arc_setup(s_rings[0], 336, 7);
    arc_setup(s_rings[1], 310, 5);
    arc_setup(s_rings[2], 288, 4);
    arc_setup(s_rings[3], 266, 3);

    s_wordmark = make_label("QUESORT", LV_ALIGN_TOP_MID, 0, 14, UI_ACCENT_HEX, 4);
    s_status = make_label("MESH SEARCH", LV_ALIGN_TOP_MID, 0, 36, UI_TEXT_DIM_HEX, 2);
    s_center = make_label("Q1", LV_ALIGN_CENTER, 0, -12, 0xFFFFFF, 2);
    s_level = make_label("EMPTY", LV_ALIGN_CENTER, 0, 18, UI_ACCENT_HEX, 3);
    s_hint = make_label("ROTATE QUEUE  HOLD ADMIN", LV_ALIGN_BOTTOM_MID, 0, -38, UI_TEXT_FAINT_HEX, 1);
    s_ota = make_label("OTA WAITING", LV_ALIGN_BOTTOM_MID, 0, -18, UI_TEXT_DIM_HEX, 1);
}

static void ui_apply_home(const ui_state_t *state)
{
    uint8_t level = phase_manager_get_queue_level();
    uint32_t color = color_for_level(level);

    lv_label_set_text_fmt(s_center, "Q%u", (unsigned)level);
    lv_label_set_text(s_level, label_for_level(level));
    lv_obj_set_style_text_color(s_level, lv_color_hex(color), LV_PART_MAIN);

    const uint8_t peer_values[4] = {
        level,
        (uint8_t)((level + 2) > 8 ? 8 : level + 2),
        (uint8_t)(level > 2 ? level - 2 : 1),
        1,
    };
    const uint32_t ring_colors[4] = {
        color,
        0x00AAFF,
        0x22C55E,
        UI_ACCENT_HEX,
    };

    for (int i = 0; i < 4; ++i) {
        lv_arc_set_value(s_rings[i], peer_values[i]);
        lv_obj_set_style_arc_color(s_rings[i], lv_color_hex(ring_colors[i]), LV_PART_INDICATOR);
    }

    lv_label_set_text(s_hint, "ROTATE QUEUE  HOLD ADMIN");
    lv_obj_set_style_text_color(s_center, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_color(s_hint, lv_color_hex(UI_TEXT_FAINT_HEX), LV_PART_MAIN);
    (void)state;
}

static void ui_apply_admin(const ui_state_t *state)
{
    for (int i = 0; i < 4; ++i) {
        lv_arc_set_value(s_rings[i], 8);
        lv_obj_set_style_arc_color(s_rings[i], lv_color_hex(i == 0 ? UI_ACCENT_HEX : UI_TRACK_HEX), LV_PART_INDICATOR);
    }

    lv_label_set_text(s_center, "ADMIN");
    lv_label_set_text_fmt(s_level, "%s  L%d",
                          state->mesh_root ? "ROOT" : "NODE",
                          state->mesh_layer);
    lv_obj_set_style_text_color(s_level, lv_color_hex(UI_ACCENT_HEX), LV_PART_MAIN);
    lv_label_set_text(s_hint, "TAP EXIT  OTA AUTO");
}

static void ui_apply_sleep(void)
{
    for (int i = 0; i < 4; ++i) {
        lv_arc_set_value(s_rings[i], 0);
    }
    lv_label_set_text(s_center, "SLEEP");
    lv_label_set_text(s_level, "TAP TO WAKE");
    lv_obj_set_style_text_color(s_level, lv_color_hex(UI_TEXT_DIM_HEX), LV_PART_MAIN);
    lv_label_set_text(s_hint, "LOW POWER");
}

static void ui_apply_boot(void)
{
    for (int i = 0; i < 4; ++i) {
        lv_arc_set_value(s_rings[i], i + 2);
        lv_obj_set_style_arc_color(s_rings[i], lv_color_hex(i == 0 ? UI_ACCENT_HEX : UI_TRACK_HEX), LV_PART_INDICATOR);
    }
    lv_label_set_text(s_center, "START");
    lv_label_set_text(s_level, "PLEASE WAIT");
    lv_obj_set_style_text_color(s_level, lv_color_hex(UI_TEXT_DIM_HEX), LV_PART_MAIN);
    lv_label_set_text(s_hint, "BOOTING");
}

static void ui_apply_state(const ui_state_t *state)
{
    if (state->mesh_connected) {
        lv_label_set_text_fmt(s_status, "%s  L%d",
                              state->mesh_root ? "MESH ROOT" : "MESH NODE",
                              state->mesh_layer);
        lv_obj_set_style_text_color(s_status, lv_color_hex(state->mesh_root ? UI_ACCENT_HEX : 0x00AAFF), LV_PART_MAIN);
    } else {
        lv_label_set_text(s_status, "MESH SEARCH");
        lv_obj_set_style_text_color(s_status, lv_color_hex(UI_TEXT_DIM_HEX), LV_PART_MAIN);
    }

    lv_label_set_text(s_ota, state->ota_status);

    switch (state->phase) {
    case PH_ADMIN:
        ui_apply_admin(state);
        break;
    case PH_SLEEP:
    case PH_OFF:
        ui_apply_sleep();
        break;
    case PH_BOOT:
        ui_apply_boot();
        break;
    case PH_HOME:
    default:
        ui_apply_home(state);
        break;
    }
}
#endif

void ui_engine_init(void)
{
    if (!xGuiSemaphore) {
        xGuiSemaphore = xSemaphoreCreateMutex();
    }

    ui_engine_set_phase(phase_manager_get_phase());

#if UI_HAS_LVGL
    ui_build();
    ui_state_t snapshot = {};
    copy_state(&snapshot);
    ui_apply_state(&snapshot);
#endif
}

void ui_engine_set_phase(phase_t phase)
{
    portENTER_CRITICAL(&s_ui_mux);
    s_ui.phase = phase;
    s_ui.dirty = true;
    portEXIT_CRITICAL(&s_ui_mux);
}

void ui_engine_set_mesh_state(bool connected, bool root, int layer)
{
    portENTER_CRITICAL(&s_ui_mux);
    s_ui.mesh_connected = connected;
    s_ui.mesh_root = root;
    s_ui.mesh_layer = layer;
    s_ui.dirty = true;
    portEXIT_CRITICAL(&s_ui_mux);
}

void ui_engine_set_ota_status(const char *status)
{
    portENTER_CRITICAL(&s_ui_mux);
    snprintf(s_ui.ota_status, sizeof(s_ui.ota_status), "%s", status ? status : "OTA IDLE");
    s_ui.dirty = true;
    portEXIT_CRITICAL(&s_ui_mux);
}

void ui_engine_render(void)
{
#if UI_HAS_LVGL
    if (consume_dirty()) {
        ui_state_t snapshot = {};
        copy_state(&snapshot);
        ui_apply_state(&snapshot);
    }
    lv_timer_handler();
#else
    ui_state_t snapshot = {};
    copy_state(&snapshot);
    (void)snapshot;
#endif
}
