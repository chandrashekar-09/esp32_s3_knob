#ifndef PHASE_MANAGER_H
#define PHASE_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PH_OFF = 0,
    PH_BOOT,
    PH_NET,
    PH_CONN,
    PH_ASSIGN,
    PH_TIME,
    PH_SYNC,
    PH_CAB_CNT,
    PH_CAB_INT,
    PH_COR_INT,
    PH_ORIENT,
    PH_START,
    PH_SPLASH,
    PH_HOME,
    PH_SLEEP,
    PH_OPEN_HR,
    PH_CLOSE_HR,
    PH_ADMIN,
    PH_ONLINE_QR,
    PH_WIFI_CONN,
    PH_ONLINE_REG,
    PH_USE_CASE,
    PH_LOC_PICK,
    PH_LOC_NAMING,
    PH_CLEANING_HUB,
    PH_CLEANING_HUB_FR
} phase_t;

typedef enum {
    USE_CASE_FITTING_ROOM = 0,
    USE_CASE_TILLS,
    USE_CASE_GASTRO,
    USE_CASE_RESTROOMS,
    USE_CASE_COUNT
} app_use_case_t;

typedef enum {
    NET_STAR = 0,
    NET_MESH,
    NET_ONLINE,
    NET_MODE_COUNT
} app_net_mode_t;

typedef enum {
    MASTER_SELECT = 0,
    MASTER_ADJUST
} app_master_mode_t;

typedef enum {
    NAV_QUEUE = 0,
    NAV_CABIN
} app_nav_mode_t;

/* Device identity TYPE. Pairs with device_number (1..16) to form a
 * full identity like FR1 or T13. The mesh holds up to 32 knobs
 * (16 of each type) and HOME filters peers by matching type. */
typedef enum {
    DEV_TYPE_FR = 0,   /* fitting room */
    DEV_TYPE_T  = 1,   /* till */
} device_type_t;

typedef struct {
    phase_t phase;
    app_use_case_t use_case;
    app_use_case_t use_case_sel;
    app_net_mode_t net_mode;
    app_net_mode_t net_sel;
    uint16_t time_fields[5];
    uint8_t time_idx;
    uint8_t open_hour;
    uint8_t close_hour;
    uint8_t cab_count;
    uint8_t cab_int;
    uint8_t cor_int;
    uint8_t orient_pos;
    uint8_t queue_level;
    uint8_t status_level;
    /* Device identity — type + number. Together they form e.g.
     * "FR1" or "T13". Sourced from APP_DEVICE_TYPE / _NUMBER at
     * boot; encoder long-press toggles device_type at runtime. */
    device_type_t device_type;
    uint8_t       device_number;   /* 1..16 */
    /* Sub-step accumulator: -(STEP-1)..+(STEP-1). Each raw encoder
     * detent adjusts this by ±1; when |queue_sub_step| reaches STEP, the
     * level commits and the accumulator resets to 0. The UI uses this
     * to animate the queue arc continuously while the discrete status
     * word only changes on level commits. */
    int8_t queue_sub_step;
    bool is_master;
    app_master_mode_t master_mode;
    uint8_t master_target;
    app_nav_mode_t nav_mode;
    /* Inactivity flag — true when the user hasn't touched the encoder
     * or tapped the screen for INACTIVITY_TIMEOUT_MS while in HOME.
     * Mirrors quesort_simulator.html's `idle >= getInactMS(dev)` gate
     * that drives the CONFIRM STATUS overlay + RGB blink + beep. */
    bool inactive;
    /* Redirect-advisor alert. Populated by advisor_recompute() after
     * every queue_level mutation (or peer update). Drives the
     * SEND>X line in the message box. See [[project-sorting-architecture]]. */
    bool    alert_active;       /* render SEND line at all */
    uint8_t alert_target;       /* peer number to redirect to (1..16) */
    uint8_t alert_target_level; /* target's level — drives subtitle */
    bool    alert_urgent;       /* self LONG QUE → brighter red */
    bool    alert_trend_down;   /* target clearing → "↓" marker */
} app_state_t;

typedef struct {
    const char *label;
    uint32_t color_hex;
} taxonomy_entry_t;

void phase_manager_init(void);
phase_t phase_manager_get_phase(void);
void phase_manager_set_phase(phase_t phase);

void phase_manager_on_encoder(int delta);
void phase_manager_on_button(bool pressed, uint32_t duration_ms);
void phase_manager_tick(uint32_t now_ms);

uint8_t phase_manager_get_queue_level(void);
const taxonomy_entry_t *phase_manager_get_taxonomy(uint8_t level);
void phase_manager_get_state(app_state_t *out);
int phase_manager_get_step_index(phase_t phase);

/* Reset the queue back to EMPTY (level 1) + clear the inactive flag +
 * zero the sub-step accumulator. Used by the inactivity alert
 * sequence as the "give up" action before entering deep sleep. */
void phase_manager_reset_queue(void);

/* Absolute level set (1..5). Used by non-encoder input sources
 * (master command, future AI suggestions) that supply a target
 * level directly rather than a relative detent. Snaps progress to
 * the level's lower boundary, recomputes advisor. Per spec
 * [[project-sorting-architecture]] §1d, does NOT stamp
 * last_user_input_us (that's reserved for manual rotation). */
void phase_manager_set_queue_level(uint8_t level);

/* Clear the inactivity alert flag if set. Returns true if the
 * alert was active when called (caller can swallow the input
 * that triggered the dismiss). Touched on every input source
 * that should act as "wake from alert" — first encoder rotation
 * after alert, first tap, etc. Idempotent. */
bool phase_manager_dismiss_alert_if_active(void);

/* Update the device's mesh slot at runtime — used by the auto-slot-
 * claim flow at boot. n must be in 1..16. Live change: peer
 * broadcasts on the next render pick up the new identity. */
void phase_manager_set_device_number(uint8_t n);

/* Re-run the redirect advisor against the current registry snapshot
 * and refresh app_state.alert_* fields. Called by:
 *   - after_level_change() on own's queue_level change
 *   - espnow_inbound on every peer broadcast received
 *
 * Without this hook, a peer's queue change wouldn't trigger a fresh
 * SEND>X evaluation on this knob until our own state changed — i.e.
 * the displayed advice could be stale for as long as we sat idle. */
void phase_manager_recompute_advisor(void);

#ifdef __cplusplus
}
#endif

#endif /* PHASE_MANAGER_H */
