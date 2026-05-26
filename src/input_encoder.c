#include "input_encoder.h"

#include "driver/gpio.h"
#include "driver/pulse_cnt.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *kTag = "encoder";

static encoder_event_cb_t s_cb = NULL;
static void *s_cb_ctx = NULL;
static pcnt_unit_handle_t s_unit = NULL;
static pcnt_channel_handle_t s_chan_a = NULL;
static pcnt_channel_handle_t s_chan_b = NULL;
static int64_t s_btn_down_us = 0;
static int64_t s_btn_last_edge_us = 0;
static int s_btn_pin = -1;
static int s_btn_last_level = 1;
static int s_last_count = 0;

static void encoder_report_delta(int delta)
{
    if (!s_cb || delta == 0) {
        return;
    }

    encoder_event_t evt = {
        .delta = delta,
        .pressed = false,
        .duration_ms = 0,
    };
    s_cb(&evt, s_cb_ctx);
}

static void encoder_report_button(uint32_t duration_ms)
{
    if (!s_cb) {
        return;
    }

    encoder_event_t evt = {
        .delta = 0,
        .pressed = true,
        .duration_ms = duration_ms,
    };
    s_cb(&evt, s_cb_ctx);
}

static void encoder_poll_button(void)
{
    if (s_btn_pin < 0) {
        return;
    }

    int level = gpio_get_level(s_btn_pin);
    if (level == s_btn_last_level) {
        return;
    }

    int64_t now = esp_timer_get_time();
    if ((now - s_btn_last_edge_us) < 25000) {
        return;
    }
    s_btn_last_edge_us = now;
    s_btn_last_level = level;

    if (level == 0) {
        s_btn_down_us = now;
        return;
    }

    if (s_btn_down_us != 0) {
        uint32_t duration_ms = (uint32_t)((now - s_btn_down_us) / 1000);
        s_btn_down_us = 0;
        encoder_report_button(duration_ms);
    }
}

static void encoder_task(void *arg)
{
    (void)arg;
    while (true) {
        int count = 0;
        if (pcnt_unit_get_count(s_unit, &count) == ESP_OK) {
            int delta = count - s_last_count;
            if (delta != 0) {
                s_last_count = count;
                encoder_report_delta(delta);
            }
        }
        encoder_poll_button();
        /* 10ms not 5ms — at CONFIG_FREERTOS_HZ=100 the latter rounds to 0
         * ticks (vTaskDelay(0)) which doesn't actually block, starving
         * IDLE1 and tripping the task watchdog. */
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

esp_err_t encoder_init(const encoder_config_t *config, encoder_event_cb_t cb, void *ctx)
{
    if (!config || config->pin_a < 0 || config->pin_b < 0) {
        return ESP_ERR_INVALID_ARG;
    }

    s_cb = cb;
    s_cb_ctx = ctx;

    pcnt_unit_config_t unit_cfg = {
        .low_limit = -32768,
        .high_limit = 32767,
    };
    ESP_ERROR_CHECK(pcnt_new_unit(&unit_cfg, &s_unit));

    pcnt_glitch_filter_config_t filter_cfg = {
        .max_glitch_ns = (uint32_t)config->glitch_filter_us * 1000,
    };
    esp_err_t err = pcnt_unit_set_glitch_filter(s_unit, &filter_cfg);
    if (err != ESP_OK) {
        return err;
    }

    pcnt_chan_config_t chan_a_cfg = {
        .edge_gpio_num = config->pin_a,
        .level_gpio_num = config->pin_b,
    };
    ESP_ERROR_CHECK(pcnt_new_channel(s_unit, &chan_a_cfg, &s_chan_a));
    ESP_ERROR_CHECK(pcnt_channel_set_edge_action(s_chan_a, PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_DECREASE));
    ESP_ERROR_CHECK(pcnt_channel_set_level_action(s_chan_a, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE));

    pcnt_chan_config_t chan_b_cfg = {
        .edge_gpio_num = config->pin_b,
        .level_gpio_num = config->pin_a,
    };
    ESP_ERROR_CHECK(pcnt_new_channel(s_unit, &chan_b_cfg, &s_chan_b));
    ESP_ERROR_CHECK(pcnt_channel_set_edge_action(s_chan_b, PCNT_CHANNEL_EDGE_ACTION_DECREASE, PCNT_CHANNEL_EDGE_ACTION_INCREASE));
    ESP_ERROR_CHECK(pcnt_channel_set_level_action(s_chan_b, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE));

    pcnt_unit_enable(s_unit);
    pcnt_unit_clear_count(s_unit);
    pcnt_unit_start(s_unit);

    if (config->pin_btn >= 0) {
        s_btn_pin = config->pin_btn;
        gpio_config_t io_conf = {
            .intr_type = GPIO_INTR_DISABLE,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pin_bit_mask = (1ULL << config->pin_btn),
        };
        gpio_config(&io_conf);
        s_btn_last_level = gpio_get_level(config->pin_btn);
    }

    pcnt_unit_get_count(s_unit, &s_last_count);

    xTaskCreatePinnedToCore(encoder_task, "encoder_task", 3072, NULL, 5, NULL, 1);

    ESP_LOGI(kTag, "encoder ready");
    return ESP_OK;
}
