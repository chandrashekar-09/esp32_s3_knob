#ifndef SCREENSHOT_H
#define SCREENSHOT_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Capture the current active LVGL screen and save it as
 * /sdcard/screen_NNNN.bmp (RGB565, BITMAPV4HEADER so Windows
 * reads the bit-field correctly). NNNN auto-increments by
 * scanning the directory at call time.
 *
 * MUST be called from the LVGL task (so the mutex is held).
 * Returns:
 *   ESP_OK                — file written
 *   ESP_ERR_INVALID_STATE — SD not mounted
 *   ESP_ERR_NO_MEM        — snapshot buffer alloc failed
 *   ESP_FAIL              — render or fopen/fwrite failed
 */
esp_err_t screenshot_capture_and_save(void);

#ifdef __cplusplus
}
#endif

#endif /* SCREENSHOT_H */
