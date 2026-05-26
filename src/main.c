/* JC3636K718 ESP32-S3 1.8" knob — hardware bring-up firmware (ESP-IDF).
 *
 * Three independent peripherals, each driven from its own FreeRTOS task:
 *
 *   display_task  (core 1, prio 5) — ST77916 QSPI panel, full-frame
 *                                    PSRAM buffer, cycles 8 colors.
 *   touch_task    (core 1, prio 3) — CST816 I2C, logs down/move/up.
 *   encoder_task  (PCNT-driven by input_encoder.c on core 1, prio 5)
 *                 — quad rotary + button. Events come in via callback.
 *
 * No mesh, no OTA, no LVGL. The goal of this build is to prove the
 * three peripherals are alive after a power-up and to give a clean
 * baseline to add UI on top of.
 *
 * Pin map, display resolution, and SPI clock live in app_config.h.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_config.h"
#include "display_driver.h"
#include "input_encoder.h"
#include "touch_cst816.h"

static const char *TAG_MAIN    = "knob";
static const char *TAG_DISPLAY = "display";
static const char *TAG_TOUCH   = "touch";
static const char *TAG_ENC     = "encoder";

/* ──────────────────────────────────────────────────────────────────────
 *  Color helpers
 * ────────────────────────────────────────────────────────────────────── */

/* The panel is configured for RGB565 via COLMOD 0x55. Pixel bytes go out
 * MSB-first on the wire, so we pre-swap the two bytes here once and the
 * wire format ends up correct without per-pixel byte-swap in the inner
 * loop. */
static inline uint16_t rgb565_be(uint8_t r, uint8_t g, uint8_t b)
{
    uint16_t v = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    return (uint16_t)((v >> 8) | (v << 8));
}

typedef struct {
    const char *name;
    uint16_t    pixel;   /* RGB565 in wire byte order */
} color_step_t;

static const color_step_t kColors[] = {
    { "RED",     0 },
    { "GREEN",   0 },
    { "BLUE",    0 },
    { "WHITE",   0 },
    { "BLACK",   0 },
    { "YELLOW",  0 },
    { "CYAN",    0 },
    { "MAGENTA", 0 },
};
#define NUM_COLORS (sizeof(kColors) / sizeof(kColors[0]))

/* Populated at runtime to avoid relying on a static initializer that
 * calls rgb565_be() (which is fine in GCC but cleaner this way). */
static uint16_t s_color_pixel[NUM_COLORS];

static void init_color_table(void)
{
    s_color_pixel[0] = rgb565_be(0xFF, 0x00, 0x00); /* RED     */
    s_color_pixel[1] = rgb565_be(0x00, 0xFF, 0x00); /* GREEN   */
    s_color_pixel[2] = rgb565_be(0x00, 0x00, 0xFF); /* BLUE    */
    s_color_pixel[3] = rgb565_be(0xFF, 0xFF, 0xFF); /* WHITE   */
    s_color_pixel[4] = rgb565_be(0x00, 0x00, 0x00); /* BLACK   */
    s_color_pixel[5] = rgb565_be(0xFF, 0xFF, 0x00); /* YELLOW  */
    s_color_pixel[6] = rgb565_be(0x00, 0xFF, 0xFF); /* CYAN    */
    s_color_pixel[7] = rgb565_be(0xFF, 0x00, 0xFF); /* MAGENTA */
}

/* ──────────────────────────────────────────────────────────────────────
 *  Display task
 *
 *  Allocates a full-frame buffer in PSRAM (360*360*2 = 252 KB), fills
 *  it with one color per cycle, and pushes the whole thing in a single
 *  SPI DMA transaction. This is the same pattern Arduino_GFX uses and
 *  is faster + cleaner than scanline-by-scanline pushes.
 * ────────────────────────────────────────────────────────────────────── */

static void display_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG_DISPLAY, "task started on core %d", xPortGetCoreID());

    display_config_t cfg = {
        .spi_host = SPI2_HOST,
        .pin_clk  = APP_LCD_PIN_CLK,
        .pin_cs   = APP_LCD_PIN_CS,
        .pin_sio0 = APP_LCD_PIN_SIO0,
        .pin_sio1 = APP_LCD_PIN_SIO1,
        .pin_sio2 = APP_LCD_PIN_SIO2,
        .pin_sio3 = APP_LCD_PIN_SIO3,
        .pin_rst  = APP_LCD_PIN_RST,
        .pin_te   = APP_LCD_PIN_TE,   /* -1 disables TE wait — confirmed
                                       * working on this hardware. */
        .pin_bl   = APP_LCD_PIN_BL,
        .width    = APP_DISPLAY_WIDTH,
        .height   = APP_DISPLAY_HEIGHT,
        .pclk_hz  = APP_DISPLAY_PCLK_HZ,
        .quad_mode = true,
        /* The JC3636K718 panel needs inversion ON — the manufacturer's
         * init array sends INVON (0x21), and the demo also calls
         * lcd->invertColor(true). Setting this to false causes the
         * driver to send INVOFF afterwards and the displayed colors
         * appear inverted (red looks cyan, etc). */
        .invert_colors = true,
    };

    if (display_driver_init(&cfg) != ESP_OK) {
        ESP_LOGE(TAG_DISPLAY, "init FAILED — task exiting");
        vTaskDelete(NULL);
        return;
    }

    /* Full-frame buffer in PSRAM. DMA-friendly aligned alloc. */
    const size_t frame_bytes =
        (size_t)APP_DISPLAY_WIDTH * APP_DISPLAY_HEIGHT * sizeof(uint16_t);
    uint16_t *frame = heap_caps_aligned_alloc(
        4, frame_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT | MALLOC_CAP_DMA);
    if (!frame) {
        /* DMA-capable PSRAM may not be available on all SDKs — fall back
         * to plain PSRAM alloc which the SPI driver will bounce through
         * internal DMA. */
        frame = heap_caps_aligned_alloc(
            4, frame_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!frame) {
        ESP_LOGE(TAG_DISPLAY, "PSRAM frame buffer alloc FAILED (%u bytes)",
                 (unsigned)frame_bytes);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG_DISPLAY, "frame buf: %u bytes in PSRAM @ %p",
             (unsigned)frame_bytes, frame);

    ESP_LOGI(TAG_DISPLAY, "entering color cycle");
    size_t idx = 0;
    const size_t pixels = (size_t)APP_DISPLAY_WIDTH * APP_DISPLAY_HEIGHT;
    while (true) {
        const color_step_t *c = &kColors[idx];
        uint16_t v = s_color_pixel[idx];

        int64_t t0 = esp_timer_get_time();
        for (size_t i = 0; i < pixels; ++i) frame[i] = v;
        display_driver_flush(0, 0, APP_DISPLAY_WIDTH - 1,
                             APP_DISPLAY_HEIGHT - 1, frame, frame_bytes);
        int64_t t1 = esp_timer_get_time();

        ESP_LOGI(TAG_DISPLAY, "FILL %-7s 0x%04X  (%lld ms)",
                 c->name, v, (long long)((t1 - t0) / 1000));

        vTaskDelay(pdMS_TO_TICKS(1500));
        idx = (idx + 1) % NUM_COLORS;
    }
}

/* ──────────────────────────────────────────────────────────────────────
 *  Touch task
 * ────────────────────────────────────────────────────────────────────── */

static void touch_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG_TOUCH, "task started on core %d", xPortGetCoreID());

    cst816_config_t cfg = {
        .i2c_port = I2C_NUM_0,
        .pin_sda  = APP_TOUCH_PIN_SDA,
        .pin_scl  = APP_TOUCH_PIN_SCL,
        .pin_int  = APP_TOUCH_PIN_INT,
        .pin_rst  = APP_TOUCH_PIN_RST,
        .i2c_addr = 0x15,
        .max_x = APP_DISPLAY_WIDTH,
        .max_y = APP_DISPLAY_HEIGHT,
        .swap_xy = false,
        .invert_x = false,
        .invert_y = false,
    };

    if (cst816_init(&cfg) != ESP_OK) {
        ESP_LOGE(TAG_TOUCH, "init FAILED — task exiting");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG_TOUCH, "ready (sda=%d scl=%d int=%d rst=%d, addr=0x%02X)",
             APP_TOUCH_PIN_SDA, APP_TOUCH_PIN_SCL,
             APP_TOUCH_PIN_INT, APP_TOUCH_PIN_RST, cfg.i2c_addr);

    bool      prev_down       = false;
    uint32_t  move_log_throttle = 0;
    while (true) {
        cst816_point_t pt = { 0 };
        bool down = cst816_read(&pt) && pt.touched;

        if (down && !prev_down) {
            ESP_LOGI(TAG_TOUCH, "down  (%u, %u)", pt.x, pt.y);
        } else if (!down && prev_down) {
            ESP_LOGI(TAG_TOUCH, "up");
        } else if (down) {
            /* Throttle: log roughly every 150 ms of continuous motion. */
            if ((++move_log_throttle % 5) == 0) {
                ESP_LOGI(TAG_TOUCH, "move  (%u, %u)", pt.x, pt.y);
            }
        }
        prev_down = down;
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

/* ──────────────────────────────────────────────────────────────────────
 *  Encoder + button — events arrive via callback from input_encoder.c
 * ────────────────────────────────────────────────────────────────────── */

static void on_encoder_event(const encoder_event_t *evt, void *ctx)
{
    (void)ctx;
    if (!evt) return;

    if (evt->delta != 0) {
        ESP_LOGI(TAG_ENC, "rotate delta=%+d", evt->delta);
    }
    if (evt->pressed) {
        const char *kind = evt->duration_ms >= 2000 ? "LONG"
                         : evt->duration_ms >= 500  ? "MED"
                                                    : "SHORT";
        ESP_LOGI(TAG_ENC, "button %s (%ums)", kind,
                 (unsigned)evt->duration_ms);
    }
}

static esp_err_t encoder_setup(void)
{
    encoder_config_t cfg = {
        .pin_a   = APP_PIN_ENC_A,
        .pin_b   = APP_PIN_ENC_B,
        .pin_btn = APP_PIN_ENC_BTN,
        .glitch_filter_us = 10,
    };
    ESP_LOGI(TAG_ENC, "pins A=%d B=%d BTN=%d",
             APP_PIN_ENC_A, APP_PIN_ENC_B, APP_PIN_ENC_BTN);
    return encoder_init(&cfg, on_encoder_event, NULL);
}

/* ──────────────────────────────────────────────────────────────────────
 *  Boot
 * ────────────────────────────────────────────────────────────────────── */

void app_main(void)
{
    /* Short countdown so `pio device monitor` started manually after
     * reset still catches the first display init log. */
    for (int i = 2; i > 0; --i) {
        ESP_LOGI(TAG_MAIN, "============ boot in %ds ============", i);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    ESP_LOGI(TAG_MAIN, "================================================");
    ESP_LOGI(TAG_MAIN, "JC3636K718 knob bring-up firmware");
    ESP_LOGI(TAG_MAIN, "build %s %s", __DATE__, __TIME__);
    ESP_LOGI(TAG_MAIN, "lcd pins   clk=%d cs=%d d0/1/2/3=%d/%d/%d/%d "
             "rst=%d te=%d bl=%d  @ %d MHz",
             APP_LCD_PIN_CLK, APP_LCD_PIN_CS,
             APP_LCD_PIN_SIO0, APP_LCD_PIN_SIO1,
             APP_LCD_PIN_SIO2, APP_LCD_PIN_SIO3,
             APP_LCD_PIN_RST, APP_LCD_PIN_TE, APP_LCD_PIN_BL,
             APP_DISPLAY_PCLK_HZ / 1000000);
    ESP_LOGI(TAG_MAIN, "touch pins sda=%d scl=%d int=%d rst=%d",
             APP_TOUCH_PIN_SDA, APP_TOUCH_PIN_SCL,
             APP_TOUCH_PIN_INT, APP_TOUCH_PIN_RST);
    ESP_LOGI(TAG_MAIN, "================================================");

    init_color_table();

    ESP_ERROR_CHECK(encoder_setup());

    BaseType_t r;
    r = xTaskCreatePinnedToCore(display_task, "display",
                                4096, NULL, 5, NULL, 1);
    ESP_LOGI(TAG_MAIN, "display task created (rc=%d)", (int)r);

    r = xTaskCreatePinnedToCore(touch_task, "touch",
                                4096, NULL, 3, NULL, 1);
    ESP_LOGI(TAG_MAIN, "touch task created (rc=%d)", (int)r);

    ESP_LOGI(TAG_MAIN, "running — watch for [display] [touch] [encoder] logs");
}
