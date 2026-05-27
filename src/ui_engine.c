/* JC3636K718 — simulator-style UI (quesort_simulator.html port).
 *
 * Four phases rendered in this build:
 *   PH_BOOT  — orange QUESORT wordmark + filling progress arc
 *   PH_HOME  — queue arc + status word in centre + cabin segments at rim
 *   PH_ADMIN — settings: cabin count adjuster
 *   PH_SLEEP — pure black with "SLEEP / TAP TO WAKE" prompt
 *
 * State source-of-truth is phase_manager. ui_engine_render() pulls
 * fresh state via phase_manager_get_state() on EVERY tick — no caching —
 * so the encoder/touch can change phase_manager state and the UI
 * reflects it on the next render with no manual cache invalidation.
 */

#include "ui_engine.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"

#if defined(__has_include) && __has_include("lvgl.h")
#include "lvgl.h"
#define UI_HAS_LVGL 1
#else
#define UI_HAS_LVGL 0
#endif

/* ───────────────────────────── design tokens ─────────────────────────── */
/* Mirrors quesort_simulator.html's :root vars (Nothing-inspired palette). */
#define COL_BG          0x000000
#define COL_BG_SOFT     0x080808
#define COL_TRACK       0x181818
#define COL_FG          0xFFFFFF
#define COL_FG_DIM      0x555555
#define COL_FG_FAINT    0x222222
#define COL_ACCENT      0xFF7520
#define COL_BLUE        0x00AAFF
#define COL_RED         0xDC2626

/* ───────────────────────────── shared state ──────────────────────────── */
static const char *TAG = "ui";

SemaphoreHandle_t xGuiSemaphore = NULL;

#if UI_HAS_LVGL
/* Per-phase root containers — only the active phase's container is
 * shown, the others are hidden. This avoids tear-down/rebuild churn
 * on phase changes and keeps render dirty regions small. */
static lv_obj_t *s_screen = NULL;

/* Phase: BOOT — progress arc + 8 dot ring + STARTING UP/PLEASE WAIT,
 * mirrors quesort_simulator.html#renderBoot. NO QUESORT wordmark — the
 * simulator's boot screen never shows that string. */
static lv_obj_t *s_boot_root      = NULL;
static lv_obj_t *s_boot_arc       = NULL;
static lv_obj_t *s_boot_dot[8]    = { NULL };
static lv_obj_t *s_boot_top       = NULL;   /* "STARTING UP" */
static lv_obj_t *s_boot_bot       = NULL;   /* "PLEASE WAIT" */

/* Phase: HOME — outer cabin segment ring + queue arc + centre status
 * word. Mirrors the simulator's single-device renderHome layout.
 * NO QUESORT wordmark, NO bottom hint — only the rings and centre. */
static lv_obj_t *s_home_root      = NULL;
static lv_obj_t *s_home_queue_arc = NULL;   /* queue level arc */
static lv_obj_t *s_home_cab_meter = NULL;   /* cabin segments meter */
static lv_meter_scale_t *s_home_cab_scale = NULL;
static lv_obj_t *s_home_center    = NULL;   /* solid disc */
static lv_obj_t *s_home_status    = NULL;   /* "BUSY" / "OK" / ... */

/* Phase: ADMIN */
static lv_obj_t *s_admin_root     = NULL;
static lv_obj_t *s_admin_title    = NULL;
static lv_obj_t *s_admin_field    = NULL;   /* field label */
static lv_obj_t *s_admin_value    = NULL;   /* value */
static lv_obj_t *s_admin_hint     = NULL;

/* Phase: SLEEP — pure black, no widgets (simulator parity). */
static lv_obj_t *s_sleep_root     = NULL;

/* Track which phase's root is currently visible to avoid pointless
 * show/hide ops every render. */
static phase_t s_visible_phase = (phase_t)-1;

/* ───────────────────────────── helpers ───────────────────────────────── */

static uint32_t color_for_level(uint8_t level)
{
    const taxonomy_entry_t *e = phase_manager_get_taxonomy(level);
    return e ? e->color_hex : COL_ACCENT;
}

static const char *label_for_level(uint8_t level)
{
    const taxonomy_entry_t *e = phase_manager_get_taxonomy(level);
    return e ? e->label : "QUEUE";
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text,
                            const lv_font_t *font, uint32_t color,
                            int letter_space, lv_align_t align,
                            int dx, int dy)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_letter_space(l, letter_space, 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(l, align, dx, dy);
    return l;
}

static void screen_tap_cb(lv_event_t *e);  /* fwd decl */

static lv_obj_t *make_phase_root(void)
{
    lv_obj_t *root = lv_obj_create(s_screen);
    lv_obj_set_size(root, 360, 360);
    lv_obj_center(root);
    lv_obj_set_style_bg_color(root, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
    /* Phase roots must catch taps directly: by default lv_obj_create
     * sets CLICKABLE so the click event lands on the root rather than
     * bubbling to s_screen. Register the wake/short-press handler
     * here so every tap on the active phase gets routed to
     * phase_manager regardless of which widget child is under the
     * finger. */
    lv_obj_add_flag(root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(root, screen_tap_cb, LV_EVENT_CLICKED, NULL);
    return root;
}

static void show_only(phase_t phase)
{
    if (phase == s_visible_phase) return;
    s_visible_phase = phase;

    lv_obj_t *to_show =
        (phase == PH_BOOT)  ? s_boot_root  :
        (phase == PH_HOME)  ? s_home_root  :
        (phase == PH_ADMIN) ? s_admin_root :
        (phase == PH_SLEEP) ? s_sleep_root :
        s_home_root;  /* fallback */

    lv_obj_t *roots[] = { s_boot_root, s_home_root, s_admin_root, s_sleep_root };
    for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); ++i) {
        if (!roots[i]) continue;
        if (roots[i] == to_show) {
            lv_obj_clear_flag(roots[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(roots[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/* ───────────────────────────── BOOT screen ───────────────────────────── */
/*
 * Mirrors quesort_simulator.html#renderBoot:
 *   - thin orange progress arc at r=60 (sim scale, → 108 px at our 1.8×
 *     scaling, so the arc widget needs to be ~216 px)
 *   - 8 dots evenly spaced at r=70 (→ 126 px), lit progressively as
 *     boot animation advances
 *   - "STARTING UP" / "PLEASE WAIT" centred text
 *   - NO QUESORT wordmark on this screen in the simulator
 */
static void build_boot(void)
{
    s_boot_root = make_phase_root();

    /* Progress arc — orange, sweeps 0..360° clockwise from top. */
    s_boot_arc = lv_arc_create(s_boot_root);
    lv_obj_set_size(s_boot_arc, 216, 216);
    lv_obj_center(s_boot_arc);
    lv_arc_set_rotation(s_boot_arc, 270);   /* start at 12 o'clock */
    lv_arc_set_bg_angles(s_boot_arc, 0, 360);
    lv_arc_set_range(s_boot_arc, 0, 100);
    lv_arc_set_value(s_boot_arc, 0);
    lv_obj_remove_style(s_boot_arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(s_boot_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_boot_arc, 4, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_boot_arc, 4, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_boot_arc, lv_color_hex(0x16110a), LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_boot_arc, lv_color_hex(COL_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(s_boot_arc, LV_OPA_60, LV_PART_INDICATOR);

    /* 8 dots at r ≈ 126 px from centre. Position via x/y offsets from
     * the centre of the root, using lv_trigo lookup so we don't drag
     * in libm. Each dot starts dim; apply_boot lights them up as the
     * boot progress crosses each octant. */
    static const int dx[8] = {   0,  89, 126,  89,   0, -89,-126, -89 };
    static const int dy[8] = {-126, -89,   0,  89, 126,  89,   0, -89 };
    for (int i = 0; i < 8; i++) {
        lv_obj_t *d = lv_obj_create(s_boot_root);
        lv_obj_set_size(d, 7, 7);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(d, 0, 0);
        lv_obj_set_style_pad_all(d, 0, 0);
        lv_obj_set_style_bg_color(d, lv_color_hex(0x1a1a1a), 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        lv_obj_clear_flag(d, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(d, LV_ALIGN_CENTER, dx[i], dy[i]);
        s_boot_dot[i] = d;
    }

    s_boot_top = make_label(s_boot_root, "STARTING UP",
        &lv_font_montserrat_14, 0xAAAAAA, 4, LV_ALIGN_CENTER, 0, -14);
    s_boot_bot = make_label(s_boot_root, "PLEASE WAIT",
        &lv_font_montserrat_10, COL_FG_DIM, 3, LV_ALIGN_CENTER, 0, 14);
}

static void apply_boot(const app_state_t *state)
{
    (void)state;
    /* Pulse the progress arc 0→100 over the ~1s boot hold and light
     * the 8 dots as bootAnim crosses each 12.5% step (matches
     * simulator's `lit = dev.bootAnim > i*12`). */
    uint32_t t = (uint32_t)(esp_timer_get_time() / 1000ULL);
    static uint32_t boot_t0 = 0;
    if (boot_t0 == 0) boot_t0 = t;
    int pct = (int)((t - boot_t0) * 100 / 1000);
    if (pct > 100) pct = 100;
    lv_arc_set_value(s_boot_arc, pct);

    for (int i = 0; i < 8; i++) {
        bool lit = pct > i * 12;
        lv_obj_set_style_bg_color(s_boot_dot[i],
            lv_color_hex(lit ? COL_ACCENT : 0x1a1a1a), 0);
        lv_obj_set_style_bg_opa(s_boot_dot[i],
            lit ? LV_OPA_80 : LV_OPA_COVER, 0);
    }
}

/* ───────────────────────────── HOME screen ───────────────────────────── */
/*
 * Layout (360×360 round):
 *   - outer cabin meter: thin segmented ring at r=170 (decorative; one
 *     wedge per cab_count + 1 corridor; all idle-coloured for now)
 *   - queue arc: 280×280, 8px wide, fills -130°..+130° proportionally
 *     to queue_level/8 with the taxonomy colour
 *   - centre disc: 160px round soft-bg circle that holds the status
 *     word + Q-level
 *   - top: small QUESORT wordmark
 *   - bottom: hint "ROTATE • HOLD ADMIN"
 */
static void build_home(void)
{
    s_home_root = make_phase_root();

    /* Outer cabin segments via lv_meter — tick marks around the rim,
     * recolored each frame based on phase_manager_get_state().cab_count. */
    s_home_cab_meter = lv_meter_create(s_home_root);
    lv_obj_set_size(s_home_cab_meter, 352, 352);
    lv_obj_center(s_home_cab_meter);
    lv_obj_clear_flag(s_home_cab_meter, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(s_home_cab_meter, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_home_cab_meter, 0, 0);
    lv_obj_set_style_pad_all(s_home_cab_meter, 0, 0);
    s_home_cab_scale = lv_meter_add_scale(s_home_cab_meter);
    /* Default 16 evenly-spaced tick marks. apply_home() rebuilds the
     * scale per-frame if cab_count differs from the prior count. */
    lv_meter_set_scale_ticks(s_home_cab_meter, s_home_cab_scale,
                             8, 3, 14, lv_color_hex(COL_FG_DIM));
    lv_meter_set_scale_range(s_home_cab_meter, s_home_cab_scale,
                             0, 100, 360, 270);

    /* Main queue arc */
    s_home_queue_arc = lv_arc_create(s_home_root);
    lv_obj_set_size(s_home_queue_arc, 300, 300);
    lv_obj_center(s_home_queue_arc);
    /* Arc from 130° (lower-left) sweeping CW to 50° (lower-right) of the
     * top half — same -130..+130 sweep as the HTML simulator's QAR=130. */
    lv_arc_set_rotation(s_home_queue_arc, 130);
    lv_arc_set_bg_angles(s_home_queue_arc, 0, 280);
    lv_arc_set_range(s_home_queue_arc, 0, 100);
    lv_arc_set_value(s_home_queue_arc, 0);
    lv_obj_remove_style(s_home_queue_arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(s_home_queue_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_home_queue_arc, 14, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_home_queue_arc, 14, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_home_queue_arc, lv_color_hex(COL_TRACK),  LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_home_queue_arc, lv_color_hex(COL_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(s_home_queue_arc, true, LV_PART_INDICATOR);

    /* Centre disc */
    s_home_center = lv_obj_create(s_home_root);
    lv_obj_set_size(s_home_center, 200, 200);
    lv_obj_center(s_home_center);
    lv_obj_set_style_bg_color(s_home_center, lv_color_hex(COL_BG_SOFT), 0);
    lv_obj_set_style_bg_opa(s_home_center, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_home_center, 1, 0);
    lv_obj_set_style_border_color(s_home_center, lv_color_hex(COL_TRACK), 0);
    lv_obj_set_style_radius(s_home_center, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(s_home_center, 0, 0);
    lv_obj_clear_flag(s_home_center, LV_OBJ_FLAG_SCROLLABLE);
    /* Keep CLICKABLE so taps on the centre disc still propagate to
     * the phase root's CLICKED handler (LVGL bubbles up). */

    /* Centre status word — only thing rendered inside the disc. The
     * simulator's renderHome for single-device puts ONLY the status
     * label here; no Q-number, no wordmark, no bottom hint. */
    s_home_status = make_label(s_home_center, "EMPTY",
        &lv_font_montserrat_28, COL_ACCENT, 4, LV_ALIGN_CENTER, 0, 0);
}

static void apply_home(const app_state_t *state)
{
    uint8_t level = state->queue_level;
    if (level < 1) level = 1; else if (level > 8) level = 8;
    uint32_t color = color_for_level(level);

    /* Queue arc: fills in fine increments per raw detent via the
     * sub_step accumulator, then "lands" cleanly when the level commits
     * at every Nth detent. Effective progress is (level-1)*STEP +
     * sub_step over a span of 7*STEP discrete sub-positions (levels 1..8
     * have 7 transitions, each subdivided into STEP detents). The
     * value 0..100 LV_arc range maps onto that span. STEP is read from
     * the magnitude limit on sub_step (max valid is STEP-1, so STEP =
     * max(|sub|)+1 conservatively — but phase_manager owns the truth, so
     * just trust DETENTS_PER_STEP defined to match here). */
    enum { STEP = 3 };  /* must match DETENTS_PER_STEP in phase_manager.c */
    int sub = state->queue_sub_step;
    if (sub > STEP - 1)  sub = STEP - 1;
    if (sub < -(STEP - 1)) sub = -(STEP - 1);
    int prog = (level - 1) * STEP + sub;
    if (prog < 0) prog = 0; else if (prog > 7 * STEP) prog = 7 * STEP;
    lv_arc_set_value(s_home_queue_arc, prog * 100 / (7 * STEP));
    lv_obj_set_style_arc_color(s_home_queue_arc, lv_color_hex(color),
                               LV_PART_INDICATOR);

    /* Centre disc border tints with the level too, for cohesion. */
    lv_obj_set_style_border_color(s_home_center,
        lv_color_hex(level >= 5 ? color : COL_TRACK), 0);

    /* Centre status word colours with the level. */
    lv_label_set_text(s_home_status, label_for_level(level));
    lv_obj_set_style_text_color(s_home_status, lv_color_hex(color), 0);

    /* Rebuild cabin tick scale if the count changed. cab_count==0 hides
     * the ticks entirely (no cabins assigned yet). */
    static uint8_t prev_cab = 255;
    uint8_t cab = state->cab_count;
    if (cab > 24) cab = 24;
    if (cab != prev_cab) {
        prev_cab = cab;
        lv_meter_set_scale_ticks(s_home_cab_meter, s_home_cab_scale,
                                 cab == 0 ? 0 : cab,
                                 3, 14,
                                 lv_color_hex(cab == 0 ? COL_BG : COL_FG_DIM));
    }
}

/* ───────────────────────────── ADMIN screen ──────────────────────────── */

static void build_admin(void)
{
    s_admin_root = make_phase_root();
    s_admin_title = make_label(s_admin_root, "CABIN CONFIG",
        &lv_font_montserrat_14, COL_ACCENT, 6, LV_ALIGN_TOP_MID, 0, 50);
    s_admin_field = make_label(s_admin_root, "CABINS",
        &lv_font_montserrat_10, COL_FG_DIM, 4, LV_ALIGN_CENTER, 0, -38);
    s_admin_value = make_label(s_admin_root, "4",
        &lv_font_montserrat_28, COL_FG, 4, LV_ALIGN_CENTER, 0, 10);
    s_admin_hint = make_label(s_admin_root, "ROTATE  •  HOLD EXIT",
        &lv_font_montserrat_10, COL_FG_FAINT, 2, LV_ALIGN_BOTTOM_MID, 0, -28);
}

static void apply_admin(const app_state_t *state)
{
    lv_label_set_text_fmt(s_admin_value, "%u", (unsigned)state->cab_count);
}

/* ───────────────────────────── SLEEP screen ──────────────────────────── */

static void build_sleep(void)
{
    /* Simulator's renderSleep fills the disc with pure black — no text,
     * no animation — matching real-hardware backlight-off behaviour.
     * Tap-to-wake still works because the phase root catches the click. */
    s_sleep_root = make_phase_root();
}

static void apply_sleep(const app_state_t *state) { (void)state; }

/* ───────────────────────────── render ────────────────────────────────── */

static void apply_state(const app_state_t *state)
{
    show_only(state->phase);
    switch (state->phase) {
    case PH_BOOT:  apply_boot(state);  break;
    case PH_HOME:  apply_home(state);  break;
    case PH_ADMIN: apply_admin(state); break;
    case PH_SLEEP: apply_sleep(state); break;
    default:       apply_home(state);  break;  /* unhandled phase → home */
    }
}

static void screen_tap_cb(lv_event_t *e)
{
    /* Touch-anywhere = short press, wakes from SLEEP and toggles HOME nav.
     * Logging so we can see in the serial monitor whether LVGL is
     * actually routing the click here when the touch driver reports a
     * press. If touch PRESS shows in the cst816 log but this never
     * fires, the issue is LVGL event routing, not the I2C read. */
    (void)e;
    ESP_LOGI(TAG, "screen TAP → button(short)  phase=%d",
             (int)phase_manager_get_phase());
    phase_manager_on_button(true, 0);
}
#endif /* UI_HAS_LVGL */

/* ───────────────────────────── public API ────────────────────────────── */

void ui_engine_init(void)
{
#if UI_HAS_LVGL
    s_screen = lv_scr_act();
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_screen, 0, 0);
    lv_obj_set_style_border_width(s_screen, 0, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_screen, LV_SCROLLBAR_MODE_OFF);

    build_boot();
    build_home();
    build_admin();
    build_sleep();

    /* Wake-on-tap via touch driver (lvgl_port already feeds touch
     * events through lv_indev). Listening on the screen catches any
     * unfocused tap. */
    lv_obj_add_event_cb(s_screen, screen_tap_cb, LV_EVENT_CLICKED, NULL);

    /* Show whatever phase_manager is in now (BOOT on first launch). */
    app_state_t st;
    phase_manager_get_state(&st);
    apply_state(&st);
    ESP_LOGI(TAG, "UI built; initial phase=%d", (int)st.phase);
#endif
}

void ui_engine_set_phase(phase_t phase)
{
    /* Kept for source compatibility — phase_manager is the source of
     * truth now and ui_engine_render pulls fresh state each tick.
     * This setter is effectively a hint. */
    (void)phase;
}

void ui_engine_set_mesh_state(bool connected, bool root, int layer)
{
    (void)connected; (void)root; (void)layer;
}

void ui_engine_set_ota_status(const char *status)
{
    (void)status;
}

void ui_engine_render(void)
{
#if UI_HAS_LVGL
    /* Pull fresh state every tick — encoder/button updates land in
     * phase_manager and propagate to the UI without manual cache
     * invalidation. LVGL's invalidation tracking ensures only the
     * widgets whose content actually changed get repainted. */
    app_state_t st;
    phase_manager_get_state(&st);
    apply_state(&st);

    lv_timer_handler();
#endif
}
