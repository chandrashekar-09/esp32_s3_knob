#ifndef UI_ENGINE_H
#define UI_ENGINE_H

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "phase_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

extern SemaphoreHandle_t xGuiSemaphore;

void ui_engine_init(void);
void ui_engine_set_phase(phase_t phase);
void ui_engine_set_mesh_state(bool connected, bool root, int layer);
void ui_engine_set_ota_status(const char *status);
void ui_engine_render(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_ENGINE_H */
