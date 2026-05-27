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

#ifdef __cplusplus
}
#endif

#endif /* PHASE_MANAGER_H */
