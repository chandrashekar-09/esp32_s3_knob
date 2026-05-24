#ifndef TOUCH_CST816_H
#define TOUCH_CST816_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "driver/i2c.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    i2c_port_t i2c_port;
    int pin_sda;
    int pin_scl;
    int pin_int;
    int pin_rst;
    uint8_t i2c_addr;
    uint16_t max_x;
    uint16_t max_y;
    bool swap_xy;
    bool invert_x;
    bool invert_y;
} cst816_config_t;

typedef struct {
    bool touched;
    uint16_t x;
    uint16_t y;
} cst816_point_t;

esp_err_t cst816_init(const cst816_config_t *config);
bool cst816_read(cst816_point_t *point);

#ifdef __cplusplus
}
#endif

#endif /* TOUCH_CST816_H */
