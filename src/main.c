/*
 * JC3636K718 ESP32-S3 1.8" knob — LVGL UI bring-up (ESP-IDF).
 *
 * Core 1: UI render loop + input (LVGL, touch, encoder) per structure.md.
 * Core 0: WiFi STA + OTA poller (esp_wifi internals + ota_task).
 */

#include <string.h>

#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_common.h"

#include "app_config.h"
#include "app_mutex.h"
#include "inactivity_alert.h"
#include "input_encoder.h"
#include "lvgl_port.h"
#include "mesh_service.h"
#include "ota_service.h"
#include "phase_manager.h"
#include "sd_card.h"
#include "ui_engine.h"
#include "wifi_manager.h"

static const char *TAG_MAIN = "knob";

static void encoder_event_handler(const encoder_event_t *evt, void *ctx)
{
    (void)ctx;
    if (!evt) {
        return;
    }

    if (evt->delta != 0) {
        ESP_LOGI(TAG_MAIN, "ENC rotate %+d  → phase=%d", evt->delta,
                 (int)phase_manager_get_phase());
        phase_manager_on_encoder(evt->delta);
    }
    if (evt->pressed) {
        const char *kind = evt->duration_ms >= 1500 ? "LONG" : "SHORT";
        ESP_LOGI(TAG_MAIN, "ENC press %s (%ums)  → phase=%d", kind,
                 (unsigned)evt->duration_ms, (int)phase_manager_get_phase());
        phase_manager_on_button(true, evt->duration_ms);
    }
}

static void input_init(void)
{
    encoder_config_t enc_cfg = {
        .pin_a = APP_PIN_ENC_A,
        .pin_b = APP_PIN_ENC_B,
        .pin_btn = APP_PIN_ENC_BTN,
        .glitch_filter_us = 10,
    };
    ESP_ERROR_CHECK(encoder_init(&enc_cfg, encoder_event_handler, NULL));
}

static void ui_task(void *arg)
{
    (void)arg;

    display_config_t disp_cfg = {
        .spi_host = SPI2_HOST,
        .pin_clk = APP_LCD_PIN_CLK,
        .pin_cs = APP_LCD_PIN_CS,
        .pin_sio0 = APP_LCD_PIN_SIO0,
        .pin_sio1 = APP_LCD_PIN_SIO1,
        .pin_sio2 = APP_LCD_PIN_SIO2,
        .pin_sio3 = APP_LCD_PIN_SIO3,
        .pin_rst = APP_LCD_PIN_RST,
        .pin_te = APP_LCD_PIN_TE,
        .pin_bl = APP_LCD_PIN_BL,
        .width = APP_DISPLAY_WIDTH,
        .height = APP_DISPLAY_HEIGHT,
        .pclk_hz = APP_DISPLAY_PCLK_HZ,
        .quad_mode = true,
        .invert_colors = true,
    };

    cst816_config_t touch_cfg = {
        .i2c_port = I2C_NUM_0,
        .pin_sda = APP_TOUCH_PIN_SDA,
        .pin_scl = APP_TOUCH_PIN_SCL,
        .pin_int = APP_TOUCH_PIN_INT,
        .pin_rst = APP_TOUCH_PIN_RST,
        .i2c_addr = 0x15,
        .max_x = APP_DISPLAY_WIDTH,
        .max_y = APP_DISPLAY_HEIGHT,
        /* 180° rotation: matches MADCTL=0xC0 (MY|MX) in display_driver.c.
         * Both axes inverted, no swap (90° rotations would set swap_xy). */
        .swap_xy = false,
        .invert_x = true,
        .invert_y = true,
    };

    if (!lvgl_port_init(&disp_cfg, &touch_cfg)) {
        ESP_LOGE(TAG_MAIN, "lvgl port init failed; UI task exiting");
        vTaskDelay(pdMS_TO_TICKS(1000));
        vTaskDelete(NULL);
    }

    ui_engine_init();
    ui_engine_set_mesh_state(false, false, -1);
    ui_engine_set_ota_status("LOCAL UI");

    while (true) {
        phase_manager_tick((uint32_t)(esp_timer_get_time() / 1000ULL));
        ui_engine_set_phase(phase_manager_get_phase());
        ui_engine_render();
        /* Drive the LED/beep cadence while the inactivity alert is
         * active. Idempotent when idle. */
        inactivity_alert_tick();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

void app_main(void)
{
    /* Log wake source — the inactivity sequence ends with deep sleep
     * which causes a full chip reset. Verifying that this code path
     * runs on wake confirms the "infinite cycle" works: cold boot →
     * 5 min idle → 3-alert sequence → reset queue → deep sleep →
     * wake on encoder/touch → THIS LOG → fresh 5 min idle timer →
     * repeats forever. If you see "woke from deep sleep" after the
     * "FINAL — reset queue, deep sleep" log line, the loop is alive. */
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    if (cause == ESP_SLEEP_WAKEUP_EXT1) {
        ESP_LOGI(TAG_MAIN, "woke from deep sleep (encoder/touch)");
    } else if (cause != ESP_SLEEP_WAKEUP_UNDEFINED) {
        ESP_LOGI(TAG_MAIN, "woke from sleep (cause=%d)", (int)cause);
    } else {
        ESP_LOGI(TAG_MAIN, "cold boot");
    }

    app_mutex_init();
    phase_manager_init();
    input_init();
    /* Pre-init the I2S beep + MUTE GPIO so failures are logged at
     * boot rather than at the first inactivity trigger. */
    inactivity_alert_init();

    /* Mount the on-board microSD slot so the triple-tap screenshot
     * feature has somewhere to write. Failure (no card / unformatted)
     * is logged but non-fatal — UI still boots, screenshot path
     * checks sd_card_is_mounted() before any fopen. */
    sd_card_mount();

    /* UI owns Core 1 — render loop + LVGL + encoder polling. */
    xTaskCreatePinnedToCore(ui_task, "ui_task", 6144, NULL, 4, NULL, 1);

    /* Networking stack on Core 0. Brought up AFTER the UI task is
     * created so the first paint (BOOT screen) lands before WiFi
     * eats cycles. Two mutually-exclusive modes per FEATURE_ZERO_
     * CONFIG_MESH:
     *
     *   ON  → mesh_service_start(): WiFi STA without AP + LR PHY,
     *         ESP-NOW broadcast peer, auto-slot-claim, 1 Hz state
     *         broadcaster. Fully offline; no internet needed.
     *
     *   OFF → wifi_manager_init() + ota_service_start(): connects
     *         to APP_MESH_ROUTER_SSID, polls HTTPS OTA endpoint
     *         every minute. Lab/dev mode. */
#if FEATURE_ZERO_CONFIG_MESH
    if (mesh_service_start() != ESP_OK) {
        ESP_LOGW(TAG_MAIN, "mesh service start failed — running solo");
    }
#else
    if (wifi_manager_init() == ESP_OK) {
        ota_service_start();
    } else {
        ESP_LOGW(TAG_MAIN, "wifi init failed — OTA polling disabled");
    }
#endif

    ESP_LOGI(TAG_MAIN, "ui firmware started");
}
