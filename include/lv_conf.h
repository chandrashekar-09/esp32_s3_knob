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

/* Use ESP-IDF's heap (stdlib malloc/free) instead of LVGL's built-in
 * 32 KB static pool. The multi-ring renderer allocates up to 160
 * widgets (144 arc pool + 16 peer-num labels + extras) and
 * overflows the built-in pool, causing the UI to fail at startup.
 * The ESP32-S3 has ~250 KB DRAM + 8 MB PSRAM — no realistic ceiling.
 * Default LV_MEM_CUSTOM_INCLUDE = <stdlib.h> + _ALLOC = malloc etc. */
#define LV_MEM_CUSTOM 1

#define LV_USE_LOG 0

#define LV_FONT_MONTSERRAT_10 1
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_22 1
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_MONTSERRAT_28 1
/* Mont 32 + 36 are the heaviest sizes we render — used for the FR
 * identity label and the centre status word respectively. LVGL's
 * stock Montserrat comes in a single weight (medium), but at these
 * sizes the strokes are thick enough to feel "bolder" even without
 * a true Bold cut. Doto remains the preferred upgrade once the
 * offline font conversion lands. */
#define LV_FONT_MONTSERRAT_32 1
#define LV_FONT_MONTSERRAT_36 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14

#define LV_USE_ARC 1
#define LV_USE_LABEL 1
#define LV_USE_BAR 1
#define LV_USE_METER 1

#define LV_USE_THEME_DEFAULT 1

/* Enable lv_snapshot_take* — used by the triple-tap screenshot
 * feature in src/screenshot.c. Pulls in extras/others/snapshot. */
#define LV_USE_SNAPSHOT 1

/* Enable lv_qrcode_create — used on the PH_PROVISIONING screen to
 * render the WiFi-join QR so a phone can one-tap onto the knob's
 * SoftAP. Pulls in extras/libs/qrcode (~5 KB). */
#define LV_USE_QRCODE 1

#endif /* LV_CONF_H */
