/* Phase / state machine for the JC3636K718 UI.
 *
 * Goal of this build (per user spec): focus on the simulator-style HOME
 * screen and the ADMIN screen. Skip the full setup wizard — BOOT auto-
 * advances to HOME, and the long setup chain (USE_CASE → NET → ASSIGN →
 * TIME → OPEN_HR → … → START → SPLASH) is bypassed entirely.
 *
 * Phases actually used by this build:
 *   PH_BOOT    — splash for ~1s after power-on
 *   PH_HOME    — main queue UI (encoder rotates queue level)
 *   PH_ADMIN   — settings (long-press from HOME to enter, long-press to exit)
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

#include "app_config.h"
#include "esp_timer.h"

static const taxonomy_entry_t kTaxonomy[8] = {
    {"EMPTY",    0x1E6B1E},
    {"QUIET",    0x22C55E},
    {"OK",       0x86EFAC},
    {"STEADY",   0xFACC15},
    {"BUSY",     0xFB923C},
    {"QUEUE",    0xF97316},
    {"HEAVY",    0xEF4444},
    {"OVERLOAD", 0xDC2626},
};

static app_state_t s_state = {};
static uint32_t s_phase_enter_ms = 0;
/* Last-input timestamp kept around for future use (opening-hours SLEEP
 * gate, idle UI dimming). No reader in this build, but write-paths
 * stay so the field stays current as input lands. */
static uint32_t s_last_input_ms  = 0;

#define BOOT_HOLD_MS         1000U

/* Encoder sensitivity: this many raw detents in one direction commits a
 * single status level change. The UI animates the queue arc per raw
 * detent (via app_state.queue_sub_step) while the discrete status word
 * only updates on commits — knob feels responsive without the level
 * changing on every click. Set to 1 for raw 1:1 behaviour. */
#define DETENTS_PER_STEP     3

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
    if (level < 1 || level > 8) {
        return NULL;
    }
    return &kTaxonomy[level - 1];
}

int phase_manager_get_step_index(phase_t phase)
{
    (void)phase;
    return 7;  /* always "HOME" step in this simplified build */
}

void phase_manager_on_encoder(int delta)
{
    if (delta == 0) {
        return;
    }
    uint32_t now = now_ms();
    s_last_input_ms = now;

    switch (s_state.phase) {
    case PH_HOME: {
        /* Damped queue-level commit. Each raw detent moves sub_step by
         * ±1. When |sub_step| reaches DETENTS_PER_STEP the level
         * commits and sub_step resets. Within the same direction, a
         * partial sub_step persists between detents so the UI's arc
         * animation can interpolate; switching direction wipes the
         * residual so a CCW tick after CW ticks doesn't have to drain
         * the prior accumulation first. */
        int p = (int)s_state.queue_sub_step;
        if ((delta > 0 && p < 0) || (delta < 0 && p > 0)) {
            p = 0;
        }
        p += delta;
        int lvl = (int)s_state.queue_level;
        while (p >=  DETENTS_PER_STEP && lvl < 8) { lvl++; p -= DETENTS_PER_STEP; }
        while (p <= -DETENTS_PER_STEP && lvl > 1) { lvl--; p += DETENTS_PER_STEP; }
        /* At the rails (level 1 or 8) the residual would point past
         * the clamp — zero it so the arc can't visually creep beyond
         * the discrete level. */
        if (lvl >= 8 && p > 0) p = 0;
        if (lvl <= 1 && p < 0) p = 0;
        if (p >  DETENTS_PER_STEP - 1) p =  DETENTS_PER_STEP - 1;
        if (p < -(DETENTS_PER_STEP - 1)) p = -(DETENTS_PER_STEP - 1);
        s_state.queue_level   = (uint8_t)lvl;
        s_state.status_level  = (uint8_t)lvl;
        s_state.queue_sub_step = (int8_t)p;
        break;
    }

    case PH_ADMIN: {
        /* In ADMIN: rotate cabin count 0..16. No damping — admin tweaks
         * are deliberate, raw 1:1 feels right for entering exact counts. */
        int v = (int)s_state.cab_count + delta;
        if (v < 0) v = 0; else if (v > 16) v = 16;
        s_state.cab_count = (uint8_t)v;
        break;
    }

    default:
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

    /* Long-press: toggle HOME ↔ ADMIN. Threshold matches the encoder
     * driver's LONG_PRESS_MS (1500). The encoder fires the event
     * immediately when the threshold is reached during the hold, so
     * the user feels the menu open before they let go. */
    if (duration_ms >= 1500) {
        if (s_state.phase == PH_HOME) {
            phase_enter(PH_ADMIN, now);
        } else if (s_state.phase == PH_ADMIN) {
            phase_enter(PH_HOME, now);
        }
        return;
    }

    /* Short tap: in HOME, toggle nav (queue ↔ cabin focus); in ADMIN, no-op. */
    if (s_state.phase == PH_HOME) {
        s_state.nav_mode = (s_state.nav_mode == NAV_QUEUE) ? NAV_CABIN : NAV_QUEUE;
    }
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

    default:
        /* HOME / ADMIN sit until input drives a transition. No idle →
         * SLEEP timer: deep sleep is reserved for outside-opening-hours
         * in the full setup flow, which this build skips. */
        break;
    }
}
