#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "mesh_light.h"
#include "mesh_manager.h"
#include "mesh_ota.h"

static const char *APP_TAG = "mesh_app";
static const uint8_t kMeshId[6] = {0x77, 0x77, 0x77, 0x77, 0x77, 0x77};

#define MESH_ROUTER_SSID   "IIIT-Guest"
#define MESH_ROUTER_PASS   "f6s68VHJ89mC"
#define MESH_AP_PASS       "mesh_ap_pass"
#define OTA_CURRENT_VERSION 6
#define OTA_VERSION_URL "https://raw.githubusercontent.com/chandrashekar-09/dosamatic/main/var.txt"
#define OTA_FIRMWARE_URL "https://raw.githubusercontent.com/chandrashekar-09/dosamatic/main/firmware.bin"
#define OTA_DEVICE_ID "tes-001"
#define OTA_BOOT_ACK_BASE_URL "https://techlora-369-default-rtdb.asia-southeast1.firebasedatabase.app/boot_ack"
#define OTA_BOOT_ACK_AUTH ""

void app_main(void)
{
    ESP_ERROR_CHECK(mesh_light_init());

    mesh_manager_config_t cfg = {};
    memcpy(cfg.mesh_id, kMeshId, sizeof(kMeshId));
    cfg.router_ssid = MESH_ROUTER_SSID;
    cfg.router_pass = MESH_ROUTER_PASS;
    cfg.mesh_ap_pass = MESH_AP_PASS;
    cfg.channel = 0; /* auto */
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
    cfg.tx_task_prio = 5;
    cfg.rx_task_prio = 5;
    cfg.rx_cb = mesh_ota_rx_cb;
    cfg.rx_cb_ctx = NULL;

    ESP_ERROR_CHECK(mesh_manager_start(&cfg));

    mesh_ota_config_t ota_cfg = {};
    ota_cfg.current_version = OTA_CURRENT_VERSION;
    ota_cfg.version_url = OTA_VERSION_URL;
    ota_cfg.firmware_url = OTA_FIRMWARE_URL;
    ota_cfg.device_id = OTA_DEVICE_ID;
    ota_cfg.firebase_boot_ack_base_url = OTA_BOOT_ACK_BASE_URL;
    ota_cfg.firebase_auth_token = OTA_BOOT_ACK_AUTH;
    ota_cfg.chunk_size = 1024;
    ota_cfg.task_stack = 8192;
    ota_cfg.task_prio = 5;

    ESP_ERROR_CHECK(mesh_ota_init(&ota_cfg));
    ESP_ERROR_CHECK(mesh_ota_start());
    ESP_LOGI(APP_TAG, "mesh manager started !");
}
