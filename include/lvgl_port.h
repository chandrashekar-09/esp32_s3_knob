#ifndef LVGL_PORT_H
#define LVGL_PORT_H

#include <stdbool.h>
#include <stdint.h>

#include "display_driver.h"
#include "touch_cst816.h"

#ifdef __cplusplus
extern "C" {
#endif

bool lvgl_port_init(const display_config_t *disp_cfg, const cst816_config_t *touch_cfg);
void lvgl_port_lock(void);
void lvgl_port_unlock(void);

#ifdef __cplusplus
}
#endif

#endif /* LVGL_PORT_H */
