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
 *   PH_SLEEP   — black screen (short tap or rotate to wake to HOME)
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
static uint32_t s_last_input_ms  = 0;

#define BOOT_HOLD_MS         1000U
#define SLEEP_TIMEOUT_MS    60000U   /* idle → SLEEP after 60s */

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

    /* Wake from sleep on any rotation. */
    if (s_state.phase == PH_SLEEP) {
        phase_enter(PH_HOME, now);
        return;
    }

    switch (s_state.phase) {
    case PH_HOME:
        /* Queue level 1..8 with hard clamp (no wrap — matches simulator). */
        if (delta > 0 && s_state.queue_level < 8) {
            s_state.queue_level++;
        } else if (delta < 0 && s_state.queue_level > 1) {
            s_state.queue_level--;
        }
        s_state.status_level = s_state.queue_level;
        break;

    case PH_ADMIN:
        /* In ADMIN: rotate cabin count 0..16 (the one user-tunable setting
         * we expose right now). */
        if (delta > 0 && s_state.cab_count < 16) {
            s_state.cab_count++;
        } else if (delta < 0 && s_state.cab_count > 0) {
            s_state.cab_count--;
        }
        break;

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

    /* Any press wakes from sleep. */
    if (s_state.phase == PH_SLEEP) {
        phase_enter(PH_HOME, now);
        return;
    }

    /* Long-press: toggle HOME ↔ ADMIN. */
    if (duration_ms >= 2000) {
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

    case PH_HOME:
        /* Idle → SLEEP. Sleep is opt-out for now (set SLEEP_TIMEOUT_MS to
         * 0 to disable). */
        if (SLEEP_TIMEOUT_MS > 0 &&
            (now - s_last_input_ms) > SLEEP_TIMEOUT_MS) {
            phase_enter(PH_SLEEP, now);
        }
        break;

    default:
        break;
    }
}
