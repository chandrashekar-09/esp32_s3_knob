/* OTA polling service.
 *
 * Mirrors the Arduino reference loop (`OtaService.cpp::check_ota`):
 *   1. GET APP_OTA_VERSION_URL → integer
 *   2. If > APP_OTA_CURRENT_VERSION → esp_https_ota against
 *      APP_OTA_FIRMWARE_URL → esp_restart()
 *   3. Sleep OTA_POLL_INTERVAL_MS and repeat
 *
 * "Minimal but reliable": uses the project's pre-compiled certificate
 * bundle (CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y) so HTTPS to GitHub +
 * Firebase works without any cert provisioning code. No pinning, no
 * SHA256 expected-hash check, no progress callback — the goal is to
 * ship updates, not enforce supply-chain policy.
 *
 * Threading: task lives on Core 0 at priority 5. UI lives on Core 1
 * at priority 4. WiFi internals also run on Core 0 by default. The OTA
 * task blocks on socket reads during a flash — at ~250 KB firmware
 * over WiFi this takes ~10–20 s — but UI rendering on Core 1 is
 * unaffected.
 */

#include "ota_service.h"

#include <string.h>
#include <stdlib.h>

#include "app_config.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wifi_manager.h"

static const char *kTag = "ota";

/* User asked for once-per-minute polling. */
#define OTA_POLL_INTERVAL_MS    (60U * 1000U)
/* Initial settle delay so WiFi has a chance to connect before the
 * first attempt — first attempt then has the same retry budget as
 * subsequent ones. */
#define OTA_FIRST_DELAY_MS      (15U * 1000U)
/* HTTP timeouts for the lightweight version-check GET. The firmware
 * download uses esp_https_ota's own internal timeout (longer). */
#define OTA_VERSION_TIMEOUT_MS  10000
/* Max body size for version.txt — anything bigger is suspicious. */
#define OTA_VERSION_MAX_BYTES   16

static bool fetch_latest_version(int *out_version)
{
    esp_http_client_config_t cfg = {
        .url               = APP_OTA_VERSION_URL,
        .timeout_ms        = OTA_VERSION_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        /* Cert-bundle handles validation; no explicit cert / no skip. */
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGW(kTag, "version: http init failed");
        return false;
    }

    bool ok = false;
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "version: open failed: %s", esp_err_to_name(err));
        goto done;
    }
    int total = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        ESP_LOGW(kTag, "version: HTTP %d", status);
        goto done;
    }

    char buf[OTA_VERSION_MAX_BYTES + 1] = {0};
    int want = (total > 0 && total < (int)sizeof(buf)) ? total : (int)sizeof(buf) - 1;
    int got = esp_http_client_read(client, buf, want);
    if (got <= 0) {
        ESP_LOGW(kTag, "version: read returned %d", got);
        goto done;
    }
    buf[got] = '\0';
    /* Trim whitespace / CR / LF */
    for (int i = got - 1; i >= 0 && (buf[i] == '\n' || buf[i] == '\r' || buf[i] == ' '); i--) {
        buf[i] = '\0';
    }

    *out_version = atoi(buf);
    ok = true;
    ESP_LOGI(kTag, "version: latest=%d (current=%d)", *out_version, APP_OTA_CURRENT_VERSION);

done:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ok;
}

static bool apply_firmware_update(void)
{
    ESP_LOGI(kTag, "update: downloading from %s", APP_OTA_FIRMWARE_URL);

    esp_http_client_config_t http_cfg = {
        .url               = APP_OTA_FIRMWARE_URL,
        .timeout_ms        = 30000,
        .keep_alive_enable = true,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };

    esp_err_t err = esp_https_ota(&ota_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "update: failed (%s)", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(kTag, "update: success, rebooting");
    return true;
}

static void ota_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(OTA_FIRST_DELAY_MS));

    while (true) {
        if (!wifi_manager_is_connected()) {
            ESP_LOGD(kTag, "skip: no WiFi");
        } else {
            int latest = 0;
            if (fetch_latest_version(&latest)) {
                if (latest > APP_OTA_CURRENT_VERSION) {
                    if (apply_firmware_update()) {
                        vTaskDelay(pdMS_TO_TICKS(1000));
                        esp_restart();  /* no return */
                    }
                } else {
                    ESP_LOGI(kTag, "up to date");
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(OTA_POLL_INTERVAL_MS));
    }
}

esp_err_t ota_service_start(void)
{
    /* 8 KB stack — esp_https_ota + mbedTLS handshake is the big consumer.
     * Core 0 keeps it away from the UI render loop on Core 1. */
    BaseType_t ok = xTaskCreatePinnedToCore(
        ota_task, "ota_task", 8192, NULL, 5, NULL, 0);
    return (ok == pdPASS) ? ESP_OK : ESP_FAIL;
}
