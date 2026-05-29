/* Cloud transport for the QueSort online-fallback path.
 *
 * One job in this stage: POST UPSERT a knob's state to Supabase
 * via PostgREST. The Realtime SUBSCRIBE path lives in a separate
 * module added in a follow-up — keeps the WebSocket lifecycle
 * isolated from the simple synchronous POST path here.
 *
 * Wire format (matches Lovable's knob_states schema):
 *   {
 *     "store_id":   "a3f2c8d1",
 *     "mac":        "ac:a7:04:ef:72:5c",
 *     "device_type":"FR",
 *     "number":     2,
 *     "queue_level":4,
 *     "sub_step":   1,
 *     "firmware_version": "v6",
 *     "last_seen":  "2026-05-29T12:34:56Z"
 *   }
 *
 * URL: POST /rest/v1/knob_states
 * Headers: apikey, Authorization: Bearer, Prefer: resolution=merge-duplicates,
 *          Content-Type: application/json
 *
 * store_id derivation: SHA-256(APP_MESH_ROUTER_SSID) → first 8 hex
 * characters. Auto-segregates fleets across stores without operator
 * input. An admin override slot exists in NVS (key "store_id") for
 * the rare case two stores share an SSID.
 */

#include "cloud_transport.h"

#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "app_config.h"
#include "cloud_config.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mbedtls/sha256.h"
#include "nvs.h"

static const char *kTag = "cloud_tx";

#define CLOUD_NVS_NAMESPACE  "cloud"
#define CLOUD_NVS_KEY_STORE  "store_id"
#define STORE_ID_LEN         9   /* 8 hex chars + NUL */
#define MAC_STR_LEN          18  /* "aa:bb:cc:dd:ee:ff" + NUL */
#define HTTP_TIMEOUT_MS      8000
#define FW_VERSION_STR       "v6"

static bool             s_inited       = false;
static char             s_store_id[STORE_ID_LEN] = {0};
static char             s_own_mac_str[MAC_STR_LEN] = {0};
static SemaphoreHandle_t s_lock        = NULL;

/* ───────────────────────────── store_id ─────────────────────── */
static void derive_store_id_from_ssid(char out[STORE_ID_LEN])
{
    /* SHA-256(SSID) first 4 bytes → 8 hex chars. */
    unsigned char digest[32];
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, (const unsigned char *)APP_MESH_ROUTER_SSID,
                          strlen(APP_MESH_ROUTER_SSID));
    mbedtls_sha256_finish(&ctx, digest);
    mbedtls_sha256_free(&ctx);
    snprintf(out, STORE_ID_LEN, "%02x%02x%02x%02x",
             digest[0], digest[1], digest[2], digest[3]);
}

static void load_store_id(void)
{
    /* NVS override beats SSID hash. */
    nvs_handle_t h;
    if (nvs_open(CLOUD_NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(s_store_id);
        if (nvs_get_str(h, CLOUD_NVS_KEY_STORE, s_store_id, &len) == ESP_OK &&
            s_store_id[0] != '\0') {
            nvs_close(h);
            ESP_LOGI(kTag, "store_id from NVS override: %s", s_store_id);
            return;
        }
        nvs_close(h);
    }
    derive_store_id_from_ssid(s_store_id);
    ESP_LOGI(kTag, "store_id from SSID hash: %s (ssid='%s')",
             s_store_id, APP_MESH_ROUTER_SSID);
}

/* ───────────────────────────── helpers ──────────────────────── */
static void format_mac(char out[MAC_STR_LEN])
{
    uint8_t mac[6] = {0};
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    snprintf(out, MAC_STR_LEN, "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/* ISO-8601 UTC timestamp string. Falls back to "1970-01-01T00:00:00Z"
 * if the system clock isn't set (no NTP yet). Supabase will still
 * accept it; the server-side DEFAULT now() in knob_states would have
 * been simpler but we want to send last_seen explicitly so dashboards
 * see the knob's notion of time. */
static void format_iso_now(char *buf, size_t buflen)
{
    time_t now;
    time(&now);
    struct tm tm_utc;
    gmtime_r(&now, &tm_utc);
    strftime(buf, buflen, "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
}

static bool wifi_is_connected(void)
{
    wifi_ap_record_t info;
    return esp_wifi_sta_get_ap_info(&info) == ESP_OK;
}

/* ───────────────────────────── public API ────────────────────── */
esp_err_t cloud_transport_init(void)
{
    if (s_inited) return ESP_OK;
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    load_store_id();
    format_mac(s_own_mac_str);
    s_inited = true;
    ESP_LOGI(kTag, "ready: url=%s mac=%s store=%s",
             CLOUD_SUPABASE_URL, s_own_mac_str, s_store_id);
    return ESP_OK;
}

bool cloud_transport_is_ready(void)
{
    return s_inited && wifi_is_connected();
}

const char *cloud_transport_store_id(void)
{
    return s_inited ? s_store_id : NULL;
}

const char *cloud_transport_own_mac(void)
{
    return s_inited ? s_own_mac_str : NULL;
}

esp_err_t cloud_transport_upsert(const app_state_t *st)
{
    if (!s_inited || !st) return ESP_ERR_INVALID_STATE;
    if (!wifi_is_connected()) {
        ESP_LOGD(kTag, "skip upsert: WiFi not connected");
        return ESP_ERR_INVALID_STATE;
    }

    /* Serialise concurrent callers — esp_http_client + TLS handshake
     * isn't thread-safe across multiple in-flight requests on the
     * same handle. */
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(HTTP_TIMEOUT_MS + 2000)) != pdTRUE) {
        ESP_LOGW(kTag, "upsert: lock timeout");
        return ESP_FAIL;
    }

    char body[256];
    char ts[32];
    format_iso_now(ts, sizeof(ts));
    int n = snprintf(body, sizeof(body),
        "{\"store_id\":\"%s\","
        "\"mac\":\"%s\","
        "\"device_type\":\"%s\","
        "\"number\":%u,"
        "\"queue_level\":%u,"
        "\"sub_step\":%d,"
        "\"firmware_version\":\"%s\","
        "\"last_seen\":\"%s\"}",
        s_store_id, s_own_mac_str,
        st->device_type == DEV_TYPE_FR ? "FR" : "T",
        (unsigned)st->device_number,
        (unsigned)st->queue_level,
        (int)st->queue_sub_step,
        FW_VERSION_STR,
        ts);
    if (n < 0 || n >= (int)sizeof(body)) {
        xSemaphoreGive(s_lock);
        ESP_LOGE(kTag, "upsert: body truncated");
        return ESP_FAIL;
    }

    char url[160];
    snprintf(url, sizeof(url), "%s%s", CLOUD_SUPABASE_URL, CLOUD_TABLE_PATH);

    esp_http_client_config_t cfg = {
        .url               = url,
        .timeout_ms        = HTTP_TIMEOUT_MS,
        .method            = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = true,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) {
        xSemaphoreGive(s_lock);
        return ESP_FAIL;
    }

    esp_http_client_set_header(cli, "apikey", CLOUD_SUPABASE_ANON_KEY);
    esp_http_client_set_header(cli, "Authorization",
                               "Bearer " CLOUD_SUPABASE_ANON_KEY);
    esp_http_client_set_header(cli, "Content-Type", "application/json");
    esp_http_client_set_header(cli, "Prefer", "resolution=merge-duplicates");
    esp_http_client_set_post_field(cli, body, n);

    esp_err_t err = esp_http_client_perform(cli);
    int status = (err == ESP_OK) ? esp_http_client_get_status_code(cli) : 0;
    esp_http_client_cleanup(cli);
    xSemaphoreGive(s_lock);

    if (err != ESP_OK) {
        ESP_LOGW(kTag, "upsert: HTTP err %s", esp_err_to_name(err));
        return ESP_FAIL;
    }
    if (status < 200 || status >= 300) {
        ESP_LOGW(kTag, "upsert: HTTP %d", status);
        return ESP_FAIL;
    }
    ESP_LOGI(kTag, "upsert OK: %s level=%u sub=%d",
             s_own_mac_str, (unsigned)st->queue_level,
             (int)st->queue_sub_step);
    return ESP_OK;
}
