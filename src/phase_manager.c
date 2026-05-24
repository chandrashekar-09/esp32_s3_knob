#include "phase_manager.h"

#include <string.h>

static volatile phase_t s_phase = PH_BOOT;
static volatile uint8_t s_queue_level = 1;

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

void phase_manager_init(void)
{
    s_phase = PH_HOME;
    s_queue_level = 1;
}

phase_t phase_manager_get_phase(void)
{
    return s_phase;
}

void phase_manager_set_phase(phase_t phase)
{
    s_phase = phase;
}

void phase_manager_on_encoder(int delta)
{
    switch (s_phase) {
    case PH_HOME:
        if (delta > 0 && s_queue_level < 8) {
            s_queue_level++;
        } else if (delta < 0 && s_queue_level > 1) {
            s_queue_level--;
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

    if (duration_ms >= 2000) {
        if (s_phase == PH_SLEEP) {
            s_phase = PH_HOME;
        } else if (s_phase == PH_ADMIN) {
            s_phase = PH_HOME;
        } else {
            s_phase = PH_ADMIN;
        }
        return;
    }

    if (duration_ms < 500) {
        if (s_phase == PH_BOOT || s_phase == PH_ADMIN || s_phase == PH_SLEEP) {
            s_phase = PH_HOME;
        }
    }
}

uint8_t phase_manager_get_queue_level(void)
{
    return s_queue_level;
}

const taxonomy_entry_t *phase_manager_get_taxonomy(uint8_t level)
{
    if (level < 1 || level > 8) {
        return NULL;
    }
    return &kTaxonomy[level - 1];
}
