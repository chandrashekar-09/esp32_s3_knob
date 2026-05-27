#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

#include "esp_timer.h"

#define LV_COLOR_DEPTH 16
/* RGB565 MSB-first on the wire. The ST77916 (and most SPI panels) expect
 * pixel bytes in big-endian order. With LV_COLOR_16_SWAP=0, LVGL stored
 * pixels little-endian and every color came out swapped → produces the
 * multi-colored horizontal stripe pattern. Setting to 1 pre-swaps so the
 * draw buffer's RGB565 bytes already match what the panel reads. */
#define LV_COLOR_16_SWAP 1

#define LV_USE_PERF_MONITOR 0
#define LV_USE_MEM_MONITOR 0

#define LV_TICK_CUSTOM 1
#define LV_TICK_CUSTOM_INCLUDE "esp_timer.h"
#define LV_TICK_CUSTOM_SYS_TIME_EXPR (esp_timer_get_time() / 1000)

#define LV_MEM_CUSTOM 0

#define LV_USE_LOG 0

#define LV_FONT_MONTSERRAT_10 1
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14

#define LV_USE_ARC 1
#define LV_USE_LABEL 1
#define LV_USE_BAR 1

#define LV_USE_THEME_DEFAULT 1

#endif /* LV_CONF_H */
