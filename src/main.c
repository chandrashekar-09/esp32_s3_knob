#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "input_encoder.h"
#include "lvgl_port.h"
#include "mesh_manager.h"
#include "mesh_ota.h"
#include "phase_manager.h"
#include "ui_engine.h"

static const char *kTag = "knob_main";

#define PIN_ENC_A 2
#define PIN_ENC_B 1
#define PIN_ENC_BTN 0

#define LCD_PIN_CLK 11
#define LCD_PIN_CS 12
#define LCD_PIN_SIO0 13
#define LCD_PIN_SIO1 14
#define LCD_PIN_SIO2 15
#define LCD_PIN_SIO3 16
#define LCD_PIN_RST 17
#define LCD_PIN_TE 18
#define LCD_PIN_BL 21

#define TOUCH_PIN_SDA 9
#define TOUCH_PIN_SCL 10
#define TOUCH_PIN_INT 7
#define TOUCH_PIN_RST 8

#define MESH_ROUTER_SSID   "TEAMPLAYER 9060"
#define MESH_ROUTER_PASS   "7mA82;58"

// #define MESH_ROUTER_SSID   "IIIT-Guest"
// #define MESH_ROUTER_PASS   "f6s68VHJ89mC"

#define MESH_AP_PASS       "mesh_ap_pass"

#define OTA_CURRENT_VERSION 2
#define OTA_VERSION_URL "https://raw.githubusercontent.com/chandrashekar-09/esp32_s3_knob/main/version.txt"
#define OTA_FIRMWARE_URL "https://raw.githubusercontent.com/chandrashekar-09/esp32_s3_knob/main/.pio/build/esp32-s3/firmware.bin"
#define OTA_DEVICE_ID "knob-002"
#define OTA_BOOT_ACK_BASE_URL "https://techlora-369-default-rtdb.asia-southeast1.firebasedatabase.app/boot_ack"
#define OTA_BOOT_ACK_AUTH ""
#define OTA_EXPECTED_SHA256 ""

static void encoder_event_handler(const encoder_event_t *evt, void *ctx)
{
    (void)ctx;
    if (!evt) {
        return;
    }

    if (evt->delta != 0) {
        phase_manager_on_encoder(evt->delta);
    }
    if (evt->pressed) {
        phase_manager_on_button(true, evt->duration_ms);
    }
    ui_engine_set_phase(phase_manager_get_phase());
}

static void ui_task(void *arg)
{
    display_config_t disp_cfg = {
        .spi_host = SPI2_HOST,
        .pin_clk = LCD_PIN_CLK,
        .pin_cs = LCD_PIN_CS,
        .pin_sio0 = LCD_PIN_SIO0,
        .pin_sio1 = LCD_PIN_SIO1,
        .pin_sio2 = LCD_PIN_SIO2,
        .pin_sio3 = LCD_PIN_SIO3,
        .pin_rst = LCD_PIN_RST,
        .pin_te = LCD_PIN_TE,
        .pin_bl = LCD_PIN_BL,
        .width = 360,
        .height = 360,
        .pclk_hz = 40 * 1000 * 1000,
        .quad_mode = true,
        .invert_colors = false,
    };

    cst816_config_t touch_cfg = {
        .i2c_port = I2C_NUM_0,
        .pin_sda = TOUCH_PIN_SDA,
        .pin_scl = TOUCH_PIN_SCL,
        .pin_int = TOUCH_PIN_INT,
        .pin_rst = TOUCH_PIN_RST,
        .i2c_addr = 0x15,
        .max_x = 360,
        .max_y = 360,
        .swap_xy = false,
        .invert_x = false,
        .invert_y = false,
    };

    lvgl_port_init(&disp_cfg, &touch_cfg);
    ui_engine_init();
    while (true) {
        ui_engine_render();
        vTaskDelay(5 / portTICK_PERIOD_MS);
    }
}

static bool time_sync_wait(uint32_t timeout_ms)
{
    static bool s_sntp_started = false;
    if (!s_sntp_started) {
        esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
        cfg.start = true;
        esp_err_t err = esp_netif_sntp_init(&cfg);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            return false;
        }
        s_sntp_started = true;
    }

    const int64_t start = esp_timer_get_time();
    while ((esp_timer_get_time() - start) / 1000 < timeout_ms) {
        if (esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    return false;
}

static void mesh_task(void *arg)
{
    static const uint8_t kMeshId[6] = {0x77, 0x77, 0x77, 0x77, 0x77, 0x77};

    mesh_manager_config_t cfg = {};
    memcpy(cfg.mesh_id, kMeshId, sizeof(kMeshId));
    cfg.router_ssid = MESH_ROUTER_SSID;
    cfg.router_pass = MESH_ROUTER_PASS;
    cfg.mesh_ap_pass = MESH_AP_PASS;
    cfg.channel = 0;
    cfg.max_layer = 15;
    cfg.topology = MESH_TOPO_TREE;
    cfg.ap_authmode = WIFI_AUTH_WPA2_PSK;
    cfg.ap_connections = 6;
    cfg.non_mesh_connections = 1;
    cfg.route_table_size = 300;
    cfg.enable_ps = false;
    cfg.enable_demo_p2p = false;
    cfg.tx_task_stack = 6144;
    cfg.rx_task_stack = 6144;
    cfg.tx_task_prio = 20;
    cfg.rx_task_prio = 20;
    cfg.rx_cb = mesh_ota_rx_cb;

    ESP_ERROR_CHECK(mesh_manager_start(&cfg));

    bool net_ready_notified = false;

    while (true) {
        bool mesh_connected = mesh_manager_is_connected();
        bool is_root = mesh_manager_is_root();
        int layer = mesh_manager_get_layer();

        ui_engine_set_mesh_state(mesh_connected, is_root, layer);
        mesh_ota_notify_mesh_ready(is_root);

        if (mesh_manager_is_connected()) {
            mesh_ota_mark_running_valid();
        }

        if (is_root && mesh_manager_is_router_connected()) {
            if (!net_ready_notified) {
                ui_engine_set_ota_status("TIME SYNC");
                if (time_sync_wait(10000)) {
                    mesh_ota_notify_net_ready();
                    net_ready_notified = true;
                    ui_engine_set_ota_status("OTA CHECK 10S");
                }
            }
        } else if (is_root) {
            ui_engine_set_ota_status("NET WAIT");
        } else if (mesh_connected) {
            ui_engine_set_ota_status("OTA VIA MESH");
        } else {
            ui_engine_set_ota_status("MESH WAIT");
        }

        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}

static void input_init(void)
{
    encoder_config_t enc_cfg = {
        .pin_a = PIN_ENC_A,
        .pin_b = PIN_ENC_B,
        .pin_btn = PIN_ENC_BTN,
        .glitch_filter_us = 10,
    };
    ESP_ERROR_CHECK(encoder_init(&enc_cfg, encoder_event_handler, NULL));
}

void app_main(void)
{
    phase_manager_init();
    input_init();

    mesh_ota_config_t ota_cfg = {};
    ota_cfg.current_version = OTA_CURRENT_VERSION;
    ota_cfg.version_url = OTA_VERSION_URL;
    ota_cfg.firmware_url = OTA_FIRMWARE_URL;
    ota_cfg.device_id = OTA_DEVICE_ID;
    ota_cfg.firebase_boot_ack_base_url = OTA_BOOT_ACK_BASE_URL;
    ota_cfg.firebase_auth_token = OTA_BOOT_ACK_AUTH;
    ota_cfg.expected_sha256 = OTA_EXPECTED_SHA256;
    ota_cfg.chunk_size = 1024;
    ota_cfg.task_stack = 8192;
    ota_cfg.task_prio = 5;
    ESP_ERROR_CHECK(mesh_ota_init(&ota_cfg));
    ESP_ERROR_CHECK(mesh_ota_start());

    xTaskCreatePinnedToCore(mesh_task, "mesh_task", 8192, NULL, 7, NULL, 0);
    xTaskCreatePinnedToCore(ui_task, "ui_task", 6144, NULL, 4, NULL, 1);

    ESP_LOGI(kTag, "knob firmware started");
}
