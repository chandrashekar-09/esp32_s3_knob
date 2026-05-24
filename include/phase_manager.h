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
    PH_HOME,
    PH_SLEEP,
    PH_ADMIN
} phase_t;

typedef struct {
    const char *label;
    uint32_t color_hex;
} taxonomy_entry_t;

void phase_manager_init(void);
phase_t phase_manager_get_phase(void);
void phase_manager_set_phase(phase_t phase);

void phase_manager_on_encoder(int delta);
void phase_manager_on_button(bool pressed, uint32_t duration_ms);

uint8_t phase_manager_get_queue_level(void);
const taxonomy_entry_t *phase_manager_get_taxonomy(uint8_t level);

#ifdef __cplusplus
}
#endif

#endif /* PHASE_MANAGER_H */
