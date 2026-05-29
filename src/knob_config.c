/* knob_config — NVS-backed runtime config for per-knob settings.
 *
 * Bootstrap defaults the first time the namespace is opened:
 *   - online_mode = true   (knob will try WiFi STA + cloud if creds set)
 *   - wifi_ssid   = ""     (knob runs ESP-NOW-only until provisioned)
 *   - wifi_pass   = ""
 *
 * Falls back to the compile-time APP_MESH_ROUTER_SSID / _PASS if the
 * NVS slot is empty AND those defines are non-empty — keeps current
 * lab knobs working through the transition without re-provisioning.
 */

#include "knob_config.h"

#include <string.h>

#include "app_config.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *kTag = "knob_cfg";

#define KNOB_NVS_NAMESPACE  "knob"
#define KNOB_KEY_ONLINE     "online"
#define KNOB_KEY_SSID       "ssid"
#define KNOB_KEY_PASS       "pass"

static knob_config_t s_cfg = {
    .online_mode = true,
    .wifi_ssid   = "",
    .wifi_pass   = "",
};
static bool s_inited = false;

static void load_or_default(void)
{
    /* Defaults are already in s_cfg; NVS reads selectively override. */
    nvs_handle_t h;
    if (nvs_open(KNOB_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(kTag, "no NVS entries — falling back to compile-time defaults");
        /* Compile-time fallback so unprovisioned knobs still work
         * during the migration from hardcoded creds. Remove the
         * fallback once every knob has been provisioned via the
         * SoftAP captive portal. */
        if (sizeof(APP_MESH_ROUTER_SSID) > 1) {
            strncpy(s_cfg.wifi_ssid, APP_MESH_ROUTER_SSID,
                    sizeof(s_cfg.wifi_ssid) - 1);
            strncpy(s_cfg.wifi_pass, APP_MESH_ROUTER_PASS,
                    sizeof(s_cfg.wifi_pass) - 1);
        }
        return;
    }

    uint8_t online_u8 = s_cfg.online_mode ? 1 : 0;
    nvs_get_u8(h, KNOB_KEY_ONLINE, &online_u8);
    s_cfg.online_mode = (online_u8 != 0);

    size_t len;
    len = sizeof(s_cfg.wifi_ssid);
    if (nvs_get_str(h, KNOB_KEY_SSID, s_cfg.wifi_ssid, &len) != ESP_OK ||
        s_cfg.wifi_ssid[0] == '\0') {
        /* Fall back to compile-time if NVS empty. */
        if (sizeof(APP_MESH_ROUTER_SSID) > 1) {
            strncpy(s_cfg.wifi_ssid, APP_MESH_ROUTER_SSID,
                    sizeof(s_cfg.wifi_ssid) - 1);
        }
    }
    len = sizeof(s_cfg.wifi_pass);
    if (nvs_get_str(h, KNOB_KEY_PASS, s_cfg.wifi_pass, &len) != ESP_OK) {
        if (sizeof(APP_MESH_ROUTER_PASS) > 1 && s_cfg.wifi_pass[0] == '\0') {
            strncpy(s_cfg.wifi_pass, APP_MESH_ROUTER_PASS,
                    sizeof(s_cfg.wifi_pass) - 1);
        }
    }
    nvs_close(h);
}

esp_err_t knob_config_init(void)
{
    if (s_inited) return ESP_OK;
    /* nvs_flash_init may already have been called by wifi_manager
     * or mesh_transport — both are idempotent. */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) return err;

    load_or_default();
    s_inited = true;
    ESP_LOGI(kTag, "loaded: online_mode=%s ssid='%s' pass='%s'",
             s_cfg.online_mode ? "ON" : "OFF",
             s_cfg.wifi_ssid,
             s_cfg.wifi_pass[0] ? "(set)" : "(empty)");
    return ESP_OK;
}

const knob_config_t *knob_config_get(void) { return &s_cfg; }

void knob_config_set_online_mode(bool on) { s_cfg.online_mode = on; }

void knob_config_set_wifi(const char *ssid, const char *pass)
{
    if (ssid) {
        strncpy(s_cfg.wifi_ssid, ssid, sizeof(s_cfg.wifi_ssid) - 1);
        s_cfg.wifi_ssid[sizeof(s_cfg.wifi_ssid) - 1] = '\0';
    }
    if (pass) {
        strncpy(s_cfg.wifi_pass, pass, sizeof(s_cfg.wifi_pass) - 1);
        s_cfg.wifi_pass[sizeof(s_cfg.wifi_pass) - 1] = '\0';
    }
}

esp_err_t knob_config_commit(void)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    nvs_handle_t h;
    esp_err_t err = nvs_open(KNOB_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_set_u8(h, KNOB_KEY_ONLINE, s_cfg.online_mode ? 1 : 0);
    nvs_set_str(h, KNOB_KEY_SSID, s_cfg.wifi_ssid);
    nvs_set_str(h, KNOB_KEY_PASS, s_cfg.wifi_pass);
    err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) {
        ESP_LOGI(kTag, "committed: online_mode=%s ssid='%s'",
                 s_cfg.online_mode ? "ON" : "OFF", s_cfg.wifi_ssid);
    }
    return err;
}

bool knob_config_has_wifi(void)
{
    return s_inited && s_cfg.wifi_ssid[0] != '\0';
}
