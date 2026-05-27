/* Screenshot capture — LVGL snapshot → BMP RGB565 on SD card.
 *
 * Trigger: triple-tap on the touchscreen (wired in ui_engine.c).
 * Format: BITMAPFILEHEADER + BITMAPV4HEADER + raw pixel bytes.
 *   - V4 header carries explicit R/G/B bit-masks (BI_BITFIELDS=3)
 *     so Windows decodes 16 bpp correctly. Older V3 headers
 *     default to RGB555 which mis-renders our RGB565 panel.
 *   - Negative biHeight = top-down image so we can stream rows in
 *     the order LVGL gives them (no row-flip needed).
 *   - LV_COLOR_16_SWAP=1 makes the snapshot buffer big-endian
 *     RGB565; BMP wants little-endian, so we byte-swap in place
 *     before fwrite.
 *
 * Memory: 360 × 360 × 2 = 259 200 bytes pulled from PSRAM via
 * heap_caps_malloc so we don't blow the 320 KB DRAM budget.
 */

#include "screenshot.h"

#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lvgl.h"
#include "sd_card.h"

static const char *kTag = "screenshot";

#define SHOT_W 360
#define SHOT_H 360
#define SHOT_BPP 2
#define PIXEL_DATA_SIZE (SHOT_W * SHOT_H * SHOT_BPP)

#pragma pack(push, 1)
typedef struct {
    uint16_t bfType;        /* "BM" */
    uint32_t bfSize;
    uint16_t bfReserved1;
    uint16_t bfReserved2;
    uint32_t bfOffBits;
} bmp_file_hdr_t;

typedef struct {
    uint32_t biSize;            /* 108 for V4 */
    int32_t  biWidth;
    int32_t  biHeight;          /* negative → top-down rows */
    uint16_t biPlanes;
    uint16_t biBitCount;        /* 16 */
    uint32_t biCompression;     /* 3 = BI_BITFIELDS */
    uint32_t biSizeImage;
    int32_t  biXPelsPerMeter;
    int32_t  biYPelsPerMeter;
    uint32_t biClrUsed;
    uint32_t biClrImportant;
    uint32_t bV4RedMask;
    uint32_t bV4GreenMask;
    uint32_t bV4BlueMask;
    uint32_t bV4AlphaMask;
    uint32_t bV4CSType;
    uint8_t  bV4Endpoints[36];
    uint32_t bV4GammaRed;
    uint32_t bV4GammaGreen;
    uint32_t bV4GammaBlue;
} bmp_v4_hdr_t;
#pragma pack(pop)

/* Scan /sdcard for screen_NNNN.bmp filenames and return the next
 * unused index. O(N) per save, but N is small (single-digit shoots
 * per session typically) and only fires on triple-tap. */
static int next_screenshot_idx(void)
{
    DIR *dir = opendir("/sdcard");
    if (!dir) {
        return 0;
    }
    int max_idx = -1;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        int idx = -1;
        if (sscanf(ent->d_name, "screen_%d.bmp", &idx) == 1 && idx >= 0) {
            if (idx > max_idx) max_idx = idx;
        }
    }
    closedir(dir);
    return max_idx + 1;
}

esp_err_t screenshot_capture_and_save(void)
{
    if (!sd_card_is_mounted()) {
        ESP_LOGE(kTag, "SD card not mounted — cannot save screenshot");
        return ESP_ERR_INVALID_STATE;
    }

    lv_obj_t *scr = lv_scr_act();
    if (!scr) {
        ESP_LOGE(kTag, "no active screen");
        return ESP_FAIL;
    }

    /* Snapshot buffer lives in PSRAM — 256 KB would crush DRAM. */
    uint32_t buf_size = lv_snapshot_buf_size_needed(scr, LV_IMG_CF_TRUE_COLOR);
    if (buf_size == 0) {
        ESP_LOGE(kTag, "lv_snapshot_buf_size_needed returned 0");
        return ESP_FAIL;
    }
    uint8_t *buf = heap_caps_malloc(buf_size,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        ESP_LOGE(kTag, "PSRAM alloc %u bytes failed", (unsigned)buf_size);
        return ESP_ERR_NO_MEM;
    }

    lv_img_dsc_t dsc;
    lv_res_t r = lv_snapshot_take_to_buf(scr, LV_IMG_CF_TRUE_COLOR,
                                         &dsc, buf, buf_size);
    if (r != LV_RES_OK) {
        ESP_LOGE(kTag, "lv_snapshot_take_to_buf failed (r=%d)", (int)r);
        heap_caps_free(buf);
        return ESP_FAIL;
    }

    int idx = next_screenshot_idx();
    char path[64];
    snprintf(path, sizeof(path), "/sdcard/screen_%04d.bmp", idx);

    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(kTag, "fopen %s failed (errno=%d)", path, errno);
        heap_caps_free(buf);
        return ESP_FAIL;
    }

    bmp_file_hdr_t fhdr = {
        .bfType    = 0x4D42,  /* 'BM' little-endian */
        .bfSize    = (uint32_t)(sizeof(bmp_file_hdr_t) + sizeof(bmp_v4_hdr_t) + PIXEL_DATA_SIZE),
        .bfOffBits = (uint32_t)(sizeof(bmp_file_hdr_t) + sizeof(bmp_v4_hdr_t)),
    };
    bmp_v4_hdr_t ihdr = {
        .biSize        = sizeof(bmp_v4_hdr_t),
        .biWidth       = SHOT_W,
        .biHeight      = -SHOT_H,  /* top-down */
        .biPlanes      = 1,
        .biBitCount    = 16,
        .biCompression = 3,        /* BI_BITFIELDS */
        .biSizeImage   = PIXEL_DATA_SIZE,
        .bV4RedMask    = 0xF800,
        .bV4GreenMask  = 0x07E0,
        .bV4BlueMask   = 0x001F,
        .bV4AlphaMask  = 0,
    };
    fwrite(&fhdr, 1, sizeof(fhdr), f);
    fwrite(&ihdr, 1, sizeof(ihdr), f);

    /* Byte-swap the snapshot in place. LV_COLOR_16_SWAP=1 stores
     * pixels big-endian (panel order); BMP wants little-endian. */
    uint8_t *pixels = (uint8_t *)dsc.data;
    for (uint32_t i = 0; i < PIXEL_DATA_SIZE; i += 2) {
        uint8_t t   = pixels[i];
        pixels[i]   = pixels[i + 1];
        pixels[i + 1] = t;
    }

    size_t wrote = fwrite(pixels, 1, PIXEL_DATA_SIZE, f);
    fclose(f);
    heap_caps_free(buf);

    if (wrote != PIXEL_DATA_SIZE) {
        ESP_LOGE(kTag, "short write %s: %u/%u bytes",
                 path, (unsigned)wrote, (unsigned)PIXEL_DATA_SIZE);
        return ESP_FAIL;
    }
    ESP_LOGI(kTag, "saved %s (%u bytes total)",
             path, (unsigned)fhdr.bfSize);
    return ESP_OK;
}
