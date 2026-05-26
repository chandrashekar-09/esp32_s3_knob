/* ST77916 QSPI driver for JC3636K718 ESP32-S3 1.8" knob.
 *
 * QSPI wire protocol (matches Espressif's esp_lcd_st77916_spi.c verbatim):
 *   command frame  = opcode 0x02 (cmd phase, 8b) || panel_cmd<<16 (addr phase, 24b)
 *                    [|| param bytes (data phase, single line)]
 *   color frame    = opcode 0x32 (cmd phase, 8b) || 0x2C<<16 (addr phase, 24b)
 *                    || pixel bytes (data phase, quad lines)
 *
 * Without these opcode prefixes the ST77916 ignores every command in QSPI mode
 * and the panel stays in its boot test pattern (yellowish-orange). The
 * minimal SPI write_cmd that ships in the legacy code path skips this entirely.
 *
 * The ~200-command vendor init sequence below is copied verbatim from
 * components/display/lcd/esp_lcd_st77916/esp_lcd_st77916_spi.c (Apache-2.0).
 * The panel needs every byte of it to enter normal display mode. */

#include "display_driver.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

/* QSPI wire protocol for this panel (matches Arduino_GFX Arduino_ESP32QSPI,
 * the only QSPI driver confirmed working on the JC3636K718).
 *
 * Every transaction is 32 bits of [opcode|24-bit-address|optional data]:
 *
 *   write command:  0x02  00  CMD  00  [params...]   <- cmd in MIDDLE byte
 *   write color:    0x32  00  3C   00  [pixels...]   <- 3C = RAMWRC
 *
 * Two non-obvious bits about this format:
 *  1. The panel command byte goes in bit position [15:8] of the 24-bit
 *     address phase (`cmd << 8`), NOT [23:16] as Espressif's generic
 *     esp_lcd_st77916 driver assumes. Wrong position = garbage on screen.
 *  2. Pixel writes use 0x3C (RAMWRC, "Memory Write Continue") rather than
 *     0x2C (RAMWR). The panel keeps the GRAM write pointer alive between
 *     transactions, so back-to-back chunks just keep filling the window. */
#define ST77916_OPCODE_WRITE_CMD   0x02
#define ST77916_OPCODE_WRITE_COLOR 0x32

#define ST77916_CMD_SLPOUT  0x11
#define ST77916_CMD_DISPON  0x29
#define ST77916_CMD_CASET   0x2A
#define ST77916_CMD_RASET   0x2B
#define ST77916_CMD_RAMWR   0x2C
#define ST77916_CMD_RAMWRC  0x3C   /* memory write continue */
#define ST77916_CMD_MADCTL  0x36
#define ST77916_CMD_COLMOD  0x3A
#define ST77916_CMD_INVON   0x21
#define ST77916_CMD_INVOFF  0x20
#define ST77916_CMD_TEON    0x35

static const char *kTag = "st77916";

static spi_device_handle_t s_spi = NULL;
static display_config_t s_cfg = {};
static display_flush_done_cb_t s_flush_cb = NULL;
static void *s_flush_ctx = NULL;

static SemaphoreHandle_t s_te_sem = NULL;
static volatile bool s_te_enabled = false;

typedef struct {
    uint8_t cmd;
    const uint8_t *data;
    uint8_t data_len;
    uint8_t delay_ms;
} st77916_init_cmd_t;

/* AUTHORITATIVE init sequence for the JC3636K718 1.8" knob — transcribed
 * verbatim from the manufacturer's ST77916_LVGL_DEMO/scr_st77916.h
 * (`lcd_init_cmd[]`, lines 42-227). This is the actual factory-tested
 * sequence for THIS panel module. It differs from both:
 *   - Espressif's esp_lcd_st77916 "default" (generic 1.5x" panel)
 *   - Arduino_GFX's st77916_180_init_operations (different 1.8" SKU)
 * — across ~60 register values: starting byte 0xF0=0x28 (not 0x08),
 * different B-block voltages, different gamma curves, different gate
 * driver timing, different OTP page 10 register values. Using either
 * of the wrong variants produces visible stripes/garbage. */

/* Compound-literal init array. Each row is `{cmd, {payload bytes...},
 * data_len, delay_ms}`. GCC accepts compound literals in file-scope
 * static const initializers as long as the result is constant. */
static const st77916_init_cmd_t kInit[] = {
    /* Page-reset and vendor unlock */
    {0xF0, (const uint8_t[]){0x28}, 1, 0},
    {0xF2, (const uint8_t[]){0x28}, 1, 0},
    {0x73, (const uint8_t[]){0xF0}, 1, 0},
    {0x7C, (const uint8_t[]){0xD1}, 1, 0},
    {0x83, (const uint8_t[]){0xE0}, 1, 0},
    {0x84, (const uint8_t[]){0x61}, 1, 0},
    {0xF2, (const uint8_t[]){0x82}, 1, 0},
    {0xF0, (const uint8_t[]){0x00}, 1, 0},
    /* Command-set page 1 — gate / source / timing */
    {0xF0, (const uint8_t[]){0x01}, 1, 0},
    {0xF1, (const uint8_t[]){0x01}, 1, 0},
    {0xB0, (const uint8_t[]){0x56}, 1, 0},
    {0xB1, (const uint8_t[]){0x4D}, 1, 0},
    {0xB2, (const uint8_t[]){0x24}, 1, 0},
    {0xB4, (const uint8_t[]){0x87}, 1, 0},
    {0xB5, (const uint8_t[]){0x44}, 1, 0},
    {0xB6, (const uint8_t[]){0x8B}, 1, 0},
    {0xB7, (const uint8_t[]){0x40}, 1, 0},
    {0xB8, (const uint8_t[]){0x86}, 1, 0},
    {0xBA, (const uint8_t[]){0x00}, 1, 0},
    {0xBB, (const uint8_t[]){0x08}, 1, 0},
    {0xBC, (const uint8_t[]){0x08}, 1, 0},
    {0xBD, (const uint8_t[]){0x00}, 1, 0},
    {0xC0, (const uint8_t[]){0x80}, 1, 0},
    {0xC1, (const uint8_t[]){0x10}, 1, 0},
    {0xC2, (const uint8_t[]){0x37}, 1, 0},
    {0xC3, (const uint8_t[]){0x80}, 1, 0},
    {0xC4, (const uint8_t[]){0x10}, 1, 0},
    {0xC5, (const uint8_t[]){0x37}, 1, 0},
    {0xC6, (const uint8_t[]){0xA9}, 1, 0},
    {0xC7, (const uint8_t[]){0x41}, 1, 0},
    {0xC8, (const uint8_t[]){0x01}, 1, 0},   /* mfg: 0x01 (not 0x51) */
    {0xC9, (const uint8_t[]){0xA9}, 1, 0},
    {0xCA, (const uint8_t[]){0x41}, 1, 0},
    {0xCB, (const uint8_t[]){0x01}, 1, 0},   /* mfg: 0x01 (not 0x51) */
    {0xD0, (const uint8_t[]){0x91}, 1, 0},
    {0xD1, (const uint8_t[]){0x68}, 1, 0},
    {0xD2, (const uint8_t[]){0x68}, 1, 0},   /* mfg: 0x68 (not 0x69) */
    {0xF5, (const uint8_t[]){0x00, 0xA5}, 2, 0},
    {0xDD, (const uint8_t[]){0x4F}, 1, 0},   /* mfg: 0x4F */
    {0xDE, (const uint8_t[]){0x4F}, 1, 0},   /* mfg: 0x4F */
    {0xF1, (const uint8_t[]){0x10}, 1, 0},
    {0xF0, (const uint8_t[]){0x00}, 1, 0},
    /* Command-set page 2 — gamma curves (panel-specific) */
    {0xF0, (const uint8_t[]){0x02}, 1, 0},
    {0xE0, (const uint8_t[]){0xF0, 0x0A, 0x10, 0x09, 0x09, 0x36, 0x35, 0x33,
                             0x4A, 0x29, 0x15, 0x15, 0x2E, 0x34}, 14, 0},
    {0xE1, (const uint8_t[]){0xF0, 0x0A, 0x0F, 0x08, 0x08, 0x05, 0x34, 0x33,
                             0x4A, 0x39, 0x15, 0x15, 0x2D, 0x33}, 14, 0},
    /* Command-set page 0x10 — OTP and power timings */
    {0xF0, (const uint8_t[]){0x10}, 1, 0},
    {0xF3, (const uint8_t[]){0x10}, 1, 0},
    {0xE0, (const uint8_t[]){0x07}, 1, 0},   /* mfg: 0x07 */
    {0xE1, (const uint8_t[]){0x00}, 1, 0},
    {0xE2, (const uint8_t[]){0x00}, 1, 0},
    {0xE3, (const uint8_t[]){0x00}, 1, 0},
    {0xE4, (const uint8_t[]){0xE0}, 1, 0},
    {0xE5, (const uint8_t[]){0x06}, 1, 0},
    {0xE6, (const uint8_t[]){0x21}, 1, 0},
    {0xE7, (const uint8_t[]){0x01}, 1, 0},   /* mfg: 0x01 (not 0x00) */
    {0xE8, (const uint8_t[]){0x05}, 1, 0},
    {0xE9, (const uint8_t[]){0x02}, 1, 0},   /* mfg: 0x02 (not 0x82) */
    {0xEA, (const uint8_t[]){0xDA}, 1, 0},   /* mfg: 0xDA */
    {0xEB, (const uint8_t[]){0x00}, 1, 0},   /* mfg: 0x00 (not 0x89) */
    {0xEC, (const uint8_t[]){0x00}, 1, 0},   /* mfg: 0x00 (not 0x20) */
    {0xED, (const uint8_t[]){0x0F}, 1, 0},   /* mfg: 0x0F (not 0x14) */
    {0xEE, (const uint8_t[]){0x00}, 1, 0},   /* mfg: 0x00 (not 0xFF) */
    {0xEF, (const uint8_t[]){0x00}, 1, 0},
    {0xF8, (const uint8_t[]){0x00}, 1, 0},   /* mfg: 0x00 (not 0xFF) */
    {0xF9, (const uint8_t[]){0x00}, 1, 0},
    {0xFA, (const uint8_t[]){0x00}, 1, 0},
    {0xFB, (const uint8_t[]){0x00}, 1, 0},   /* mfg: 0x00 (not 0x30) */
    {0xFC, (const uint8_t[]){0x00}, 1, 0},
    {0xFD, (const uint8_t[]){0x00}, 1, 0},
    {0xFE, (const uint8_t[]){0x00}, 1, 0},
    {0xFF, (const uint8_t[]){0x00}, 1, 0},
    /* Source-output configuration block */
    {0x60, (const uint8_t[]){0x40}, 1, 0},   /* mfg: 0x40 */
    {0x61, (const uint8_t[]){0x04}, 1, 0},   /* mfg: 0x04 */
    {0x62, (const uint8_t[]){0x00}, 1, 0},
    {0x63, (const uint8_t[]){0x42}, 1, 0},
    {0x64, (const uint8_t[]){0xD9}, 1, 0},   /* mfg: 0xD9 */
    {0x65, (const uint8_t[]){0x00}, 1, 0},
    {0x66, (const uint8_t[]){0x00}, 1, 0},   /* mfg: 0x00 */
    {0x67, (const uint8_t[]){0x00}, 1, 0},   /* mfg: 0x00 */
    {0x68, (const uint8_t[]){0x00}, 1, 0},
    {0x69, (const uint8_t[]){0x00}, 1, 0},
    {0x6A, (const uint8_t[]){0x00}, 1, 0},
    {0x6B, (const uint8_t[]){0x00}, 1, 0},
    {0x70, (const uint8_t[]){0x40}, 1, 0},
    {0x71, (const uint8_t[]){0x03}, 1, 0},
    {0x72, (const uint8_t[]){0x00}, 1, 0},
    {0x73, (const uint8_t[]){0x42}, 1, 0},
    {0x74, (const uint8_t[]){0xD8}, 1, 0},
    {0x75, (const uint8_t[]){0x00}, 1, 0},
    {0x76, (const uint8_t[]){0x00}, 1, 0},
    {0x77, (const uint8_t[]){0x00}, 1, 0},
    {0x78, (const uint8_t[]){0x00}, 1, 0},
    {0x79, (const uint8_t[]){0x00}, 1, 0},
    {0x7A, (const uint8_t[]){0x00}, 1, 0},
    {0x7B, (const uint8_t[]){0x00}, 1, 0},
    /* Gate-driver timing block — 8 groups of 8 registers */
    {0x80, (const uint8_t[]){0x48}, 1, 0}, {0x81, (const uint8_t[]){0x00}, 1, 0},
    {0x82, (const uint8_t[]){0x06}, 1, 0}, {0x83, (const uint8_t[]){0x02}, 1, 0},
    {0x84, (const uint8_t[]){0xD6}, 1, 0}, {0x85, (const uint8_t[]){0x04}, 1, 0},
    {0x86, (const uint8_t[]){0x00}, 1, 0}, {0x87, (const uint8_t[]){0x00}, 1, 0},
    {0x88, (const uint8_t[]){0x48}, 1, 0}, {0x89, (const uint8_t[]){0x00}, 1, 0},
    {0x8A, (const uint8_t[]){0x08}, 1, 0}, {0x8B, (const uint8_t[]){0x02}, 1, 0},
    {0x8C, (const uint8_t[]){0xD8}, 1, 0}, {0x8D, (const uint8_t[]){0x04}, 1, 0},
    {0x8E, (const uint8_t[]){0x00}, 1, 0}, {0x8F, (const uint8_t[]){0x00}, 1, 0},
    {0x90, (const uint8_t[]){0x48}, 1, 0}, {0x91, (const uint8_t[]){0x00}, 1, 0},
    {0x92, (const uint8_t[]){0x0A}, 1, 0}, {0x93, (const uint8_t[]){0x02}, 1, 0},
    {0x94, (const uint8_t[]){0xDA}, 1, 0}, {0x95, (const uint8_t[]){0x04}, 1, 0},
    {0x96, (const uint8_t[]){0x00}, 1, 0}, {0x97, (const uint8_t[]){0x00}, 1, 0},
    {0x98, (const uint8_t[]){0x48}, 1, 0}, {0x99, (const uint8_t[]){0x00}, 1, 0},
    {0x9A, (const uint8_t[]){0x0C}, 1, 0}, {0x9B, (const uint8_t[]){0x02}, 1, 0},
    {0x9C, (const uint8_t[]){0xDC}, 1, 0}, {0x9D, (const uint8_t[]){0x04}, 1, 0},
    {0x9E, (const uint8_t[]){0x00}, 1, 0}, {0x9F, (const uint8_t[]){0x00}, 1, 0},
    {0xA0, (const uint8_t[]){0x48}, 1, 0}, {0xA1, (const uint8_t[]){0x00}, 1, 0},
    {0xA2, (const uint8_t[]){0x05}, 1, 0}, {0xA3, (const uint8_t[]){0x02}, 1, 0},
    {0xA4, (const uint8_t[]){0xD5}, 1, 0}, {0xA5, (const uint8_t[]){0x04}, 1, 0},
    {0xA6, (const uint8_t[]){0x00}, 1, 0}, {0xA7, (const uint8_t[]){0x00}, 1, 0},
    {0xA8, (const uint8_t[]){0x48}, 1, 0}, {0xA9, (const uint8_t[]){0x00}, 1, 0},
    {0xAA, (const uint8_t[]){0x07}, 1, 0}, {0xAB, (const uint8_t[]){0x02}, 1, 0},
    {0xAC, (const uint8_t[]){0xD7}, 1, 0}, {0xAD, (const uint8_t[]){0x04}, 1, 0},
    {0xAE, (const uint8_t[]){0x00}, 1, 0}, {0xAF, (const uint8_t[]){0x00}, 1, 0},
    {0xB0, (const uint8_t[]){0x48}, 1, 0}, {0xB1, (const uint8_t[]){0x00}, 1, 0},
    {0xB2, (const uint8_t[]){0x09}, 1, 0}, {0xB3, (const uint8_t[]){0x02}, 1, 0},
    {0xB4, (const uint8_t[]){0xD9}, 1, 0}, {0xB5, (const uint8_t[]){0x04}, 1, 0},
    {0xB6, (const uint8_t[]){0x00}, 1, 0}, {0xB7, (const uint8_t[]){0x00}, 1, 0},
    {0xB8, (const uint8_t[]){0x48}, 1, 0}, {0xB9, (const uint8_t[]){0x00}, 1, 0},
    {0xBA, (const uint8_t[]){0x0B}, 1, 0}, {0xBB, (const uint8_t[]){0x02}, 1, 0},
    {0xBC, (const uint8_t[]){0xDB}, 1, 0}, {0xBD, (const uint8_t[]){0x04}, 1, 0},
    {0xBE, (const uint8_t[]){0x00}, 1, 0}, {0xBF, (const uint8_t[]){0x00}, 1, 0},
    /* OTP / source-driver final block */
    {0xC0, (const uint8_t[]){0x10}, 1, 0}, {0xC1, (const uint8_t[]){0x47}, 1, 0},
    {0xC2, (const uint8_t[]){0x56}, 1, 0}, {0xC3, (const uint8_t[]){0x65}, 1, 0},
    {0xC4, (const uint8_t[]){0x74}, 1, 0}, {0xC5, (const uint8_t[]){0x88}, 1, 0},
    {0xC6, (const uint8_t[]){0x99}, 1, 0}, {0xC7, (const uint8_t[]){0x01}, 1, 0},
    {0xC8, (const uint8_t[]){0xBB}, 1, 0}, {0xC9, (const uint8_t[]){0xAA}, 1, 0},
    {0xD0, (const uint8_t[]){0x10}, 1, 0}, {0xD1, (const uint8_t[]){0x47}, 1, 0},
    {0xD2, (const uint8_t[]){0x56}, 1, 0}, {0xD3, (const uint8_t[]){0x65}, 1, 0},
    {0xD4, (const uint8_t[]){0x74}, 1, 0}, {0xD5, (const uint8_t[]){0x88}, 1, 0},
    {0xD6, (const uint8_t[]){0x99}, 1, 0}, {0xD7, (const uint8_t[]){0x01}, 1, 0},
    {0xD8, (const uint8_t[]){0xBB}, 1, 0}, {0xD9, (const uint8_t[]){0xAA}, 1, 0},
    /* Lock and exit vendor pages */
    {0xF3, (const uint8_t[]){0x01}, 1, 0},
    {0xF0, (const uint8_t[]){0x00}, 1, 0},
    /* Display on + leave sleep. Manufacturer's array ends here — no
     * 0xA3/0xA5 calibration loop, no manual CASET/RASET (the panel
     * has sane defaults for 360x360). Inversion is ON by default
     * because of the panel's color polarity. */
    {0x21, (const uint8_t[]){0x00}, 1, 0},     /* INVON */
    {0x11, (const uint8_t[]){0x00}, 1, 120},   /* SLPOUT + 120 ms wake */
    {0x29, (const uint8_t[]){0x00}, 1, 0},     /* DISPON */
};

static void IRAM_ATTR display_te_isr(void *arg)
{
    (void)arg;
    BaseType_t hpw = pdFALSE;
    if (s_te_sem) {
        xSemaphoreGiveFromISR(s_te_sem, &hpw);
    }
    if (hpw) {
        portYIELD_FROM_ISR();
    }
}

/* Panel command frame: opcode 0x02 + panel cmd in MIDDLE byte of the
 * 24-bit address phase + optional param bytes. Everything on single line. */
static esp_err_t tx_param(uint8_t cmd, const uint8_t *param, size_t len)
{
    if (!s_spi) {
        return ESP_ERR_INVALID_STATE;
    }
    spi_transaction_t t = {};
    t.cmd = ST77916_OPCODE_WRITE_CMD;
    t.addr = ((uint32_t)cmd) << 8;     /* `[00][CMD][00]` — middle byte */
    t.length = len * 8;
    t.tx_buffer = (len > 0) ? param : NULL;
    t.flags = 0;
    return spi_device_polling_transmit(s_spi, &t);
}

/* Pixel writes now go through tx_color_stream() further down, which
 * keeps CS low across all chunks so the panel sees a single RAMWRC
 * stream. The old single-shot tx_color() was inadequate because every
 * fresh CS-low edge made the panel restart writes at the window origin
 * and overwrite the previous chunk. */

static void st77916_reset(void)
{
    if (s_cfg.pin_rst < 0) {
        return;
    }
    gpio_set_level(s_cfg.pin_rst, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(s_cfg.pin_rst, 1);
    vTaskDelay(pdMS_TO_TICKS(150));
}

static esp_err_t st77916_run_init(void)
{
    /* MADCTL + COLMOD go first — Espressif convention; the manufacturer's
     * vendor array doesn't include them so we must. */
    uint8_t madctl = 0x00;
    esp_err_t err = tx_param(ST77916_CMD_MADCTL, &madctl, 1);
    if (err != ESP_OK) return err;

    uint8_t colmod = 0x55; /* 16 bpp RGB565 */
    err = tx_param(ST77916_CMD_COLMOD, &colmod, 1);
    if (err != ESP_OK) return err;

    /* Walk the manufacturer's vendor init array. It already ends with
     * INVON (0x21), SLPOUT (0x11 + 120 ms), and DISPON (0x29) — so we
     * do NOT send those again afterwards. The previous override path
     * was actively un-doing the panel's required color inversion. */
    const size_t n = sizeof(kInit) / sizeof(kInit[0]);
    for (size_t i = 0; i < n; ++i) {
        const st77916_init_cmd_t *e = &kInit[i];
        err = tx_param(e->cmd, e->data, e->data_len);
        if (err != ESP_OK) {
            ESP_LOGE(kTag, "init cmd 0x%02X failed (%d)", e->cmd, err);
            return err;
        }
        if (e->delay_ms) {
            vTaskDelay(pdMS_TO_TICKS(e->delay_ms));
        }
    }

    /* Optional inversion override (the array already sent INVON). If the
     * caller explicitly wants inversion DISABLED, send INVOFF here; else
     * leave the array's INVON in effect. */
    if (!s_cfg.invert_colors) {
        err = tx_param(ST77916_CMD_INVOFF, NULL, 0);
        if (err != ESP_OK) return err;
    }

    /* TEON only if a TE pin is wired and configured. */
    if (s_cfg.pin_te >= 0) {
        uint8_t teon = 0x00;
        if (tx_param(ST77916_CMD_TEON, &teon, 1) != ESP_OK) {
            ESP_LOGW(kTag, "TEON failed, continuing without TE sync");
        }
    }

    vTaskDelay(pdMS_TO_TICKS(20));
    return ESP_OK;
}

esp_err_t display_driver_init(const display_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }
    s_cfg = *config;

    ESP_LOGI(kTag, "init: pins clk=%d cs=%d d0..3=%d/%d/%d/%d rst=%d te=%d bl=%d",
             s_cfg.pin_clk, s_cfg.pin_cs, s_cfg.pin_sio0, s_cfg.pin_sio1,
             s_cfg.pin_sio2, s_cfg.pin_sio3, s_cfg.pin_rst, s_cfg.pin_te, s_cfg.pin_bl);

    /* RST + BL as outputs (CS handled by the SPI driver). */
    gpio_config_t io_conf = {};
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = 0;
    if (s_cfg.pin_rst >= 0) io_conf.pin_bit_mask |= (1ULL << s_cfg.pin_rst);
    if (s_cfg.pin_bl  >= 0) io_conf.pin_bit_mask |= (1ULL << s_cfg.pin_bl);
    if (io_conf.pin_bit_mask) gpio_config(&io_conf);

    spi_bus_config_t buscfg = {};
    buscfg.sclk_io_num = s_cfg.pin_clk;
    buscfg.data0_io_num = s_cfg.pin_sio0;
    buscfg.data1_io_num = s_cfg.pin_sio1;
    buscfg.data2_io_num = s_cfg.pin_sio2;
    buscfg.data3_io_num = s_cfg.pin_sio3;
    buscfg.max_transfer_sz = s_cfg.width * s_cfg.height * sizeof(uint16_t) + 16;

    ESP_LOGI(kTag, "spi_bus_initialize host=%d", s_cfg.spi_host);
    esp_err_t err = spi_bus_initialize(s_cfg.spi_host, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(kTag, "spi_bus_initialize failed (%s)", esp_err_to_name(err));
        return err;
    }

    spi_device_interface_config_t devcfg = {};
    devcfg.clock_speed_hz = s_cfg.pclk_hz;
    devcfg.mode = 0;
    devcfg.spics_io_num = s_cfg.pin_cs;
    devcfg.queue_size = 1;
    devcfg.command_bits = 8;   /* QSPI opcode 0x02 / 0x32 */
    devcfg.address_bits = 24;  /* panel cmd (8b) + dummy (16b) */
    devcfg.flags = SPI_DEVICE_HALFDUPLEX;

    ESP_LOGI(kTag, "spi_bus_add_device pclk=%d Hz", s_cfg.pclk_hz);
    err = spi_bus_add_device(s_cfg.spi_host, &devcfg, &s_spi);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "spi_bus_add_device failed (%s)", esp_err_to_name(err));
        return err;
    }

    if (s_cfg.pin_te >= 0) {
        gpio_config_t te_conf = {};
        te_conf.pin_bit_mask = (1ULL << s_cfg.pin_te);
        te_conf.mode = GPIO_MODE_INPUT;
        te_conf.pull_up_en = GPIO_PULLUP_DISABLE;
        te_conf.pull_down_en = GPIO_PULLDOWN_ENABLE;
        te_conf.intr_type = GPIO_INTR_POSEDGE;
        gpio_config(&te_conf);

        if (!s_te_sem) s_te_sem = xSemaphoreCreateBinary();
        if (s_te_sem) {
            esp_err_t isr_err = gpio_install_isr_service(0);
            if (isr_err == ESP_OK || isr_err == ESP_ERR_INVALID_STATE) {
                if (gpio_isr_handler_add(s_cfg.pin_te, display_te_isr, NULL) == ESP_OK) {
                    s_te_enabled = true;
                    ESP_LOGI(kTag, "TE sync enabled on GPIO %d", s_cfg.pin_te);
                }
            }
        }
    }

    ESP_LOGI(kTag, "hw reset + vendor init (%u cmds)", (unsigned)(sizeof(kInit)/sizeof(kInit[0])));
    st77916_reset();
    err = st77916_run_init();
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "vendor init failed (%s)", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(kTag, "panel READY (%dx%d, QSPI %d MHz)",
             s_cfg.width, s_cfg.height, s_cfg.pclk_hz / 1000000);
    display_driver_set_backlight(100);
    return ESP_OK;
}

void display_driver_set_flush_cb(display_flush_done_cb_t cb, void *user_ctx)
{
    s_flush_cb = cb;
    s_flush_ctx = user_ctx;
}

void display_driver_set_backlight(uint8_t percent)
{
    if (s_cfg.pin_bl < 0) return;
    if (percent > 100) percent = 100;

    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER_0,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&timer);

    ledc_channel_config_t channel = {
        .gpio_num = s_cfg.pin_bl,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = (percent * 1023) / 100,
        .hpoint = 0,
    };
    ledc_channel_config(&channel);
}

/* Per-chunk hardware ceiling: ESP32-S3 SPI MOSI length register is 18
 * bits → 32767 bytes max per single `spi_transaction_t`. We split below
 * that with plenty of headroom (16 KB ≈ 22 rows at 360 px wide). */
/* Max single SPI transaction: 32 767 bytes is the ESP32-S3 hardware
 * ceiling (18-bit MOSI length register). 32 000 leaves a safety margin
 * and lets a 40-row (28 800-byte) strip fit in ONE chunk — eliminating
 * the multi-chunk CS_KEEP_ACTIVE pattern for typical LVGL flushes and
 * the strip-based color cycle in main.c. */
#define DISPLAY_FLUSH_MAX_CHUNK_BYTES (32 * 1000)

static esp_err_t set_window(int x1, int y1, int x2, int y2)
{
    uint8_t caset[4] = {
        (uint8_t)((x1 >> 8) & 0xFF), (uint8_t)(x1 & 0xFF),
        (uint8_t)((x2 >> 8) & 0xFF), (uint8_t)(x2 & 0xFF),
    };
    uint8_t raset[4] = {
        (uint8_t)((y1 >> 8) & 0xFF), (uint8_t)(y1 & 0xFF),
        (uint8_t)((y2 >> 8) & 0xFF), (uint8_t)(y2 & 0xFF),
    };
    esp_err_t err = tx_param(ST77916_CMD_CASET, caset, 4);
    if (err != ESP_OK) return err;
    return tx_param(ST77916_CMD_RASET, raset, 4);
}

/* Push a contiguous run of pixel bytes to the panel as ONE continuous
 * QSPI write that crosses multiple SPI transactions. CS is held LOW
 * across all chunks (via `spi_device_acquire_bus` + `SPI_TRANS_CS_KEEP_ACTIVE`)
 * so the panel sees a single RAMWRC stream rather than N restart-from-
 * window-origin RAMWRCs. First chunk carries the `0x32 + 0x003C00`
 * opcode prefix; subsequent chunks skip the cmd/addr/dummy phases
 * (`SPI_TRANS_VARIABLE_*` with all bit-counts = 0) and ride pure data
 * on the four quad lines. Mirrors Arduino_GFX's writePixels exactly. */
static esp_err_t tx_color_stream(const uint8_t *buf, size_t len)
{
    if (!s_spi || !buf || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = spi_device_acquire_bus(s_spi, portMAX_DELAY);
    if (err != ESP_OK) return err;

    bool first = true;
    size_t remaining = len;
    while (remaining > 0) {
        size_t chunk = remaining > DISPLAY_FLUSH_MAX_CHUNK_BYTES
                     ? DISPLAY_FLUSH_MAX_CHUNK_BYTES : remaining;

        spi_transaction_ext_t ext = {};
        ext.base.tx_buffer = buf;
        ext.base.length    = chunk * 8;

        if (first) {
            ext.base.flags = SPI_TRANS_MODE_QIO;
            ext.base.cmd   = ST77916_OPCODE_WRITE_COLOR;
            /* RAMWR (0x2C), not RAMWRC (0x3C) — the manufacturer's
             * ESP_PanelLcd_ST77916 + Espressif's esp_lcd_st77916 both
             * use RAMWR. RAMWRC is "memory write continue", but the
             * panel may not transition cleanly from CASET/RASET into
             * a continuation write — it expects a fresh RAMWR to
             * engage the new window. */
            ext.base.addr  = ((uint32_t)ST77916_CMD_RAMWR) << 8;
            first = false;
        } else {
            /* Skip the cmd/addr/dummy phases entirely — keep streaming
             * data on D0-D3 from where the previous chunk left off. */
            ext.base.flags = SPI_TRANS_MODE_QIO |
                             SPI_TRANS_VARIABLE_CMD |
                             SPI_TRANS_VARIABLE_ADDR |
                             SPI_TRANS_VARIABLE_DUMMY;
            ext.command_bits = 0;
            ext.address_bits = 0;
            ext.dummy_bits   = 0;
        }
        if (remaining > chunk) {
            /* More chunks coming — tell the driver NOT to raise CS yet. */
            ext.base.flags |= SPI_TRANS_CS_KEEP_ACTIVE;
        }

        err = spi_device_polling_transmit(s_spi, (spi_transaction_t *)&ext);
        if (err != ESP_OK) {
            ESP_LOGE(kTag, "tx_color_stream chunk %u bytes failed (%s)",
                     (unsigned)chunk, esp_err_to_name(err));
            break;
        }

        buf       += chunk;
        remaining -= chunk;
    }

    spi_device_release_bus(s_spi);
    return err;
}

void display_driver_flush(int x1, int y1, int x2, int y2, const void *color_data, size_t color_bytes)
{
    if (!s_spi || !color_data) {
        if (s_flush_cb) s_flush_cb(s_flush_ctx);
        return;
    }

    /* PSRAM cache coherency: if the caller's pixel buffer lives in PSRAM,
     * the CPU's L1 cache holds the freshly written pixels but the SPI DMA
     * reads directly from physical memory. Without a writeback flush, the
     * panel receives stale data (typically zeros, i.e. a black screen).
     * `esp_cache_msync` with C2M direction performs the writeback. The
     * UNALIGNED flag lets it cover the surrounding cache lines for buffers
     * that aren't cache-line aligned, which `heap_caps_aligned_alloc(4, …)`
     * gives us. DRAM-resident buffers skip this — internal RAM is
     * DMA-coherent by design. */
    if (esp_ptr_external_ram(color_data)) {
        esp_cache_msync((void *)color_data, color_bytes,
                        ESP_CACHE_MSYNC_FLAG_DIR_C2M |
                        ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    }

    /* TE sync (skipped when pin_te < 0). Only the first chunk of a frame
     * waits — subsequent strips ride the same scan window. */
    if (s_te_enabled && s_te_sem && y1 == 0) {
        xSemaphoreTake(s_te_sem, 0);
        xSemaphoreTake(s_te_sem, pdMS_TO_TICKS(30));
    }

    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 >= s_cfg.width)  x2 = s_cfg.width  - 1;
    if (y2 >= s_cfg.height) y2 = s_cfg.height - 1;

    /* 1) Set the GRAM write window once — toggles CS normally. */
    esp_err_t err = set_window(x1, y1, x2, y2);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "flush: set_window failed (%s)", esp_err_to_name(err));
        if (s_flush_cb) s_flush_cb(s_flush_ctx);
        return;
    }

    /* 2) Stream all pixel bytes as ONE continuous QSPI RAMWRC write
     *    (multiple SPI transactions, single CS-low envelope). */
    err = tx_color_stream((const uint8_t *)color_data, color_bytes);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "flush: tx_color_stream %u bytes failed (%s)",
                 (unsigned)color_bytes, esp_err_to_name(err));
    }

    if (s_flush_cb) s_flush_cb(s_flush_ctx);
}
