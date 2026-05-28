/* Phase / state machine for the JC3636K718 UI.
 *
 * Goal of this build (per user spec): single-purpose HOME screen +
 * inactivity sleep. NO setup wizard, NO ADMIN, NO cabin config, NO
 * opening hours. Power on → auto-mesh (later) → HOME.
 *
 * Phases actually used by this build:
 *   PH_BOOT    — splash for ~1s after power-on
 *   PH_HOME    — main queue UI (encoder rotates queue level,
 *                long-press toggles FR↔T device type)
 *
 * Deep SLEEP is intentionally NOT reachable here. In the full product
 * SLEEP only activates outside the operator-configured opening hours
 * (OPEN_HR / CLOSE_HR setup steps). Since this build skips that whole
 * setup chain, there's no schedule to honour and we stay in HOME
 * indefinitely. PH_SLEEP stays defined in the enum so the header API
 * keeps compiling, but no transition lands on it.
 *
 * All other PH_* values stay defined so the header API doesn't break,
 * but no transitions land on them in this build.
 */

#include "phase_manager.h"

#include <string.h>

#include "advisor.h"
#include "app_config.h"
#include "esp_timer.h"
#include "peer_registry.h"

/* 5-level taxonomy (was 8). Traffic-light progression:
 *   EMPTY     dark green  — no traffic
 *   FREE      green       — light traffic
 *   FULL      yellow      — busy but managed (neutral threshold)
 *   QUE       orange      — queue forming (warning)
 *   LONG QUE  red         — alert
 * The level-4-and-up colours also drive the centre disc's warning
 * ring in ui_engine.c. */
static const taxonomy_entry_t kTaxonomy[5] = {
    {"EMPTY",    0x1E6B1E},
    {"FREE",     0x22C55E},
    {"FULL",     0xFACC15},
    {"QUE",      0xFB923C},
    {"LONG QUE", 0xDC2626},
};

static app_state_t s_state = {};
static uint32_t s_phase_enter_ms = 0;
/* Last-input timestamp kept around for future use (opening-hours SLEEP
 * gate, idle UI dimming). No reader in this build, but write-paths
 * stay so the field stays current as input lands. */
static uint32_t s_last_input_ms  = 0;

/* Cumulative encoder progress — single source of truth for the
 * queue level. Range [0, MAX_QUEUE_PROGRESS] where MAX = 5 *
 * DETENTS_PER_STEP - 1 = 14. Each of the 5 status levels gets its
 * own 3-substep band (0..2), so the total span is 15 positions
 * (progress 0..14). LONG QUE now has the same micro-step
 * granularity as the others — previously MAX was 12 which capped
 * LONG QUE at sub_step 0 only, breaking symmetry with EMPTY/FREE/
 * FULL/QUE and starving the advisor's sub_step heuristics of
 * LONG-QUE detail.
 *
 * Derivation per encoder event:
 *   level    = (progress / DETENTS_PER_STEP) + 1   (1..5)
 *   sub_step = progress % DETENTS_PER_STEP         (0..STEP-1)
 *
 * Why a single counter instead of (level + signed sub_step)?
 * Hysteresis-free behaviour. Three detents up commits to FREE; one
 * detent back drops straight to EMPTY because progress moves from
 * 3 → 2 and the derived level is 1 again. The old (level + sub_step)
 * model needed three detents back to undo three forward because
 * sub_step zeroed at every commit boundary — the eye saw the
 * commit but the state didn't remember how far past the boundary
 * it had gone. */
#define MAX_QUEUE_PROGRESS  (5 * DETENTS_PER_STEP - 1)   /* = 14 */
static int s_queue_progress = 0;

#define BOOT_HOLD_MS         1000U

/* Encoder sensitivity: this many raw detents in one direction commits a
 * single status level change. The UI animates the queue arc per raw
 * detent (via app_state.queue_sub_step) while the discrete status word
 * only updates on commits — knob feels responsive without the level
 * changing on every click. Set to 1 for raw 1:1 behaviour. */
#define DETENTS_PER_STEP     3

/* Inactivity timeout — after this long without encoder or touch input
 * while in HOME, the firmware enters "confirm status" alert mode:
 * UI overlay, RGB LED blink, beep. Mirrors the simulator's per-device
 * `getInactMS()` default. */
#define INACTIVITY_TIMEOUT_MS (5U * 60U * 1000U)

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static void phase_enter(phase_t phase, uint32_t now)
{
    s_state.phase = phase;
    s_phase_enter_ms = now;
    s_last_input_ms = now;
}

/* Called after ANY input source mutates queue_level (encoder, future
 * AI, master command, pre-open wake). Syncs the registry with the
 * new own-level, recomputes the redirect advisor, and parks the
 * advice on app_state so the UI reads from one place. Mirrors the
 * "application path" enumerated in [[project-sorting-architecture]]. */
static void after_level_change(void)
{
    peer_registry_sync_own(&s_state);
    advisor_advice_t adv;
    advisor_recompute(&s_state, &adv);
    s_state.alert_active       = adv.active;
    s_state.alert_target       = adv.target_number;
    s_state.alert_target_level = adv.target_level;
    s_state.alert_urgent       = adv.urgent;
    s_state.alert_trend_down   = adv.trend_down;
}

void phase_manager_init(void)
{
    memset(&s_state, 0, sizeof(s_state));
    s_state.phase = PH_BOOT;
    s_state.use_case = USE_CASE_FITTING_ROOM;
    s_state.use_case_sel = USE_CASE_FITTING_ROOM;
    s_state.net_mode = NET_MESH;
    s_state.net_sel = NET_MESH;
    s_state.open_hour = 8;
    s_state.close_hour = 22;
    s_state.cab_count = 4;
    s_state.cab_int = 15;
    s_state.cor_int = 15;
    s_state.queue_level = 1;
    s_state.status_level = 1;
    s_state.queue_sub_step = 0;
    s_queue_progress = 0;
    s_state.device_type   = (device_type_t)APP_DEVICE_TYPE;
    s_state.device_number = APP_DEVICE_NUMBER;
    s_state.nav_mode = NAV_QUEUE;
    s_phase_enter_ms = now_ms();
    s_last_input_ms = s_phase_enter_ms;
}

phase_t phase_manager_get_phase(void)
{
    return s_state.phase;
}

void phase_manager_set_phase(phase_t phase)
{
    phase_enter(phase, now_ms());
}

void phase_manager_get_state(app_state_t *out)
{
    if (out) {
        *out = s_state;
    }
}

uint8_t phase_manager_get_queue_level(void)
{
    return s_state.queue_level;
}

const taxonomy_entry_t *phase_manager_get_taxonomy(uint8_t level)
{
    if (level < 1 || level > 5) {
        return NULL;
    }
    return &kTaxonomy[level - 1];
}

int phase_manager_get_step_index(phase_t phase)
{
    (void)phase;
    return 7;  /* always "HOME" step in this simplified build */
}

void phase_manager_reset_queue(void)
{
    s_state.queue_level     = 1;
    s_state.status_level    = 1;
    s_state.queue_sub_step  = 0;
    s_queue_progress        = 0;
    s_state.inactive        = false;
    s_last_input_ms         = now_ms();
    after_level_change();
}

void phase_manager_set_queue_level(uint8_t level)
{
    if (level < 1) level = 1; else if (level > 5) level = 5;
    /* Snap progress to the level's lower boundary so the arc
     * lands at the start of the level's sub-step range. */
    s_queue_progress       = (level - 1) * DETENTS_PER_STEP;
    s_state.queue_level    = level;
    s_state.status_level   = level;
    s_state.queue_sub_step = 0;
    /* Per spec: do NOT stamp last_user_input_us — this is a
     * non-encoder source (master/AI). Stamping would lock out
     * subsequent AI updates for STAFF_AUTHORITY_WINDOW. */
    after_level_change();
}

/* Post-dismiss debounce: after the alert is dismissed by any input,
 * subsequent encoder rotations within this window are ABSORBED
 * (counted as input — keeps inactivity timer fresh — but do NOT
 * change queue_level). Lets the user dismiss the CONFIRM STATUS
 * prompt with their finger/knob and gather intent before
 * committing a level change. */
#define POST_DISMISS_DEBOUNCE_MS  500U
static uint32_t s_alert_dismissed_ms = 0;

bool phase_manager_dismiss_alert_if_active(void)
{
    if (!s_state.inactive) return false;
    s_state.inactive = false;
    s_alert_dismissed_ms = now_ms();
    s_last_input_ms = s_alert_dismissed_ms;
    return true;
}

void phase_manager_on_encoder(int delta)
{
    if (delta == 0) {
        return;
    }
    uint32_t now = now_ms();
    s_last_input_ms = now;

    /* First rotation while alert is active = dismiss only. The
     * rotation itself is consumed; queue level stays put so the
     * user can deliberately pick a new level on a subsequent
     * rotation. */
    if (s_state.inactive) {
        s_state.inactive = false;
        s_alert_dismissed_ms = now;
        return;
    }
    /* Brief post-dismiss debounce: even after the alert is gone,
     * absorb rotations for POST_DISMISS_DEBOUNCE_MS so the same
     * encoder gesture that dismissed the alert doesn't immediately
     * change the level. After the debounce, rotations behave as
     * usual. */
    if (s_alert_dismissed_ms != 0 &&
        (now - s_alert_dismissed_ms) < POST_DISMISS_DEBOUNCE_MS) {
        return;
    }
    s_alert_dismissed_ms = 0;

    switch (s_state.phase) {
    case PH_HOME: {
        /* Cumulative-position model: each raw detent moves the
         * absolute progress counter by ±1, clamped to [0, MAX]. The
         * level + sub_step are pure functions of the position, so
         * the boundary is symmetric — three up commits to FREE
         * (progress 3); one back drops to EMPTY (progress 2) on
         * the very next detent. No direction-flip reset needed
         * because progress is its own memory. */
        s_queue_progress += delta;
        if (s_queue_progress < 0) s_queue_progress = 0;
        if (s_queue_progress > MAX_QUEUE_PROGRESS) {
            s_queue_progress = MAX_QUEUE_PROGRESS;
        }
        int lvl = (s_queue_progress / DETENTS_PER_STEP) + 1;
        int sub = s_queue_progress % DETENTS_PER_STEP;
        if (lvl > 5) lvl = 5;   /* defensive clamp; unreachable
                                   given progress is clamped to
                                   MAX_QUEUE_PROGRESS=14 above */
        s_state.queue_level    = (uint8_t)lvl;
        s_state.status_level   = (uint8_t)lvl;
        s_state.queue_sub_step = (int8_t)sub;
        after_level_change();
        break;
    }

    default:
        /* ADMIN and other phases no longer reachable in this build. */
        break;
    }
}

void phase_manager_on_button(bool pressed, uint32_t duration_ms)
{
    if (!pressed) {
        return;
    }
    uint32_t now = now_ms();
    s_last_input_ms = now;
    s_state.inactive = false;  /* any input dismisses the CONFIRM alert */

    /* Long-press: toggle device TYPE between FR ↔ T. The same flashed
     * firmware can play either role; long-press in the field lets
     * the operator re-purpose a knob without re-flashing. The number
     * (1..16) persists across the toggle, so FR3 becomes T3 and
     * vice-versa. HOME re-filters its peer list to the new type on
     * the next render tick.
     *
     * Threshold matches the encoder driver's LONG_PRESS_MS (1500).
     * The encoder fires the event immediately at the threshold so
     * the user feels the change before they release. */
    if (duration_ms >= 1500) {
        s_state.device_type =
            (s_state.device_type == DEV_TYPE_FR) ? DEV_TYPE_T : DEV_TYPE_FR;
        return;
    }

    /* Short tap: no-op for now. Reserved for future short-tap
     * meanings (e.g. quick acknowledge of overlay alerts already
     * handled via touch). */
}

void phase_manager_tick(uint32_t now)
{
    switch (s_state.phase) {
    case PH_OFF:
        phase_enter(PH_BOOT, now);
        break;

    case PH_BOOT:
        if (now - s_phase_enter_ms > BOOT_HOLD_MS) {
            phase_enter(PH_HOME, now);
        }
        break;

    case PH_HOME:
        /* Inactivity gate — only set, never cleared here. Input
         * handlers above clear it on the next encoder/touch event. */
        if (!s_state.inactive &&
            (now - s_last_input_ms) >= INACTIVITY_TIMEOUT_MS) {
            s_state.inactive = true;
        }
        break;

    default:
        /* ADMIN / others sit until input drives a transition. No idle →
         * SLEEP timer: deep sleep is reserved for outside-opening-hours
         * in the full setup flow, which this build skips. */
        break;
    }
}
