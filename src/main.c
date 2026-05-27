/*
 * JC3636K718 ESP32-S3 1.8" knob — LVGL UI bring-up (ESP-IDF).
 *
 * Core 1: UI render loop + input (LVGL, touch, encoder) per structure.md.
 * Core 0: unused for now (no mesh/network yet).
 */

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_common.h"

#include "app_config.h"
#include "app_mutex.h"
#include "input_encoder.h"
#include "lvgl_port.h"
#include "phase_manager.h"
#include "ui_engine.h"

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
        .swap_xy = false,
        .invert_x = false,
        .invert_y = false,
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
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

void app_main(void)
{
    app_mutex_init();
    phase_manager_init();
    input_init();

    xTaskCreatePinnedToCore(ui_task, "ui_task", 6144, NULL, 4, NULL, 1);
    ESP_LOGI(TAG_MAIN, "ui firmware started");
}
