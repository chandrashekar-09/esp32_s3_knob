#ifndef DISPLAY_DRIVER_H
#define DISPLAY_DRIVER_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int spi_host;
    int pin_clk;
    int pin_cs;
    int pin_sio0;
    int pin_sio1;
    int pin_sio2;
    int pin_sio3;
    int pin_rst;
    int pin_bl;
    int width;
    int height;
    int pclk_hz;
    bool quad_mode;
    bool invert_colors;
} display_config_t;

typedef void (*display_flush_done_cb_t)(void *user_ctx);

esp_err_t display_driver_init(const display_config_t *config);
void display_driver_set_flush_cb(display_flush_done_cb_t cb, void *user_ctx);
void display_driver_set_backlight(uint8_t percent);
void display_driver_flush(int x1, int y1, int x2, int y2, const void *color_data, size_t color_bytes);

#ifdef __cplusplus
}
#endif

#endif /* DISPLAY_DRIVER_H */
