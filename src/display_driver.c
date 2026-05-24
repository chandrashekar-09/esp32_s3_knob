#include "display_driver.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define ST77916_CMD_SWRESET 0x01
#define ST77916_CMD_SLPOUT  0x11
#define ST77916_CMD_DISPON  0x29
#define ST77916_CMD_CASET   0x2A
#define ST77916_CMD_RASET   0x2B
#define ST77916_CMD_RAMWR   0x2C
#define ST77916_CMD_MADCTL  0x36
#define ST77916_CMD_COLMOD  0x3A
#define ST77916_CMD_INVON   0x21
#define ST77916_CMD_INVOFF  0x20

static const char *kTag = "st77916";

static spi_device_handle_t s_spi = NULL;
static display_config_t s_cfg = {};
static display_flush_done_cb_t s_flush_cb = NULL;
static void *s_flush_ctx = NULL;

static esp_err_t st77916_write_cmd(uint8_t cmd)
{
    if (!s_spi) {
        return ESP_ERR_INVALID_STATE;
    }

    spi_transaction_ext_t t = {};
    t.base.cmd = cmd;
    t.base.length = 0;
    t.base.flags = 0;

    return spi_device_polling_transmit(s_spi, (spi_transaction_t *)&t);
}

static esp_err_t st77916_write_data(const void *data, size_t len, bool use_quad)
{
    if (!s_spi || !data || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    spi_transaction_ext_t t = {};
    t.base.length = len * 8;
    t.base.tx_buffer = data;
    t.base.flags = use_quad ? SPI_TRANS_MODE_QIO : 0;

    return spi_device_polling_transmit(s_spi, (spi_transaction_t *)&t);
}

static void st77916_reset(void)
{
    if (s_cfg.pin_rst < 0) {
        return;
    }

    gpio_set_level(s_cfg.pin_rst, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(s_cfg.pin_rst, 1);
    vTaskDelay(pdMS_TO_TICKS(120));
}

static esp_err_t st77916_init_sequence(void)
{
    esp_err_t err = st77916_write_cmd(ST77916_CMD_SWRESET);
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(120));

    err = st77916_write_cmd(ST77916_CMD_SLPOUT);
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(120));

    uint8_t colmod = 0x55;
    err = st77916_write_cmd(ST77916_CMD_COLMOD);
    if (err != ESP_OK) {
        return err;
    }
    err = st77916_write_data(&colmod, sizeof(colmod), s_cfg.quad_mode);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t madctl = 0x00;
    err = st77916_write_cmd(ST77916_CMD_MADCTL);
    if (err != ESP_OK) {
        return err;
    }
    err = st77916_write_data(&madctl, sizeof(madctl), s_cfg.quad_mode);
    if (err != ESP_OK) {
        return err;
    }

    if (s_cfg.invert_colors) {
        err = st77916_write_cmd(ST77916_CMD_INVON);
    } else {
        err = st77916_write_cmd(ST77916_CMD_INVOFF);
    }
    if (err != ESP_OK) {
        return err;
    }

    err = st77916_write_cmd(ST77916_CMD_DISPON);
    if (err != ESP_OK) {
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(50));
    return ESP_OK;
}

static esp_err_t st77916_set_window(int x1, int y1, int x2, int y2)
{
    uint8_t data[4] = {0};

    data[0] = (x1 >> 8) & 0xFF;
    data[1] = x1 & 0xFF;
    data[2] = (x2 >> 8) & 0xFF;
    data[3] = x2 & 0xFF;
    ESP_ERROR_CHECK(st77916_write_cmd(ST77916_CMD_CASET));
    ESP_ERROR_CHECK(st77916_write_data(data, sizeof(data), s_cfg.quad_mode));

    data[0] = (y1 >> 8) & 0xFF;
    data[1] = y1 & 0xFF;
    data[2] = (y2 >> 8) & 0xFF;
    data[3] = y2 & 0xFF;
    ESP_ERROR_CHECK(st77916_write_cmd(ST77916_CMD_RASET));
    ESP_ERROR_CHECK(st77916_write_data(data, sizeof(data), s_cfg.quad_mode));

    return st77916_write_cmd(ST77916_CMD_RAMWR);
}

esp_err_t display_driver_init(const display_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    s_cfg = *config;

    gpio_config_t io_conf = {};
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << s_cfg.pin_cs);
    if (s_cfg.pin_rst >= 0) {
        io_conf.pin_bit_mask |= (1ULL << s_cfg.pin_rst);
    }
    if (s_cfg.pin_bl >= 0) {
        io_conf.pin_bit_mask |= (1ULL << s_cfg.pin_bl);
    }
    gpio_config(&io_conf);

    spi_bus_config_t buscfg = {};
    buscfg.sclk_io_num = s_cfg.pin_clk;
    buscfg.mosi_io_num = s_cfg.pin_sio0;
    buscfg.miso_io_num = s_cfg.pin_sio1;
    buscfg.quadwp_io_num = s_cfg.pin_sio2;
    buscfg.quadhd_io_num = s_cfg.pin_sio3;
    buscfg.max_transfer_sz = s_cfg.width * s_cfg.height * 2 + 8;

    esp_err_t err = spi_bus_initialize(s_cfg.spi_host, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    spi_device_interface_config_t devcfg = {};
    devcfg.clock_speed_hz = s_cfg.pclk_hz;
    devcfg.mode = 0;
    devcfg.spics_io_num = s_cfg.pin_cs;
    devcfg.queue_size = 1;
    devcfg.command_bits = 8;
    devcfg.flags = SPI_DEVICE_HALFDUPLEX;

    err = spi_bus_add_device(s_cfg.spi_host, &devcfg, &s_spi);
    if (err != ESP_OK) {
        return err;
    }

    st77916_reset();
    err = st77916_init_sequence();
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "init failed (%d)", err);
        return err;
    }

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
    if (s_cfg.pin_bl < 0) {
        return;
    }

    if (percent > 100) {
        percent = 100;
    }

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

void display_driver_flush(int x1, int y1, int x2, int y2, const void *color_data, size_t color_bytes)
{
    if (!s_spi || !color_data) {
        if (s_flush_cb) {
            s_flush_cb(s_flush_ctx);
        }
        return;
    }

    if (x1 < 0) {
        x1 = 0;
    }
    if (y1 < 0) {
        y1 = 0;
    }
    if (x2 >= s_cfg.width) {
        x2 = s_cfg.width - 1;
    }
    if (y2 >= s_cfg.height) {
        y2 = s_cfg.height - 1;
    }

    if (st77916_set_window(x1, y1, x2, y2) == ESP_OK) {
        st77916_write_data(color_data, color_bytes, s_cfg.quad_mode);
    }

    if (s_flush_cb) {
        s_flush_cb(s_flush_ctx);
    }
}
