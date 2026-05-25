#include "phase_manager.h"

#include <string.h>

#include "app_config.h"
#include "esp_timer.h"

static const taxonomy_entry_t kTaxonomy[8] = {
    {"EMPTY",    0x1e6b1e},
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

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static void phase_enter(phase_t phase, uint32_t now)
{
    s_state.phase = phase;
    s_phase_enter_ms = now;
}

static void advance_setup(uint32_t now)
{
    switch (s_state.phase) {
    case PH_ORIENT:
        phase_enter(PH_USE_CASE, now);
        break;
    case PH_USE_CASE:
        s_state.use_case = s_state.use_case_sel;
        phase_enter(PH_NET, now);
        break;
    case PH_NET:
        s_state.net_mode = s_state.net_sel;
        phase_enter(PH_CONN, now);
        break;
    case PH_CONN:
        phase_enter(PH_ASSIGN, now);
        break;
    case PH_ASSIGN:
        phase_enter(PH_TIME, now);
        break;
    case PH_TIME:
        phase_enter(PH_OPEN_HR, now);
        break;
    case PH_OPEN_HR:
        phase_enter(PH_CLOSE_HR, now);
        break;
    case PH_CLOSE_HR:
        phase_enter(PH_CAB_CNT, now);
        break;
    case PH_CAB_CNT:
        phase_enter(s_state.cab_count == 0 ? PH_START : PH_CAB_INT, now);
        break;
    case PH_CAB_INT:
        phase_enter(PH_COR_INT, now);
        break;
    case PH_COR_INT:
        phase_enter(PH_START, now);
        break;
    case PH_START:
        phase_enter(PH_SPLASH, now);
        break;
    case PH_SPLASH:
        phase_enter(PH_HOME, now);
        break;
    default:
        break;
    }

    if (APP_SKIP_NET_UI && s_state.phase == PH_NET) {
        s_state.net_sel = NET_MESH;
        s_state.net_mode = NET_MESH;
        phase_enter(PH_ASSIGN, now);
    }

    if (APP_SKIP_TIME_UI && s_state.phase == PH_TIME) {
        phase_enter(PH_OPEN_HR, now);
    }
}

void phase_manager_init(void)
{
    memset(&s_state, 0, sizeof(s_state));
    s_state.phase = PH_OFF;
    s_state.use_case = USE_CASE_FITTING_ROOM;
    s_state.use_case_sel = USE_CASE_FITTING_ROOM;
    s_state.net_mode = NET_MESH;
    s_state.net_sel = NET_MESH;
    s_state.time_fields[0] = 2026;
    s_state.time_fields[1] = 1;
    s_state.time_fields[2] = 1;
    s_state.time_fields[3] = 9;
    s_state.time_fields[4] = 0;
    s_state.time_idx = 0;
    s_state.open_hour = 8;
    s_state.close_hour = 22;
    s_state.cab_count = 4;
    s_state.cab_int = 15;
    s_state.cor_int = 15;
    s_state.orient_pos = 0;
    s_state.queue_level = 1;
    s_state.status_level = 1;
    s_state.is_master = false;
    s_state.master_mode = MASTER_SELECT;
    s_state.master_target = 0;
    s_state.nav_mode = NAV_QUEUE;
    s_phase_enter_ms = now_ms();
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
    if (!out) {
        return;
    }
    *out = s_state;
}

void phase_manager_on_encoder(int delta)
{
    if (delta == 0) {
        return;
    }

    switch (s_state.phase) {
    case PH_HOME:
        if (delta > 0 && s_state.queue_level < 8) {
            s_state.queue_level++;
        } else if (delta < 0 && s_state.queue_level > 1) {
            s_state.queue_level--;
        }
        s_state.status_level = s_state.queue_level;
        break;
    case PH_USE_CASE:
        if (delta > 0) {
            s_state.use_case_sel = (app_use_case_t)((s_state.use_case_sel + 1) % USE_CASE_COUNT);
        } else {
            s_state.use_case_sel = (app_use_case_t)((s_state.use_case_sel + USE_CASE_COUNT - 1) % USE_CASE_COUNT);
        }
        break;
    case PH_NET:
        if (delta > 0) {
            s_state.net_sel = (app_net_mode_t)((s_state.net_sel + 1) % NET_MODE_COUNT);
        } else {
            s_state.net_sel = (app_net_mode_t)((s_state.net_sel + NET_MODE_COUNT - 1) % NET_MODE_COUNT);
        }
        break;
    case PH_TIME: {
        static const uint16_t min_v[5] = {2020, 1, 1, 0, 0};
        static const uint16_t max_v[5] = {2099, 12, 31, 23, 59};
        uint16_t val = s_state.time_fields[s_state.time_idx];
        if (delta > 0 && val < max_v[s_state.time_idx]) {
            val++;
        } else if (delta < 0 && val > min_v[s_state.time_idx]) {
            val--;
        }
        s_state.time_fields[s_state.time_idx] = val;
        break;
    }
    case PH_CAB_CNT:
        if (delta > 0 && s_state.cab_count < 16) {
            s_state.cab_count++;
        } else if (delta < 0 && s_state.cab_count > 0) {
            s_state.cab_count--;
        }
        break;
    case PH_CAB_INT:
        if (delta > 0 && s_state.cab_int < 60) {
            s_state.cab_int++;
        } else if (delta < 0 && s_state.cab_int > 1) {
            s_state.cab_int--;
        }
        break;
    case PH_COR_INT:
        if (delta > 0 && s_state.cor_int < 60) {
            s_state.cor_int++;
        } else if (delta < 0 && s_state.cor_int > 1) {
            s_state.cor_int--;
        }
        break;
    case PH_OPEN_HR:
        if (delta > 0 && s_state.open_hour < 23) {
            s_state.open_hour++;
        } else if (delta < 0 && s_state.open_hour > 0) {
            s_state.open_hour--;
        }
        break;
    case PH_CLOSE_HR:
        if (delta > 0 && s_state.close_hour < 23) {
            s_state.close_hour++;
        } else if (delta < 0 && s_state.close_hour > 0) {
            s_state.close_hour--;
        }
        break;
    case PH_ORIENT:
        if (delta > 0) {
            s_state.orient_pos = (uint8_t)((s_state.orient_pos + 1) % 16);
        } else {
            s_state.orient_pos = (uint8_t)((s_state.orient_pos + 15) % 16);
        }
        break;
    case PH_ADMIN:
        if (delta != 0) {
            s_state.is_master = !s_state.is_master;
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

    if (duration_ms >= 2000) {
        if (s_state.phase == PH_ADMIN) {
            phase_enter(PH_HOME, now);
        } else if (s_state.phase == PH_HOME || s_state.phase == PH_SLEEP) {
            phase_enter(PH_ADMIN, now);
        }
        return;
    }

    if (s_state.phase == PH_OFF) {
        phase_enter(PH_BOOT, now);
        return;
    }

    if (s_state.phase == PH_SLEEP) {
        phase_enter(PH_HOME, now);
        return;
    }

    if (s_state.phase == PH_HOME) {
        s_state.nav_mode = (s_state.nav_mode == NAV_QUEUE) ? NAV_CABIN : NAV_QUEUE;
        return;
    }

    advance_setup(now);
}

void phase_manager_tick(uint32_t now_ms)
{
    switch (s_state.phase) {
    case PH_BOOT:
        if (now_ms - s_phase_enter_ms > 1200) {
            phase_enter(PH_ORIENT, now_ms);
        }
        break;
    case PH_SPLASH:
        if (now_ms - s_phase_enter_ms > 900) {
            phase_enter(PH_HOME, now_ms);
        }
        break;
    case PH_CONN:
    case PH_WIFI_CONN:
        if (now_ms - s_phase_enter_ms > 1500) {
            phase_enter(PH_ASSIGN, now_ms);
        }
        break;
    case PH_NET:
        if (APP_SKIP_NET_UI) {
            phase_enter(PH_ASSIGN, now_ms);
        }
        break;
    case PH_TIME:
        if (APP_SKIP_TIME_UI) {
            phase_enter(PH_OPEN_HR, now_ms);
        }
        break;
    default:
        break;
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
    switch (phase) {
    case PH_USE_CASE:
        return 0;
    case PH_NET:
    case PH_CONN:
    case PH_WIFI_CONN:
    case PH_ONLINE_QR:
    case PH_ONLINE_REG:
        return 1;
    case PH_ASSIGN:
        return 2;
    case PH_TIME:
    case PH_OPEN_HR:
    case PH_CLOSE_HR:
        return 3;
    case PH_CAB_CNT:
        return 4;
    case PH_CAB_INT:
        return 5;
    case PH_COR_INT:
        return 6;
    case PH_START:
    case PH_SPLASH:
    case PH_SYNC:
        return 7;
    case PH_HOME:
    case PH_ADMIN:
    case PH_SLEEP:
    case PH_OFF:
    default:
        return 8;
    }
}
