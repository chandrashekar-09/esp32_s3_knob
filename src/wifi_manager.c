/* Minimal WiFi STA manager.
 *
 * Mirrors the spirit of the Arduino reference (WiFi.begin → wait →
 * use), but uses ESP-IDF's event-loop wiring so reconnects are handled
 * passively without a babysitter task. The OTA task polls
 * wifi_manager_is_connected() before issuing HTTPS, no notification
 * channel needed.
 *
 * Intentionally minimal — no provisioning, no NVS persistence beyond
 * what esp_wifi does internally, no power-save toggling. The SSID/PASS
 * come from app_config.h compile-time constants.
 */

#include "wifi_manager.h"

#include <string.h>

#include "app_config.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

static const char *kTag = "wifi";

static volatile bool s_connected = false;
static bool s_initialised = false;

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)data;
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            ESP_LOGI(kTag, "STA start — connecting to '%s'", APP_MESH_ROUTER_SSID);
            esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            s_connected = false;
            ESP_LOGW(kTag, "disconnected — retrying");
            esp_wifi_connect();
            break;
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
        s_connected = true;
        ESP_LOGI(kTag, "got IP " IPSTR, IP2STR(&evt->ip_info.ip));
    }
}

esp_err_t wifi_manager_init(void)
{
    if (s_initialised) return ESP_OK;

    /* NVS — esp_wifi stores PHY calibration here. Erase + retry if
     * the partition is corrupt or the layout version changed. */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, NULL, NULL));

    wifi_config_t wifi_cfg = {0};
    strncpy((char *)wifi_cfg.sta.ssid, APP_MESH_ROUTER_SSID,
            sizeof(wifi_cfg.sta.ssid) - 1);
    strncpy((char *)wifi_cfg.sta.password, APP_MESH_ROUTER_PASS,
            sizeof(wifi_cfg.sta.password) - 1);
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;  /* accept any auth */

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    s_initialised = true;
    return ESP_OK;
}

bool wifi_manager_is_connected(void)
{
    return s_connected;
}
