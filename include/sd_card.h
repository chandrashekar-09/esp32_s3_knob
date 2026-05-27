#ifndef SD_CARD_H
#define SD_CARD_H

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Mount the on-board microSD slot (4-bit SDMMC on GPIO 38/39/40/41/47/48).
 * Mount point is /sdcard. Returns ESP_OK on success or any ESP error
 * code on failure (no card inserted, bad FAT, etc.). Failure is
 * non-fatal — callers should check sd_card_is_mounted() before any
 * filesystem access. Safe to call multiple times; subsequent calls
 * are no-ops once mounted. */
esp_err_t sd_card_mount(void);

bool sd_card_is_mounted(void);

#ifdef __cplusplus
}
#endif

#endif /* SD_CARD_H */
