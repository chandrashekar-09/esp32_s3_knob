/* JC3636K718 — simulator-style UI (quesort_simulator.html port).
 *
 * Three phases rendered in this build:
 *   PH_BOOT  — orange progress arc + 8-dot ring + STARTING UP
 *   PH_HOME  — queue arc + status word in centre (single-ring own,
 *              multi-ring/wedge layouts dormant pending mesh)
 *   PH_SLEEP — pure black, post-inactivity deep sleep
 *
 * State source-of-truth is phase_manager. ui_engine_render() pulls
 * fresh state via phase_manager_get_state() on EVERY tick — no caching —
 * so the encoder/touch can change phase_manager state and the UI
 * reflects it on the next render with no manual cache invalidation.
 */

#include "ui_engine.h"

#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "inactivity_alert.h"
#include "peer_registry.h"
#include "device_role.h"
#include "peer_sim.h"
#include "screenshot.h"

#if defined(__has_include) && __has_include("lvgl.h")
#include "lvgl.h"
#define UI_HAS_LVGL 1
#else
#define UI_HAS_LVGL 0
#endif

/* ───────────────────────────── design tokens ─────────────────────────── */
/* Aligned with the Nothing design system tokens
 * (https://github.com/dominikmartn/nothing-design-skill). Grayscale ramp
 * hits the system's --black / --surface / --border / --text-disabled
 * exactly. Accent stays brand orange (QueSort identity) instead of the
 * Nothing system's #D71921 red — the rule "color is meaningful, not
 * decorative" is satisfied by reserving the taxonomy palette for
 * status meaning and orange for brand identity moments only. */
#define COL_BG          0x000000   /* Nothing --black                 */
#define COL_BG_SOFT     0x111111   /* Nothing --surface (centre disc) */
#define COL_TRACK       0x222222   /* Nothing --border (queue track)  */
#define COL_FG          0xE8E8E8   /* Nothing --text-primary          */
#define COL_FG_DIM      0x666666   /* Nothing --text-disabled         */
#define COL_FG_FAINT    0x333333   /* Nothing --border-visible        */
#define COL_ACCENT      0xFF7520   /* QueSort brand orange (override) */
#define COL_BLUE        0x00AAFF
#define COL_RED         0xDC2626

/* Dot-matrix background tile. 12×12 RGB565 tile with a single dim
 * pixel at the centre — tiled across each phase root via the
 * bg_img_tiled style. 12 is chosen because 360 / 12 = 30 exactly,
 * which means an overlay widget centred on the screen with a width
 * that's a multiple of 12 has its tile pattern perfectly phase-
 * aligned with the root's tile — letting overlay masks blend
 * invisibly into the surrounding dotted bg. */
#define DOT_TILE_SIZE   12

/* ───────────────────────── multi-ring geometry ───────────────────────── */
/*
 * Up to 16 same-type peers visible at once. The screen is divided into
 * 4 CONCENTRIC RING SLOTS (outermost = own/lowest-numbered peer); each
 * slot is split into N WEDGES based on the peer-count tier:
 *
 *   1-4  peers → TIER_FULL    (1 wedge per ring, full 260° sweep)
 *   5-8  peers → TIER_HALF    (2 wedges/ring, L+R halves)
 *   9-12 peers → TIER_THIRD   (3 wedges/ring, top + L + R)
 *   13-16 peers → TIER_QUARTER (4 wedges/ring, evenly spaced)
 *
 * In every tier the 100° bottom gap (centered at south) is preserved
 * so the message box area stays free.
 *
 * Today only the OWN device is in the peer registry, so only one
 * wedge in one slot ever renders. The tables below define the
 * geometry for all 16 slots so when mesh ships, the renderer can
 * fan out without changing any widget code — the registry feeds
 * data, the renderer iterates the table.
 */

typedef enum {
    PEER_TIER_FULL    = 0,   /* 1-4 peers,  1 wedge per ring */
    PEER_TIER_HALF    = 1,   /* 5-8 peers,  2 wedges per ring */
    PEER_TIER_THIRD   = 2,   /* 9-12 peers, 3 wedges per ring */
    PEER_TIER_QUARTER = 3,   /* 13-16 peers, 4 wedges per ring */
} peer_tier_t;

/* Resolve tier from same-type peer count.
 *
 * "Prioritise 4 sections" per user spec: 9-16 peers all map to
 * QUARTER (4 wedges) instead of using THIRD for 9-12. This makes
 * the layout LESS rings-per-wedge for 10/11/12 peers (3 vs 4),
 * giving more breathing space and allowing the thicker-stroke
 * table to kick in. PEER_TIER_THIRD is left in the enum for
 * compatibility but never selected. */
static peer_tier_t peer_tier_for_count(uint8_t n)
{
    if (n <= 4)  return PEER_TIER_FULL;
    if (n <= 8)  return PEER_TIER_HALF;
    return PEER_TIER_QUARTER;   /* 9-16 — prefer 4 sections */
}

static uint8_t peer_tier_wedges_per_ring(peer_tier_t tier)
{
    return (uint8_t)(tier + 1);   /* FULL=1, HALF=2, THIRD=3, QUARTER=4 */
}

/* Concentric ring slot geometry. Slot 0 is outermost (matches the
 * current single-ring outer/inner) so 1-peer case looks identical to
 * today. Slots 1..3 step inward; stroke shrinks to fit more rings
 * without overlapping the centre disc (radius 110).
 *
 *   slot 0:  outer 172, inner 158  (stroke 14) ← outer ring, own/FR1
 *   slot 1:  outer 152, inner 142  (stroke 10)
 *   slot 2:  outer 136, inner 126  (stroke 10)
 *   slot 3:  outer 120, inner 110  (stroke 10) ← innermost, touches disc edge
 *
 * Used only by the multi-ring renderer (future). The OWN ring today
 * is exercised every frame via the wedge pool — only the OWN
 * device's pool entry is visible until mesh populates peer slots. */
typedef struct {
    int16_t outer_r;
    int16_t inner_r;
    int16_t stroke;   /* outer_r - inner_r, cached */
} ring_slot_geom_t;

/* Four geometry tables — runtime-selected by active_ring_slots()
 * based on how many rings the busiest wedge actually uses. Stroke
 * scales with the radial budget freed up by fewer rings:
 *
 *   max=4: stroke 20 (baseline, 100 %)         — kRingSlots_m4
 *   max=3: stroke 25 (+25 %)                    — kRingSlots_m3
 *   max=2: stroke 30 (+50 %)                    — kRingSlots_m2
 *   max=1: stroke 30 (same as m2; unspecified)  — kRingSlots_m1
 *
 * All four tables anchor slot 0's outer at 172 (no bezel encroach)
 * and use a 6 px inter-ring gap. Unused slots are zeroed so any
 * accidental reference paints nothing instead of a garbled overlap. */
static const ring_slot_geom_t kRingSlots_m4[4] = {
    { 172, 152, 20 },   /* slot 0 — outermost */
    { 146, 126, 20 },   /* gap 6 from slot 0 inner */
    { 120, 100, 20 },   /* gap 6 from slot 1 inner */
    {  94,  74, 20 },   /* slot 3 — innermost */
};
static const ring_slot_geom_t kRingSlots_m3[4] = {
    { 172, 147, 25 },   /* slot 0 — +25 % thicker */
    { 141, 116, 25 },
    { 110,  85, 25 },   /* slot 2 — innermost in this mode */
    {  85,  85,  0 },   /* slot 3 unused */
};
static const ring_slot_geom_t kRingSlots_m2[4] = {
    { 172, 142, 30 },   /* slot 0 — +50 % thicker */
    { 136, 106, 30 },   /* slot 1 — innermost in this mode */
    { 106, 106,  0 },   /* slot 2 unused */
    { 106, 106,  0 },   /* slot 3 unused */
};
static const ring_slot_geom_t kRingSlots_m1[4] = {
    { 172, 142, 30 },   /* slot 0 — only ring; stroke matches m2 */
    { 142, 142,  0 },   /* slot 1 unused */
    { 142, 142,  0 },   /* slot 2 unused */
    { 142, 142,  0 },   /* slot 3 unused */
};

/* Pick the active table for the current frame. Caller passes the
 * MAX rings any wedge will hold (computed from total peers + tier
 * via ceil(total/W)). */
static inline const ring_slot_geom_t *active_ring_slots(uint8_t max_rings)
{
    if (max_rings >= 4) return kRingSlots_m4;
    if (max_rings == 3) return kRingSlots_m3;
    if (max_rings == 2) return kRingSlots_m2;
    return kRingSlots_m1;  /* max_rings == 0 or 1 */
}

/* Wedge angular layout per tier. Each entry is the (rotation,
 * angular_span) for one wedge inside a ring slot. Angles are LVGL's
 * CW-from-east convention. Bottom gap (40°..140° going CW through
 * south=90°) is preserved by construction across all tiers.
 *
 *   TIER_FULL:    1 wedge spanning the full 260° (rotation 140, span 260)
 *   TIER_HALF:    2 wedges of 120° each, 20° top gap at 270°
 *   TIER_THIRD:   3 wedges of 76° each, two 16° inner gaps
 *   TIER_QUARTER: 4 wedges of 56° each, three 12° inner gaps
 */
typedef struct {
    int16_t rotation;    /* LVGL arc rotation (degrees CW from east) */
    int16_t span;        /* arc span in degrees */
} wedge_angle_t;

/* Layout tables — index by wedge_idx (0..N-1) within the tier. */
static const wedge_angle_t kWedges_full[1] = {
    { 140, 260 },
};
static const wedge_angle_t kWedges_half[2] = {
    { 140, 120 },   /* left half: SSW → just past N */
    { 280, 120 },   /* right half: just past N → ESE */
};
static const wedge_angle_t kWedges_third[3] = {
    { 140,  76 },   /* left: SSW → NW */
    { 232,  76 },   /* top:  NW → NE */
    { 324,  76 },   /* right: NE → ESE */
};
/* QUARTER gaps widened 12° → 20° (matches HALF) per user request.
 * Adjacent wedges' rounded end-caps (radius = stroke/2 = 13 px in
 * thin mode) were swallowing the entire 12° gap at the innermost
 * ring (slot 2 mid_r ≈ 95 px), making the rings visually merge.
 * 20° gives ~7 px of clear space at that radius and a comfortable
 * 22 px at the outermost. Wedges shrink 56° → 50° to absorb the
 * extra gap (4×50 + 3×20 = 260, same total sweep). */
static const wedge_angle_t kWedges_quarter[4] = {
    { 140,  50 },   /* slot 0 — bottom-left quarter */
    { 210,  50 },
    { 280,  50 },
    { 350,  50 },   /* slot 3 — bottom-right quarter */
};

static const wedge_angle_t *peer_tier_wedge_table(peer_tier_t tier)
{
    switch (tier) {
    case PEER_TIER_FULL:    return kWedges_full;
    case PEER_TIER_HALF:    return kWedges_half;
    case PEER_TIER_THIRD:   return kWedges_third;
    case PEER_TIER_QUARTER: return kWedges_quarter;
    }
    return kWedges_full;
}

/* Map peer index (0..N-1 within type, ascending by .number) to the
 * (ring_slot, wedge_idx) pair that hosts it. The OWN device (which
 * is the LOWEST-NUMBERED peer when present) lands in slot 0 wedge 0
 * — outermost, leftmost — per Option B (own placement follows number).
 *
 * Layout fill order is COLUMN-MAJOR + BALANCED: distribute the
 * `total` peers across the tier's W wedges as EVENLY as possible.
 * Within each wedge, rings stack outside → inside; wedges fill
 * left → right.
 *
 *   total / W = base  (each wedge gets at least this many rings)
 *   total % W = extra (first `extra` wedges get one MORE ring)
 *
 * Examples:
 *   5 peers in HALF  (W=2) → 3+2 (NOT 4+1)
 *   6 peers in HALF  (W=2) → 3+3
 *   7 peers in HALF  (W=2) → 4+3
 *   10 peers in THIRD (W=3) → 4+3+3
 *   13 peers in QUARTER (W=4) → 4+3+3+3
 *
 * Prior version was rigid column-major (4 rings per wedge until full,
 * then spill to next) which made 6 peers render as 4+2 instead of
 * 3+3. The new formula always picks the closest-to-equal split.
 *
 * Within a wedge, when fewer than 4 rings are used, they fill from
 * slot 0 (outermost) inward — so the outermost ring is always
 * populated. Peer NUMBER ascends from outside → inside within a
 * wedge, and from left → right across wedges. */
#define RINGS_PER_WEDGE 4
static void peer_slot_for_index(uint8_t idx, peer_tier_t tier,
                                uint8_t total,
                                uint8_t *out_ring, uint8_t *out_wedge)
{
    uint8_t W = peer_tier_wedges_per_ring(tier);
    if (W == 0 || total == 0) { *out_wedge = 0; *out_ring = 0; return; }

    uint8_t base  = total / W;
    uint8_t extra = total % W;

    /* "Fat" wedges (first `extra` of them) hold base+1 rings each;
     * "lean" wedges hold base. Idx range [0, extra*(base+1)) maps
     * into the fat wedges; the rest into the lean wedges. */
    uint8_t threshold = (uint8_t)(extra * (base + 1));
    if (idx < threshold) {
        uint8_t fat = (uint8_t)(base + 1);
        *out_wedge = (uint8_t)(idx / fat);
        *out_ring  = (uint8_t)(idx % fat);
    } else {
        uint8_t rem = (uint8_t)(idx - threshold);
        /* base could be 0 only if total < W, which never happens
         * given the tier definition (FULL: 1-4 / HALF: 5-8 / etc.)
         * but guard so a misconfig can't divide-by-zero. */
        uint8_t lean = base ? base : 1;
        *out_wedge = (uint8_t)(extra + rem / lean);
        *out_ring  = (uint8_t)(rem % lean);
    }
}

/* update_wedge / hide_wedge are defined further down, after the
 * pool storage and arc_set_value_anim adapter are in scope. */

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
static lv_obj_t *s_home_root         = NULL;
/* s_home_queue_arc removed — replaced by the wedge pool below.
 * Each peer (including own) renders into its own pool entry; the
 * pool entry occupied by the own device is the visual successor to
 * the prior single-arc widget. */
/* Hairline identity guide removed — the white-bordered own ball is
 * a strong enough "this is YOUR wedge" marker on its own, and the
 * extra arc was adding visual noise on the smaller disc. */

/* Wedge widget pool — ONE lv_arc per peer. Each arc has a track
 * (unfilled, COL_TRACK) and indicator (filled, peer's identity
 * colour). Rounded indicator ends are drawn in the indicator
 * colour so the leading/trailing semicircles of the fill blend
 * into the rest of the bar — no flat caps. */
#define MAX_PEERS_IN_VIEW    16
#define WEDGE_POOL_SIZE      MAX_PEERS_IN_VIEW
static lv_obj_t *s_wedge_pool[WEDGE_POOL_SIZE];
static int       s_wedge_prev_target[WEDGE_POOL_SIZE];
/* Tier change forces all wedges to re-target (geometry changed). */
static peer_tier_t s_prev_tier = (peer_tier_t)-1;

/* Per-peer NUMBER labels — one per peer slot. Each is a small
 * Mont 14 number ("1".."16") positioned at the wedge's mid-radius
 * / mid-angle so peers can be identified at a glance. Colour
 * flips based on fill level (see apply_home). */
static lv_obj_t *s_peer_num[MAX_PEERS_IN_VIEW];

/* End-of-fill ball — white-stroked circle on the own wedge at the
 * angle corresponding to the current fill end. Helps the user spot
 * their own ring in crowded multi-ring tiers. */
static lv_obj_t *s_own_end_ball = NULL;
static lv_obj_t *s_home_center       = NULL;  /* circular disc — true circle */
/* Centre disc is now a TRUE FULL CIRCLE — the bottom-chord mask
 * and chord stroke were removed per user request. The disc border
 * tints with status colour via apply_home(); no separate stroke
 * geometry needed. */
static lv_obj_t *s_home_fr           = NULL;  /* "FR1".."FR4" identity label */
static lv_obj_t *s_home_status       = NULL;  /* "EMPTY" / "FREE" / ... */
/* SEND labels are children of s_home_root directly (no wrapper box).
 * Positions use LV_ALIGN_CENTER + screen-y offsets so the text lands
 * below the queue rings without needing a layout container. */
static lv_obj_t *s_send_main         = NULL;  /* "SEND > 3" hero line */
static lv_obj_t *s_send_subtitle     = NULL;  /* "(FREE)" / "(EMPTY) ↓" line */

/* Inactivity-alert overlay (CONFIRM STATUS prompt). Hidden by default;
 * shown when state->inactive is true. Two elements only — blinking
 * "CONFIRM STATUS" hero + small "TAP TO DISMISS ALERT" hint. The
 * status echo from the simulator is dropped: the disc behind the
 * scrim still shows it, repeating in the overlay is noise. */
static lv_obj_t *s_inactive_overlay = NULL;
static lv_obj_t *s_inactive_top     = NULL;  /* blinking "CONFIRM STATUS" */
static lv_obj_t *s_inactive_hint    = NULL;  /* "TAP TO DISMISS ALERT" */

/* ADMIN phase removed — long-press now toggles device type
 * (FR↔T) at runtime; cabin config / opening hours / setup wizard
 * are no longer part of this build. */

/* Phase: SLEEP — pure black, no widgets (simulator parity). */
static lv_obj_t *s_sleep_root     = NULL;

/* Track which phase's root is currently visible to avoid pointless
 * show/hide ops every render. */
static phase_t s_visible_phase = (phase_t)-1;

/* Dotted-background tile — single shared lv_img_dsc_t referenced by
 * every phase root via bg_img_src + bg_img_tiled. Built once in
 * ui_engine_init() before any phase root is constructed. */
static lv_color_t s_dot_tile_buf[DOT_TILE_SIZE * DOT_TILE_SIZE];
static lv_img_dsc_t s_dot_tile_dsc = {
    .header = {
        .always_zero = 0,
        .w = DOT_TILE_SIZE,
        .h = DOT_TILE_SIZE,
        .cf = LV_IMG_CF_TRUE_COLOR,
    },
    .data_size = DOT_TILE_SIZE * DOT_TILE_SIZE * sizeof(lv_color_t),
    .data = (uint8_t *)s_dot_tile_buf,
};

static void build_dot_tile(void)
{
    /* Fill black, then plant a single dim grey pixel at the tile's
     * centre. Tiled at 14 px spacing this lands a dot every 14 px
     * across each phase root — Nothing's reference grid is 12-16 px
     * for the dot-matrix idiom, this sits in the middle. */
    lv_color_t bg  = lv_color_hex(COL_BG);
    lv_color_t dot = lv_color_hex(COL_TRACK);
    for (int i = 0; i < DOT_TILE_SIZE * DOT_TILE_SIZE; i++) {
        s_dot_tile_buf[i] = bg;
    }
    int mid = DOT_TILE_SIZE / 2;
    s_dot_tile_buf[mid * DOT_TILE_SIZE + mid] = dot;
}

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

/* 16-colour device-identity palette. ALL NEON — the previous
 * bronze/olive entries (dim by design) are replaced with brighter
 * variants so every device reads as "lit up" rather than "muted".
 * Adjacent pairs intentionally on opposite sides of the wheel so
 * peers with consecutive numbers in the same ring don't blur. */
static const uint32_t kDevicePalette[16] = {
    0xFF7520,   /*  1  orange       */
    0x00CCFF,   /*  2  cyan         */
    0xA855F7,   /*  3  purple       */
    0x00FF66,   /*  4  green        */
    0xFFEE00,   /*  5  yellow       */
    0xFF1493,   /*  6  hot pink     */
    0x00FFCC,   /*  7  teal         */
    0xFF2222,   /*  8  red          */
    0x3366FF,   /*  9  blue         */
    0x99FF33,   /* 10  lime         */
    0xFF66AA,   /* 11  light pink   (was bronze — too dim) */
    0xFF9933,   /* 12  amber        */
    0x00BBFF,   /* 13  sky          */
    0xFF6347,   /* 14  coral        */
    0x44DD44,   /* 15  spring green (was olive — too dim) */
    0x6633FF,   /* 16  indigo       */
};

/* Stripe / pattern logic removed — back to simple single-colour
 * homogenous wedges. The 16-colour neon kDevicePalette above is
 * itself the source of differentiation: all entries are bright
 * and visually distinct from their neighbours on the wheel. */

static uint32_t device_color_for_number(uint8_t n)
{
    if (n < 1 || n > 16) return COL_ACCENT;
    return kDevicePalette[n - 1];
}

/* Type-prefixed identity string. "FR1".."FR16" or "T1".."T16". */
static void format_device_label(const app_state_t *st, char *buf, size_t buflen)
{
    const char *prefix = (st->device_type == DEV_TYPE_T) ? "T" : "FR";
    snprintf(buf, buflen, "%s%u", prefix, (unsigned)st->device_number);
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

static void screen_event_cb(lv_event_t *e);  /* fwd decl */

/* Render one peer's wedge — single solid-colour arc with rounded
 * indicator AND track ends. The neon palette is the differentiator;
 * homogeneous fill keeps the look readable across multi-ring
 * layouts. */
static void update_peer_wedge(int peer_idx, const ring_slot_geom_t *geom,
                              const wedge_angle_t *angle,
                              uint32_t color, uint32_t track_color,
                              int target_value, bool stale)
{
    if (peer_idx < 0 || peer_idx >= MAX_PEERS_IN_VIEW) return;
    lv_obj_t *a = s_wedge_pool[peer_idx];

    int diam = geom->outer_r * 2;
    lv_obj_set_size(a, diam, diam);
    lv_obj_center(a);
    lv_arc_set_rotation(a, angle->rotation);
    lv_arc_set_bg_angles(a, 0, angle->span);
    lv_obj_set_style_arc_width(a, geom->stroke, LV_PART_MAIN);
    lv_obj_set_style_arc_width(a, geom->stroke, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(a, lv_color_hex(track_color), LV_PART_MAIN);
    lv_obj_set_style_arc_color(a, lv_color_hex(color),       LV_PART_INDICATOR);
    /* Stale peers (no broadcast in MESH_PEER_STALE_MS) fade to 50%
     * opacity so the user can see the connection is degrading
     * without the peer disappearing yet. Full opacity returns
     * automatically on the next received frame. */
    lv_opa_t op = stale ? LV_OPA_50 : LV_OPA_COVER;
    lv_obj_set_style_arc_opa(a, op, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(a, op, LV_PART_INDICATOR);
    lv_obj_clear_flag(a, LV_OBJ_FLAG_HIDDEN);

    if (s_wedge_prev_target[peer_idx] != target_value) {
        s_wedge_prev_target[peer_idx] = target_value;
        lv_arc_set_value(a, target_value);
    }
}

static void hide_peer_wedge(int peer_idx)
{
    if (peer_idx < 0 || peer_idx >= MAX_PEERS_IN_VIEW) return;
    lv_obj_add_flag(s_wedge_pool[peer_idx], LV_OBJ_FLAG_HIDDEN);
    s_wedge_prev_target[peer_idx] = -1;
}

/* Project (radius, angle_deg) onto screen coordinates using LVGL's
 * fixed-point trig (avoids pulling in libm). LVGL uses CW-from-east
 * for arc angles, matching our wedge_angle_t convention. */
static void polar_to_screen(int radius, int angle_deg,
                            int *out_x, int *out_y)
{
    int32_t s = lv_trigo_sin(angle_deg);
    int32_t c = lv_trigo_sin(angle_deg + 90);   /* cos via sin shift */
    *out_x = 180 + (int)((c * radius) >> LV_TRIGO_SHIFT);
    *out_y = 180 + (int)((s * radius) >> LV_TRIGO_SHIFT);
}

static lv_obj_t *make_phase_root(void)
{
    lv_obj_t *root = lv_obj_create(s_screen);
    lv_obj_set_size(root, 360, 360);
    lv_obj_center(root);
    lv_obj_set_style_bg_color(root, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    /* Dot-matrix background — tiled across the whole root. Sleep
     * phase overrides this to pure black to match the simulator's
     * power-saved feel. */
    lv_obj_set_style_bg_img_src(root, &s_dot_tile_dsc, 0);
    lv_obj_set_style_bg_img_tiled(root, true, 0);
    lv_obj_set_style_bg_img_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
    /* Phase roots are the ONLY CLICKABLE widgets in the tree —
     * inner widgets (overlay, centre disc, etc.) have CLICKABLE
     * cleared so LVGL's hit-test passes through them and the
     * touch always lands on the phase root. The per-root handler
     * below catches BOTH short-tap dismisses (LV_EVENT_CLICKED)
     * and FR↔T toggles (LV_EVENT_LONG_PRESSED, fires after
     * indev_drv.long_press_time = 1500 ms set in lvgl_port.c).
     *
     * NOTE: LVGL 8 does NOT bubble events from clickable children
     * to parents by default — requires LV_OBJ_FLAG_EVENT_BUBBLE
     * on the parent. The earlier screen-level handler relied on
     * non-existent bubbling, which is why long-press never fired
     * even though the indev was correctly generating the event. */
    lv_obj_add_flag(root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(root, screen_event_cb, LV_EVENT_PRESSED,      NULL);
    lv_obj_add_event_cb(root, screen_event_cb, LV_EVENT_CLICKED,      NULL);
    lv_obj_add_event_cb(root, screen_event_cb, LV_EVENT_LONG_PRESSED, NULL);
    return root;
}

static void show_only(phase_t phase)
{
    if (phase == s_visible_phase) return;
    s_visible_phase = phase;

    lv_obj_t *to_show =
        (phase == PH_BOOT)  ? s_boot_root  :
        (phase == PH_HOME)  ? s_home_root  :
        /* PH_ADMIN no longer reachable in this build. */
        (phase == PH_SLEEP) ? s_sleep_root :
        s_home_root;  /* fallback */

    lv_obj_t *roots[] = { s_boot_root, s_home_root, s_sleep_root };
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
 *   - queue arc: 344×344, ~16 px from the rim, fills -130°..+130°
 *     proportionally to queue_level/8 (+ sub_step) with the taxonomy
 *     colour
 *   - centre disc: 200 px soft-bg circle holding the status word
 *
 * Cabin-tick ring is intentionally absent in this build — single-device
 * setups don't have peer cabins to display, the ticks read as
 * decorative noise. Will reappear with real semantics once the cabin
 * setup steps land.
 */
static void build_home(void)
{
    s_home_root = make_phase_root();

    /* Wedge pool — WEDGE_POOL_SIZE (= 16) pre-allocated lv_arc widgets,
     * one per visible peer. Rounded ends on BOTH track and
     * indicator so the wedge's start/end (track ends) and the
     * fill's start/end (indicator ends) both get semicircular
     * caps in their own colour — no flat ends anywhere. */
    for (int i = 0; i < WEDGE_POOL_SIZE; i++) {
        s_wedge_prev_target[i] = -1;
        lv_obj_t *a = lv_arc_create(s_home_root);
        s_wedge_pool[i] = a;
        lv_obj_remove_style(a, NULL, LV_PART_KNOB);
        lv_obj_set_style_bg_opa(a, LV_OPA_TRANSP, LV_PART_KNOB);
        lv_obj_set_style_pad_all(a, 0, LV_PART_KNOB);
        lv_obj_clear_flag(a, LV_OBJ_FLAG_CLICKABLE);
        lv_arc_set_range(a, 0, 100);
        lv_arc_set_value(a, 0);
        lv_obj_set_style_arc_rounded(a, true, LV_PART_INDICATOR);
        lv_obj_set_style_arc_rounded(a, true, LV_PART_MAIN);
        lv_obj_add_flag(a, LV_OBJ_FLAG_HIDDEN);
    }

    /* Peer-number labels — Mont 22 (bumped from 16 to match the
     * thicker 20 px ring band), centred at wedge mid-radius/
     * mid-angle each render. Hidden by default. */
    for (int i = 0; i < MAX_PEERS_IN_VIEW; i++) {
        lv_obj_t *l = lv_label_create(s_home_root);
        s_peer_num[i] = l;
        lv_obj_set_style_text_font(l, &lv_font_montserrat_22, 0);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(COL_FG), 0);
        lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
    }

    /* Own end-of-fill ball — 18 px circle (a bit bigger than the
     * 16 px ring stroke so it visibly POPS off the ring). White
     * 2 px border for contrast against any wedge colour. Filled
     * with own's identity colour. This is now the SOLE "this is
     * YOUR wedge" marker (hairline guide removed). */
    s_own_end_ball = lv_obj_create(s_home_root);
    lv_obj_set_size(s_own_end_ball, 18, 18);
    lv_obj_set_style_radius(s_own_end_ball, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_own_end_ball, lv_color_hex(COL_FG), 0);
    lv_obj_set_style_bg_opa(s_own_end_ball, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_own_end_ball, lv_color_hex(COL_FG), 0);
    lv_obj_set_style_border_width(s_own_end_ball, 2, 0);
    lv_obj_set_style_pad_all(s_own_end_ball, 0, 0);
    lv_obj_clear_flag(s_own_end_ball, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_own_end_ball, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_own_end_ball, LV_OBJ_FLAG_HIDDEN);

    /* Centre disc — TRUE FULL CIRCLE, centred on screen. Earlier
     * builds clipped the bottom with a chord mask + chord stroke
     * to make room for the SEND text; that's gone now, the disc
     * paints as a complete circle and the SEND labels sit in the
     * angular gap below the rings without needing the disc to
     * step out of the way. Queue rings are drawn concentrically
     * around this same centre.
     *
     * Border (2 px) ALWAYS tints with status colour — see
     * apply_home(). */
    s_home_center = lv_obj_create(s_home_root);
    /* 120 px (radius 60) — shrunk another ~15 % per user request.
     * Slot 3 inner is at radius 74 → 14 px gap to the disc, lots
     * of breathing room between the innermost ring and the disc
     * border now that the central elements use Mont 24. */
    lv_obj_set_size(s_home_center, 120, 120);
    lv_obj_center(s_home_center);
    lv_obj_set_style_bg_color(s_home_center, lv_color_hex(COL_BG_SOFT), 0);
    lv_obj_set_style_bg_opa(s_home_center, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_home_center, 4, 0);
    lv_obj_set_style_border_color(s_home_center, lv_color_hex(COL_TRACK), 0);
    lv_obj_set_style_radius(s_home_center, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(s_home_center, 0, 0);
    lv_obj_clear_flag(s_home_center, LV_OBJ_FLAG_SCROLLABLE);
    /* CLICKABLE cleared so taps on the centre pass through the
     * hit-test and land on the phase root (which is the ONLY
     * clickable widget — keeps all input on one well-known target). */
    lv_obj_clear_flag(s_home_center, LV_OBJ_FLAG_CLICKABLE);

    /* FR identity label + centre status word — both Mont 24. FR
     * stays at offset -28 (centre y=152). Status was at +28 but the
     * 2-line "LONG\nQUE" extended to y=234, kissing the disc border
     * (disc bottom y=240). Lifted to +18 (centre y=198) so the
     * 2-line case bottoms out at y=224 with 16 px clear to the
     * disc edge. The block is no longer perfectly centred but the
     * gap to the disc-border is now consistent top vs bottom. */
    s_home_fr = make_label(s_home_center, "FR1",
        &lv_font_montserrat_24, 0xFF7520, 4, LV_ALIGN_CENTER, 0, -28);

    s_home_status = make_label(s_home_center, "EMPTY",
        &lv_font_montserrat_24, COL_ACCENT, 4, LV_ALIGN_CENTER, 0, 18);
    lv_obj_set_style_text_align(s_home_status, LV_TEXT_ALIGN_CENTER, 0);

    /* SEND>X advisor render — two labels parented DIRECTLY to
     * s_home_root (no wrapper box). Lifted again (+10 px) so the
     * pair sits at y=295/y=325, closer to the disc bottom but
     * still inside the queue arc's 100° bottom gap (40°-140°).
     * Slot 3 (innermost ring) outer radius is 109 → its south
     * edge would be at y=289; SEND main centred at y=295 has its
     * top ~y=281, comfortably inside the angular gap so no wedge
     * can overlap. */
    s_send_main = make_label(s_home_root, "",
        &lv_font_montserrat_28, COL_RED, 2, LV_ALIGN_CENTER, 0, 115);
    /* Enable LVGL inline recolor on the hero line so apply_home()
     * can tint just the target NUMBER in that peer's identity
     * colour while "SEND >" stays in the urgent-red base. Uses
     * #RRGGBB ...# syntax in the label text. */
    lv_label_set_recolor(s_send_main, true);
    s_send_subtitle = make_label(s_home_root, "",
        &lv_font_montserrat_14, COL_FG_DIM, 1, LV_ALIGN_CENTER, 0, 145);
    lv_obj_add_flag(s_send_main,     LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_send_subtitle, LV_OBJ_FLAG_HIDDEN);

    /* Inactivity overlay — full-disc scrim with the CONFIRM STATUS
     * prompt. CLICKABLE cleared so the visual scrim doesn't intercept
     * touches — they pass through to the phase root (the only
     * clickable widget) where screen_event_cb dispatches dismiss /
     * long-press. The overlay still PAINTS over the disc; it just
     * doesn't claim hit-test. */
    s_inactive_overlay = lv_obj_create(s_home_root);
    lv_obj_set_size(s_inactive_overlay, 360, 360);
    lv_obj_center(s_inactive_overlay);
    lv_obj_set_style_bg_color(s_inactive_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_inactive_overlay, LV_OPA_70, 0);
    lv_obj_set_style_border_width(s_inactive_overlay, 0, 0);
    lv_obj_set_style_radius(s_inactive_overlay, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(s_inactive_overlay, 0, 0);
    lv_obj_clear_flag(s_inactive_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_inactive_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_inactive_overlay, LV_OBJ_FLAG_HIDDEN);

    /* Blinking yellow "CONFIRM STATUS" — Mont 22 hero. Mirrors the
     * simulator's #FACC15 urgency colour. apply_home toggles opacity
     * for the 1 Hz blink. Sits slightly above centre to leave room
     * for the dismiss hint underneath. */
    s_inactive_top = make_label(s_inactive_overlay, "CONFIRM STATUS",
        &lv_font_montserrat_22, 0xFACC15, 4, LV_ALIGN_CENTER, 0, -16);

    /* Dim hint at the bottom — single short line fits the round bezel
     * comfortably without wrapping. */
    s_inactive_hint = make_label(s_inactive_overlay, "TAP TO DISMISS ALERT",
        &lv_font_montserrat_10, COL_FG_DIM, 2, LV_ALIGN_CENTER, 0, 24);

    /* Final z-order — LVGL draws children in creation order, so
     * everything that should appear ABOVE the disc + SEND labels
     * gets moved to the foreground here. Order matters: each
     * move_foreground places its target at the END of the parent's
     * child list, so the LAST call ends up topmost.
     *
     * Bottom-up:
     *   1. wedges (queue status rings) — kept on top so nothing
     *      below them (disc border, SEND text bg, etc.) can clip
     *      the wedge endpoints at the 140°/40° gap edges.
     *   2. peer-number labels — sit on top of their wedges so the
     *      digit is legible against the wedge fill.
     *   3. end-of-fill ball — sole own-wedge marker (hairline guide
     *      was removed; the ball + white border already reads as
     *      "this is yours" without the extra arc).
     *   4. inactivity overlay — topmost so the CONFIRM STATUS
     *      scrim dims everything beneath it. */
    for (int i = 0; i < WEDGE_POOL_SIZE; i++) {
        lv_obj_move_foreground(s_wedge_pool[i]);
    }
    for (int i = 0; i < MAX_PEERS_IN_VIEW; i++) {
        lv_obj_move_foreground(s_peer_num[i]);
    }
    lv_obj_move_foreground(s_own_end_ball);
    lv_obj_move_foreground(s_inactive_overlay);
}

static void apply_home(const app_state_t *state)
{
    uint8_t level = state->queue_level;
    if (level < 1) level = 1; else if (level > 5) level = 5;
    uint32_t color = color_for_level(level);

    /* Type-toggle re-seed: when the encoder long-press flips
     * device_type (FR↔T), the old-type peers in the registry would
     * naturally drop out of the type-filtered iteration below — but
     * in sim mode that leaves the ring lonely (just own, no
     * simulated peers of the new type). Re-seed the registry with
     * fresh random peers of the NEW type. Runtime-gated by either
     * the APP_PEER_SIM compile flag OR per-device MAC enrolment in
     * device_role.c, so a single physical knob can stay in demo
     * mode even when the rest of the fleet OTAs to APP_PEER_SIM=0. */
    if (APP_PEER_SIM || device_role_is_demo()) {
        static device_type_t prev_type = (device_type_t)-1;
        if (prev_type != (device_type_t)-1 && prev_type != state->device_type) {
            peer_sim_populate(state);
        }
        prev_type = state->device_type;
    }

    /* ── peer registry sync + tier resolution ────────────────────────
     * Mirror the own device's current identity/level into the
     * registry so the rendering pipeline reads from a single source
     * of truth (works for own today, works for own+peers tomorrow
     * once mesh feeds peer_registry_upsert). The tier + slot/wedge
     * resolution is computed every frame; with only the own device
     * online the tier is FULL and the own device lands in slot 0,
     * wedge 0 — the outermost full 260° ring, matching what the
     * UI showed before this refactor. Drives the wedge pool below.
     *
     * Diagnostic log fires once per tier change so tier transitions
     * stay visible in the serial monitor as peers are simulated. */
    peer_registry_sync_own(state);
    uint8_t same_type_count = peer_registry_count_of_type(state->device_type);
    peer_tier_t tier = peer_tier_for_count(same_type_count);

    /* Compute the MAX rings any wedge will hold this frame, which
     * picks the ring geometry table (thin = stroke 26 when ≤3,
     * full = stroke 20 when ==4). ceil(total / wedges_per_ring). */
    uint8_t W_now = peer_tier_wedges_per_ring(tier);
    uint8_t max_rings = (uint8_t)((same_type_count + W_now - 1) / W_now);
    const ring_slot_geom_t *kRingSlots = active_ring_slots(max_rings);

    static peer_tier_t prev_tier = (peer_tier_t)-1;
    static uint8_t     prev_count = 255;
    if (tier != prev_tier || same_type_count != prev_count) {
        prev_tier = tier;
        prev_count = same_type_count;
        ESP_LOGI(TAG, "peer-tier %d (count=%u, wedges/ring=%u, max_rings=%u, stroke=%u)",
                 (int)tier, (unsigned)same_type_count,
                 (unsigned)W_now, (unsigned)max_rings,
                 (unsigned)kRingSlots[0].stroke);
    }

    /* OWN target uses the (level + sub_step) accumulator so detents
     * animate the arc smoothly between discrete levels. Peers from
     * the mesh don't have a sub_step (they only ship discrete
     * levels), so their targets are just (level-1)/4 * 100.
     *
     * Progress range now spans 0..14 (was 0..12) so LONG QUE (level 5)
     * gets its own 3-substep band — sub 0 at 85.7 %, sub 2 at 100 %.
     * Without this widening, LONG QUE always rendered as a fully-
     * filled wedge regardless of which detent inside LONG QUE we
     * were on, hiding the sub-step granularity the encoder produces
     * and the advisor consumes. */
    enum { STEP = 3 };  /* must match DETENTS_PER_STEP in phase_manager.c */
    enum { PROG_MAX = 5 * STEP - 1 };   /* 14 — full sub-step range across 5 levels */
    int sub = state->queue_sub_step;
    if (sub > STEP - 1)  sub = STEP - 1;
    if (sub < -(STEP - 1)) sub = -(STEP - 1);
    int prog = (level - 1) * STEP + sub;
    if (prog < 0) prog = 0; else if (prog > PROG_MAX) prog = PROG_MAX;
    int own_target = prog * 100 / PROG_MAX;

    uint32_t fr_col = device_color_for_number(state->device_number);

    /* Tier OR slot-geometry transition: either changes the radii /
     * stroke / angles of EVERY wedge, so old animation targets
     * become meaningless. Reset prev_target on every pool entry
     * so the next configure-pass re-animates from the new baseline.
     * Geometry-only transitions happen when peer count crosses the
     * thin↔full threshold within the same tier (e.g. 6→7 peers in
     * HALF flips max_rings 3→4). */
    static const ring_slot_geom_t *s_prev_slots = NULL;
    if (tier != s_prev_tier || kRingSlots != s_prev_slots) {
        s_prev_tier  = tier;
        s_prev_slots = kRingSlots;
        for (int i = 0; i < WEDGE_POOL_SIZE; i++) {
            s_wedge_prev_target[i] = -1;
        }
    }

    /* Walk same-type peers in number-ascending order, assigning each
     * to the next pool slot. Track the OWN device's pool index so
     * the hairline guide can follow it. */
    const wedge_angle_t *wedges = peer_tier_wedge_table(tier);
    peer_iter_t it;
    peer_iter_start(&it, state->device_type);

    int peer_count = 0;
    int own_pool_idx = -1;
    uint8_t own_ring_slot = 0, own_ring_wedge = 0;
    const peer_t *p;

    while ((p = peer_iter_next(&it)) != NULL && peer_count < MAX_PEERS_IN_VIEW) {
        uint8_t slot, wedge;
        peer_slot_for_index((uint8_t)peer_count, tier, same_type_count,
                            &slot, &wedge);
        const ring_slot_geom_t *gm = &kRingSlots[slot];
        const wedge_angle_t    *an = &wedges[wedge];

        uint32_t pa = device_color_for_number(p->number);
        bool is_own = (p->type == state->device_type &&
                       p->number == state->device_number);

        int p_target;
        if (is_own) {
            p_target = own_target;
            own_pool_idx = peer_count;
            own_ring_slot = slot;
            own_ring_wedge = wedge;
        } else {
            /* Mirror the own-target formula so peer fills also
             * reflect sub_step granularity (covers LONG QUE 85→100 %
             * band and every other level's micro-positions). Real-
             * mesh peers default sub_step to 0 until the broadcast
             * protocol carries it; sim peers carry randomised
             * sub_steps via peer_sim_populate. */
            int pl = p->queue_level;
            if (pl < 1) pl = 1; else if (pl > 5) pl = 5;
            int psub = p->sub_step;
            if (psub > STEP - 1) psub = STEP - 1;
            if (psub < -(STEP - 1)) psub = -(STEP - 1);
            int pprog = (pl - 1) * STEP + psub;
            if (pprog < 0) pprog = 0;
            else if (pprog > PROG_MAX) pprog = PROG_MAX;
            p_target = pprog * 100 / PROG_MAX;
        }
        /* Peer freshness: own (slot 0) is always fresh because
         * sync_own runs every render. Other peers go "stale" when
         * we haven't received their broadcast for MESH_PEER_STALE_MS
         * — UI fades them to 50 % opacity so the user sees the
         * connection wobbling without the peer disappearing. Frame
         * recovers automatically when the next broadcast arrives. */
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
        bool stale = !is_own && peer_is_stale(p, now_ms, 3000U);

        update_peer_wedge(peer_count, gm, an, pa, COL_TRACK, p_target, stale);

        /* Peer number label — show for EVERY peer including own.
         * Carrying the own number on its wedge means the identity
         * reads from the ring without needing to look at the
         * centre disc, which is cleaner now that the disc has
         * shrunk. Colour flips based on the peer's fill level:
         * low levels (1..2) → primary neon (legible against the
         * unfilled dark wedge area); high levels (3..5) → COL_BG
         * black (legible against the bright filled wedge area).
         * Stale peers' label also fades to 50% opacity. */
        lv_obj_t *num = s_peer_num[peer_count];
        {
            int mid_r     = (gm->outer_r + gm->inner_r) / 2;
            int mid_angle = an->rotation + an->span / 2;
            int sx, sy;
            polar_to_screen(mid_r, mid_angle, &sx, &sy);

            char nbuf[4];
            snprintf(nbuf, sizeof(nbuf), "%u", (unsigned)p->number);
            lv_label_set_text(num, nbuf);

            uint32_t text_col = (p->queue_level >= 3) ? COL_BG : pa;
            lv_obj_set_style_text_color(num, lv_color_hex(text_col), 0);
            lv_obj_set_style_text_opa(num, stale ? LV_OPA_50 : LV_OPA_COVER, 0);

            /* Centre the label on (sx, sy) by aligning to
             * screen-relative offset from the home root's centre. */
            lv_obj_align(num, LV_ALIGN_CENTER, sx - 180, sy - 180);
            lv_obj_clear_flag(num, LV_OBJ_FLAG_HIDDEN);
        }

        peer_count++;
    }

    /* Hide widget slots for peers not present this frame. */
    for (int i = peer_count; i < MAX_PEERS_IN_VIEW; i++) {
        hide_peer_wedge(i);
        lv_obj_add_flag(s_peer_num[i], LV_OBJ_FLAG_HIDDEN);
    }

    /* End-of-fill ball — sits on the own wedge at the angle where
     * the current fill terminates. The white-bordered identity-
     * coloured dot is now the SOLE marker for "this wedge is
     * yours" (hairline guide removed). Helps the user spot their
     * own ring instantly in crowded multi-ring tiers. Position
     * computed from own_target (0..100) mapped onto the own
     * wedge's angular range. */
    if (own_pool_idx >= 0) {
        const ring_slot_geom_t *og = &kRingSlots[own_ring_slot];
        const wedge_angle_t    *ow = &wedges[own_ring_wedge];
        int mid_r = (og->outer_r + og->inner_r) / 2;
        int fill_angle = ow->rotation + (own_target * ow->span) / 100;
        int bx, by;
        polar_to_screen(mid_r, fill_angle, &bx, &by);
        /* Ball diameter tracks the active ring stroke so it always
         * pokes out from the wedge edges. +4 px per side gives a
         * 2 px overhang above and below the ring fill regardless of
         * which geometry table is active (stroke 20/25/30). */
        int ball_d = og->stroke + 4;
        lv_obj_set_size(s_own_end_ball, ball_d, ball_d);
        lv_obj_align(s_own_end_ball, LV_ALIGN_CENTER, bx - 180, by - 180);
        lv_obj_set_style_bg_color(s_own_end_ball, lv_color_hex(fr_col), 0);
        lv_obj_set_style_border_color(s_own_end_ball,
                                       lv_color_hex(0xFFFFFF), 0);
        lv_obj_clear_flag(s_own_end_ball, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_own_end_ball, LV_OBJ_FLAG_HIDDEN);
    }

    /* Centre disc border ALWAYS tints with status colour now (no
     * level>=4 threshold). Combined with the status text inside it
     * the disc is the only place colour reflects urgency — outside
     * ring stays steady in the FR colour. */
    lv_obj_set_style_border_color(s_home_center, lv_color_hex(color), 0);

    /* Identity label text + colour. format_device_label() handles
     * the type prefix (FR vs T) so a long-press toggle on the
     * encoder switches the displayed prefix on the next render. */
    char id_buf[8];
    format_device_label(state, id_buf, sizeof(id_buf));
    lv_label_set_text(s_home_fr, id_buf);
    lv_obj_set_style_text_color(s_home_fr, lv_color_hex(fr_col), 0);

    /* Centre status word colours with the level. */
    /* Downshift the font ONLY for the long "LONG QUE" label (8 chars
     * incl. space) so it fits the disc; all other labels keep the
     * full 28 px size. Letter-spacing also tightens for the long
     * label so it doesn't kiss the queue ring.
     *
     * DOTO INTEGRATION POINT — per the Nothing design skill, this
     * status word is a "hero moment" and should render in Doto
     * (dot-matrix display font). Doto isn't bundled with LVGL; to
     * land it:
     *   1. Grab Doto.ttf from Google Fonts.
     *   2. Run it through https://lvgl.io/tools/fontconverter at
     *      size 36, bpp=4, range 0x20-0x7F.
     *   3. Drop the resulting C file into src/, declare:
     *        extern const lv_font_t lv_font_doto_36;
     *   4. Replace lv_font_montserrat_36 reference below.
     * Until then, Montserrat carries the slot. */
    const char *txt = label_for_level(level);
    /* "LONG QUE" is 8 chars and overflows the disc at Mont 36 — split
     * to two lines so it fits while keeping the same font size as
     * EMPTY / FREE / FULL / QUE. All other labels render single-line. */
    const char *display_txt = txt;
    if (strcmp(txt, "LONG QUE") == 0) {
        display_txt = "LONG\nQUE";
    }
    lv_obj_set_style_text_font(s_home_status, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_letter_space(s_home_status, 4, 0);
    lv_label_set_text(s_home_status, display_txt);
    lv_obj_set_style_text_color(s_home_status, lv_color_hex(color), 0);

    /* EMPTY is the widest single-line status (5 chars vs 3-4 for the
     * others) and its right/left edges were grazing the 4-px disc
     * border at the default offset +18. Lift this label only — every
     * other status keeps its build-time position. Offset +10 puts
     * EMPTY at y=190, where the disc inner width is ~111 px (vs
     * ~106 at y=198), restoring the clearance the old 2-px border
     * provided. */
    int status_off_y = 18;
    if (strcmp(txt, "EMPTY") == 0) {
        status_off_y = 10;
    }
    lv_obj_align(s_home_status, LV_ALIGN_CENTER, 0, status_off_y);

    /* Bottom message box — three mutually-exclusive modes:
     *   1. ADVISOR alert: "SEND > N" + "(LEVEL)" subtitle. Live when
     *      advisor found a receiver (state->alert_active).
     *   2. STUCK reminder: cycling 3 PSA messages in two rows. Lives
     *      when own is QUE/LONG QUE (level>=4) AND advisor inactive —
     *      i.e. user is full but nowhere to send. Cycles every 3 s.
     *   3. Hidden: everything else.
     *
     * Both modes re-set font + colour every frame so switching
     * between them doesn't leak styling from the previous mode. */
    bool show_reminder = !state->alert_active && state->queue_level >= 4;

    if (show_reminder) {
        static const struct { const char *l1; const char *l2; } kReminders[] = {
            { "HANDOUT",  "BASKETS"  },
            { "PROMOTE",  "APP"      },
            { "THANK",    "PATIENCE" },
        };
        enum { N_REMINDERS = sizeof(kReminders) / sizeof(kReminders[0]) };

        static uint32_t s_rem_last_ms = 0;
        static uint8_t  s_rem_idx     = 0;
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
        if (s_rem_last_ms == 0) {
            s_rem_last_ms = now_ms;
        } else if ((now_ms - s_rem_last_ms) >= 3000U) {
            s_rem_idx = (uint8_t)((s_rem_idx + 1) % N_REMINDERS);
            s_rem_last_ms = now_ms;
        }

        /* Row 1 + row 2 share Mont 22 + amber so the PSA reads as a
         * single 2-line block. Amber (0xFFB020) is the "attention,
         * not emergency" colour — distinct from the red advisor
         * SEND line. */
        lv_label_set_text(s_send_main, kReminders[s_rem_idx].l1);
        lv_obj_set_style_text_font(s_send_main, &lv_font_montserrat_22, 0);
        lv_obj_set_style_text_color(s_send_main, lv_color_hex(0xFFB020), 0);

        lv_label_set_text(s_send_subtitle, kReminders[s_rem_idx].l2);
        lv_obj_set_style_text_font(s_send_subtitle, &lv_font_montserrat_22, 0);
        lv_obj_set_style_text_color(s_send_subtitle, lv_color_hex(0xFFB020), 0);

        lv_obj_clear_flag(s_send_main,     LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_send_subtitle, LV_OBJ_FLAG_HIDDEN);
    } else if (state->alert_active) {
        /* SEND>X advisor render. Hero line "SEND > N" uses the
         * urgent variant of red when self is at LONG QUE; the
         * subtitle "(LEVEL)" uses the TARGET peer's status colour.
         * Tint the target NUMBER via LVGL's inline #RRGGBB N#
         * recolor so the digit visually matches the wedge it's
         * pointing at. Font sizes RESTORED here in case the prior
         * frame was in reminder mode. */
        uint32_t target_col = device_color_for_number(state->alert_target);
        char main_buf[32];
        snprintf(main_buf, sizeof(main_buf), "SEND > #%06X %u#",
                 (unsigned)(target_col & 0xFFFFFF),
                 (unsigned)state->alert_target);
        lv_label_set_text(s_send_main, main_buf);
        lv_obj_set_style_text_font(s_send_main, &lv_font_montserrat_28, 0);
        lv_obj_set_style_text_color(s_send_main,
            lv_color_hex(state->alert_urgent ? 0xFF1A1A : COL_RED), 0);

        const char *tlabel = label_for_level(state->alert_target_level);
        char sub_buf[24];
        snprintf(sub_buf, sizeof(sub_buf), "(%s)%s",
                 tlabel, state->alert_trend_down ? " \xE2\x86\x93" : "");
        lv_label_set_text(s_send_subtitle, sub_buf);
        lv_obj_set_style_text_font(s_send_subtitle, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(s_send_subtitle,
            lv_color_hex(color_for_level(state->alert_target_level)), 0);

        lv_obj_clear_flag(s_send_main,     LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_send_subtitle, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_send_main,     LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_send_subtitle, LV_OBJ_FLAG_HIDDEN);
    }

    /* Inactivity overlay driver. Edge-detect on state->inactive
     * starts/aborts the alert SEQUENCE in inactivity_alert.c. The
     * overlay's visibility is then gated by
     * inactivity_alert_overlay_visible() so it shows ONLY during
     * the ALERT macro phases — hidden during BREAK phases between
     * alerts (per the new 10s-on / 10s-off cadence). */
    static bool prev_inactive = false;
    if (state->inactive != prev_inactive) {
        prev_inactive = state->inactive;
        inactivity_alert_set(state->inactive);
    }

    bool overlay_should_show = inactivity_alert_overlay_visible();
    static bool overlay_visible = false;
    if (overlay_should_show != overlay_visible) {
        overlay_visible = overlay_should_show;
        if (overlay_visible) {
            lv_obj_clear_flag(s_inactive_overlay, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_inactive_overlay, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (overlay_visible) {
        /* 1 Hz blink on the CONFIRM STATUS label — opacity toggle.
         * Hint stays solid so the dismiss instruction is always
         * readable. */
        bool on = ((esp_timer_get_time() / 500000ULL) & 1ULL) == 0;
        lv_obj_set_style_text_opa(s_inactive_top,
            on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    }
}

/* ───────────────────────────── ADMIN screen ──────────────────────────── */

/* ───────────────────────────── SLEEP screen ──────────────────────────── */

static void build_sleep(void)
{
    /* Simulator's renderSleep fills the disc with pure black — no text,
     * no animation — matching real-hardware backlight-off behaviour.
     * Tap-to-wake still works because the phase root catches the click.
     *
     * Override the make_phase_root() dotted bg: SLEEP needs pure
     * black to match the panel-off feel. */
    s_sleep_root = make_phase_root();
    lv_obj_set_style_bg_img_opa(s_sleep_root, LV_OPA_TRANSP, 0);
}

static void apply_sleep(const app_state_t *state) { (void)state; }

/* ───────────────────────────── render ────────────────────────────────── */

static void apply_state(const app_state_t *state)
{
    show_only(state->phase);
    switch (state->phase) {
    case PH_BOOT:  apply_boot(state);  break;
    case PH_HOME:  apply_home(state);  break;
    /* PH_ADMIN removed in this build. */
    case PH_SLEEP: apply_sleep(state); break;
    default:       apply_home(state);  break;  /* unhandled phase → home */
    }
}

/* Screen-level input dispatcher. Per-phase-root registration (see
 * make_phase_root). Three event codes handled:
 *
 *   PRESSED       → INSTANT alert dismiss if active. Doesn't fire
 *                   short-tap action; that's handled by CLICKED on
 *                   release. This makes the tap-to-dismiss feel
 *                   immediate rather than waiting ~120 ms for the
 *                   release-driven CLICKED.
 *   CLICKED       → phase_manager_on_button(true, 0)
 *                   Short-tap action (also dismisses, idempotent).
 *   LONG_PRESSED  → phase_manager_on_button(true, 1500)
 *                   Toggles device type (FR ↔ T). */
static void screen_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        if (phase_manager_dismiss_alert_if_active()) {
            ESP_LOGI(TAG, "screen PRESS → alert dismissed (instant)");
        }
    } else if (code == LV_EVENT_CLICKED) {
        /* Triple-tap → screenshot. Window is 600 ms between
         * consecutive clicks; the 3rd hit triggers and we swallow
         * its normal short-tap dispatch so the screenshot tap
         * doesn't also nudge phase_manager. State is static — only
         * one source of CLICKED (this handler) so no race. */
        static uint32_t s_click_last_ms = 0;
        static uint8_t  s_click_count   = 0;
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
        if ((now_ms - s_click_last_ms) > 600U) {
            s_click_count = 1;
        } else {
            s_click_count++;
        }
        s_click_last_ms = now_ms;
        if (s_click_count >= 3) {
            ESP_LOGI(TAG, "screen TRIPLE-TAP → screenshot");
            s_click_count = 0;
            esp_err_t ser = screenshot_capture_and_save();
            if (ser != ESP_OK) {
                ESP_LOGW(TAG, "screenshot failed (%s)", esp_err_to_name(ser));
            }
            return;   /* swallow — don't also fire short-tap */
        }

        ESP_LOGI(TAG, "screen CLICK → button(short)  phase=%d",
                 (int)phase_manager_get_phase());
        phase_manager_on_button(true, 0);
    } else if (code == LV_EVENT_LONG_PRESSED) {
        ESP_LOGI(TAG, "screen LONG → button(long) → type toggle");
        phase_manager_on_button(true, 1500);
    }
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

    /* Peer registry holds the same-type peer list driving the
     * multi-ring layout. Initialised empty; apply_home() seeds
     * slot 0 with the own device every render. Mesh layer (future)
     * will populate slots 1..31 via peer_registry_upsert(). */
    peer_registry_init();

    /* TESTING / DEMO MODE — populate the registry with random same-
     * type peers so the multi-ring layouts can be visualised without
     * a real mesh. Gated by (APP_PEER_SIM || device_role_is_demo()) —
     * the MAC-enrolment path keeps designated "demo unit" knobs in
     * sim mode permanently, surviving any OTA that flips the compile
     * flag to 0 for the rest of the fleet. */
    if (APP_PEER_SIM || device_role_is_demo()) {
        app_state_t boot_state;
        phase_manager_get_state(&boot_state);
        peer_registry_sync_own(&boot_state);
        peer_sim_populate(&boot_state);
    }

    /* Build the shared dot-matrix tile BEFORE any phase root references it. */
    build_dot_tile();

    build_boot();
    build_home();
    build_sleep();

    /* Touch event handlers are registered directly on each phase
     * root in make_phase_root() — that's the only CLICKABLE widget
     * in each phase's tree, so it catches every tap deterministically.
     * No s_screen-level registration here (LVGL 8 doesn't bubble
     * events from clickable children to parents without an
     * explicit EVENT_BUBBLE flag, which the earlier setup
     * incorrectly assumed). */

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
