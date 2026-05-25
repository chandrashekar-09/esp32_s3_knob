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
#define UI_BG_HEX 0x000000
#define UI_STEP_COUNT 9

typedef struct {
    phase_t phase;
    app_state_t app;
    bool mesh_connected;
    bool mesh_root;
    int mesh_layer;
    char ota_status[32];
    bool dirty;
} ui_state_t;

static ui_state_t s_ui = {
    .phase = PH_BOOT,
    .app = {0},
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
static lv_obj_t *s_subtitle = NULL;
static lv_obj_t *s_status = NULL;
static lv_obj_t *s_center = NULL;
static lv_obj_t *s_level = NULL;
static lv_obj_t *s_hint = NULL;
static lv_obj_t *s_ota = NULL;
static lv_obj_t *s_rings[4] = {};
static lv_obj_t *s_steps[UI_STEP_COUNT] = {};

static const char *kStepLabels[UI_STEP_COUNT] = {
    "01 USE CASE",
    "02 NETWORK",
    "03 ASSIGN",
    "04 TIME",
    "05 CABINS",
    "06 CABIN INT",
    "07 CORR INT",
    "08 START",
    "HOME",
};

static const char *phase_label(phase_t phase)
{
    switch (phase) {
    case PH_OFF:
        return "POWER OFF";
    case PH_BOOT:
        return "BOOT";
    case PH_ORIENT:
        return "ORIENT";
    case PH_USE_CASE:
        return "USE CASE";
    case PH_NET:
        return "NETWORK";
    case PH_CONN:
        return "CONNECT";
    case PH_ASSIGN:
        return "ASSIGN";
    case PH_TIME:
        return "TIME";
    case PH_OPEN_HR:
        return "OPEN HOUR";
    case PH_CLOSE_HR:
        return "CLOSE HOUR";
    case PH_CAB_CNT:
        return "CABIN COUNT";
    case PH_CAB_INT:
        return "CABIN INTERVAL";
    case PH_COR_INT:
        return "CORRIDOR INTERVAL";
    case PH_START:
        return "START";
    case PH_SPLASH:
        return "SPLASH";
    case PH_HOME:
        return "HOME";
    case PH_SLEEP:
        return "SLEEP";
    case PH_ADMIN:
        return "ADMIN";
    case PH_ONLINE_QR:
        return "ONLINE QR";
    case PH_WIFI_CONN:
        return "WIFI CONNECT";
    case PH_ONLINE_REG:
        return "ONLINE REG";
    case PH_LOC_PICK:
        return "LOC PICK";
    case PH_LOC_NAMING:
        return "LOC NAME";
    case PH_CLEANING_HUB:
        return "CLEANING HUB";
    case PH_CLEANING_HUB_FR:
        return "CLEANING DETAIL";
    default:
        return "STATE";
    }
}

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

static lv_obj_t *make_label(const char *text,
                            lv_align_t align,
                            int x,
                            int y,
                            uint32_t color,
                            int letter_space,
                            const lv_font_t *font)
{
    lv_obj_t *label = lv_label_create(s_screen);
    lv_label_set_text(label, text);
    lv_obj_align(label, align, x, y);
    lv_obj_set_style_text_color(label, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(label, letter_space, LV_PART_MAIN);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    if (font) {
        lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    }
    return label;
}

static void ui_style_step(lv_obj_t *step, bool active)
{
    uint32_t color = active ? UI_ACCENT_HEX : UI_TEXT_FAINT_HEX;
    lv_obj_set_style_text_color(step, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_border_color(step, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_bg_color(step, lv_color_hex(UI_BG_HEX), LV_PART_MAIN);
}

static void ui_update_steps(phase_t phase)
{
    int active = phase_manager_get_step_index(phase);

    for (int i = 0; i < UI_STEP_COUNT; ++i) {
        if (s_steps[i]) {
            ui_style_step(s_steps[i], i == active);
        }
    }
}

static void ui_build(void)
{
    s_screen = lv_scr_act();
    lv_obj_clean(s_screen);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(UI_BG_HEX), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, LV_PART_MAIN);

    s_rings[0] = lv_arc_create(s_screen);
    s_rings[1] = lv_arc_create(s_screen);
    s_rings[2] = lv_arc_create(s_screen);
    s_rings[3] = lv_arc_create(s_screen);
    arc_setup(s_rings[0], 336, 7);
    arc_setup(s_rings[1], 310, 5);
    arc_setup(s_rings[2], 288, 4);
    arc_setup(s_rings[3], 266, 3);

    s_wordmark = make_label("QUESORT", LV_ALIGN_TOP_MID, 0, 10, UI_ACCENT_HEX, 4, &lv_font_montserrat_14);
    s_subtitle = make_label("MULTI-SPECTRUM", LV_ALIGN_TOP_MID, 0, 26, UI_TEXT_FAINT_HEX, 2, &lv_font_montserrat_10);
    make_label("UNIFIED SIMULATOR", LV_ALIGN_TOP_MID, 0, 40, UI_TEXT_FAINT_HEX, 2, &lv_font_montserrat_10);

    const int step_w = 108;
    const int step_h = 16;
    const int step_gap = 4;
    const int rows = 3;
    const int cols = 3;
    const int grid_w = cols * step_w + (cols - 1) * step_gap;
    const int start_x = (lv_obj_get_width(s_screen) - grid_w) / 2;
    const int start_y = 58;

    for (int i = 0; i < UI_STEP_COUNT; ++i) {
        int row = i / cols;
        int col = i % cols;
        if (row >= rows) {
            row = rows - 1;
            col = cols - 1;
        }

        lv_obj_t *step = lv_label_create(s_screen);
        lv_label_set_text(step, kStepLabels[i]);
        lv_label_set_long_mode(step, LV_LABEL_LONG_CLIP);
        lv_obj_set_size(step, step_w, step_h);
        lv_obj_set_pos(step,
                   start_x + col * (step_w + step_gap),
                   start_y + row * (step_h + 2));
        lv_obj_set_style_text_align(step, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_style_text_font(step, &lv_font_montserrat_10, LV_PART_MAIN);
        lv_obj_set_style_text_letter_space(step, 1, LV_PART_MAIN);
        lv_obj_set_style_border_width(step, 1, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(step, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_pad_left(step, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_right(step, 0, LV_PART_MAIN);
        s_steps[i] = step;
    }

    s_status = make_label("POWER ON TO BEGIN", LV_ALIGN_TOP_MID, 0, 120, UI_TEXT_DIM_HEX, 2, &lv_font_montserrat_10);
    s_center = make_label("Q1", LV_ALIGN_CENTER, 0, -12, 0xFFFFFF, 2, &lv_font_montserrat_16);
    s_level = make_label("EMPTY", LV_ALIGN_CENTER, 0, 18, UI_ACCENT_HEX, 3, &lv_font_montserrat_12);
    s_hint = make_label("ROTATE QUEUE  HOLD ADMIN", LV_ALIGN_BOTTOM_MID, 0, -38, UI_TEXT_FAINT_HEX, 1, &lv_font_montserrat_10);
    s_ota = make_label("OTA WAITING", LV_ALIGN_BOTTOM_MID, 0, -18, UI_TEXT_DIM_HEX, 1, &lv_font_montserrat_10);
}

static void ui_apply_home(const ui_state_t *state)
{
    uint8_t level = state->app.queue_level;
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

static void ui_apply_setup(const ui_state_t *state)
{
    for (int i = 0; i < 4; ++i) {
        lv_arc_set_value(s_rings[i], 2 + i);
        lv_obj_set_style_arc_color(s_rings[i], lv_color_hex(UI_TRACK_HEX), LV_PART_INDICATOR);
    }

    lv_label_set_text(s_center, phase_label(state->phase));
    lv_obj_set_style_text_color(s_center, lv_color_hex(UI_ACCENT_HEX), LV_PART_MAIN);

    switch (state->phase) {
    case PH_USE_CASE:
        lv_label_set_text_fmt(s_level, "UC %d", (int)state->app.use_case_sel + 1);
        break;
    case PH_NET:
        lv_label_set_text_fmt(s_level, "NET %d", (int)state->app.net_sel + 1);
        break;
    case PH_TIME:
        lv_label_set_text_fmt(s_level, "%04u-%02u-%02u",
                              (unsigned)state->app.time_fields[0],
                              (unsigned)state->app.time_fields[1],
                              (unsigned)state->app.time_fields[2]);
        break;
    case PH_OPEN_HR:
        lv_label_set_text_fmt(s_level, "OPEN %02u:00", (unsigned)state->app.open_hour);
        break;
    case PH_CLOSE_HR:
        lv_label_set_text_fmt(s_level, "CLOSE %02u:00", (unsigned)state->app.close_hour);
        break;
    case PH_CAB_CNT:
        lv_label_set_text_fmt(s_level, "CABINS %u", (unsigned)state->app.cab_count);
        break;
    case PH_CAB_INT:
        lv_label_set_text_fmt(s_level, "CAB INT %u", (unsigned)state->app.cab_int);
        break;
    case PH_COR_INT:
        lv_label_set_text_fmt(s_level, "COR INT %u", (unsigned)state->app.cor_int);
        break;
    case PH_ORIENT:
        lv_label_set_text_fmt(s_level, "ORIENT %u", (unsigned)state->app.orient_pos);
        break;
    default:
        lv_label_set_text(s_level, "CONFIG");
        break;
    }

    lv_label_set_text(s_hint, "PRESS TO ADVANCE");
}

static void ui_apply_state(const ui_state_t *state)
{
    ui_update_steps(state->phase);
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
        ui_apply_home(state);
        break;
    default:
        ui_apply_setup(state);
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
    phase_manager_get_state(&s_ui.app);
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
    if (xGuiSemaphore && xSemaphoreTake(xGuiSemaphore, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (consume_dirty()) {
            ui_state_t snapshot = {};
            copy_state(&snapshot);
            phase_manager_get_state(&snapshot.app);
            ui_apply_state(&snapshot);
        }
        lv_timer_handler();
        xSemaphoreGive(xGuiSemaphore);
    }
#else
    ui_state_t snapshot = {};
    copy_state(&snapshot);
    (void)snapshot;
#endif
}
