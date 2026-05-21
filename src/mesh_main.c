#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "mesh_light.h"
#include "mesh_manager.h"

static const char *APP_TAG = "mesh_app";
static const uint8_t kMeshId[6] = {0x77, 0x77, 0x77, 0x77, 0x77, 0x77};

#define MESH_ROUTER_SSID   "IIIT-Guest"
#define MESH_ROUTER_PASS   "f6s68VHJ89mC"
#define MESH_AP_PASS       "mesh_ap_pass"

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
    cfg.enable_demo_p2p = true;
    cfg.tx_task_stack = 6144;
    cfg.rx_task_stack = 6144;
    cfg.tx_task_prio = 5;
    cfg.rx_task_prio = 5;

    ESP_ERROR_CHECK(mesh_manager_start(&cfg));
    ESP_LOGI(APP_TAG, "mesh manager started !");
}
