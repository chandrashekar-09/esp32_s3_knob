#include "touch_cst816.h"

#include <string.h>

#include "app_mutex.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define CST816_REG_TP_STATUS 0x02
#define CST816_REG_TP_XH 0x03
#define CST816_REG_TP_XL 0x04
#define CST816_REG_TP_YH 0x05
#define CST816_REG_TP_YL 0x06

static const char *kTag = "cst816";
static cst816_config_t s_cfg = {};

static esp_err_t cst816_write(uint8_t reg, uint8_t val)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (s_cfg.i2c_addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_write_byte(cmd, val, true);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(s_cfg.i2c_port, cmd, pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(cmd);
    return err;
}

static esp_err_t cst816_read_reg(uint8_t reg, uint8_t *data, size_t len)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (s_cfg.i2c_addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (s_cfg.i2c_addr << 1) | I2C_MASTER_READ, true);
    if (len > 1) {
        i2c_master_read(cmd, data, len - 1, I2C_MASTER_ACK);
    }
    i2c_master_read_byte(cmd, data + len - 1, I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(s_cfg.i2c_port, cmd, pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(cmd);
    return err;
}

static void cst816_reset(void)
{
    if (s_cfg.pin_rst < 0) {
        return;
    }

    gpio_set_level(s_cfg.pin_rst, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(s_cfg.pin_rst, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
}

esp_err_t cst816_init(const cst816_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    s_cfg = *config;
    app_mutex_init();

    i2c_config_t i2c_conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = s_cfg.pin_sda,
        .scl_io_num = s_cfg.pin_scl,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 400000,
    };
    ESP_ERROR_CHECK(i2c_param_config(s_cfg.i2c_port, &i2c_conf));
    ESP_ERROR_CHECK(i2c_driver_install(s_cfg.i2c_port, I2C_MODE_MASTER, 0, 0, 0));

    if (s_cfg.pin_rst >= 0) {
        gpio_config_t io_conf = {
            .pin_bit_mask = (1ULL << s_cfg.pin_rst),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&io_conf);
    }

    cst816_reset();

    uint8_t status = 0;
    if (cst816_read_reg(CST816_REG_TP_STATUS, &status, 1) != ESP_OK) {
        ESP_LOGW(kTag, "touch read failed");
    }

    return ESP_OK;
}

bool cst816_read(cst816_point_t *point)
{
    if (!point) {
        return false;
    }

    uint8_t buf[5] = {0};
    if (cst816_read_reg(CST816_REG_TP_STATUS, buf, sizeof(buf)) != ESP_OK) {
        point->touched = false;
        return false;
    }

    uint8_t touched = buf[0] & 0x0F;
    if (touched == 0) {
        point->touched = false;
        return false;
    }

    uint16_t x = ((buf[1] & 0x0F) << 8) | buf[2];
    uint16_t y = ((buf[3] & 0x0F) << 8) | buf[4];

    if (s_cfg.swap_xy) {
        uint16_t tmp = x;
        x = y;
        y = tmp;
    }
    if (s_cfg.invert_x) {
        x = (s_cfg.max_x > 0) ? (s_cfg.max_x - 1 - x) : x;
    }
    if (s_cfg.invert_y) {
        y = (s_cfg.max_y > 0) ? (s_cfg.max_y - 1 - y) : y;
    }

    if (s_cfg.max_x > 0 && x >= s_cfg.max_x) {
        x = s_cfg.max_x - 1;
    }
    if (s_cfg.max_y > 0 && y >= s_cfg.max_y) {
        y = s_cfg.max_y - 1;
    }

    point->touched = true;
    point->x = x;
    point->y = y;
    return true;
}
