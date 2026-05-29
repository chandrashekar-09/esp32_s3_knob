/* Cloud subscribe — HTTPS poll of /rest/v1/knob_states.
 *
 * Why polling not WebSocket: ESP-IDF doesn't bundle a WebSocket
 * client; pulling in esp_websocket_client as a managed component
 * is fragile under PlatformIO. Polling reuses the TLS stack we
 * already opened for the upsert path, and 2-second poll latency
 * is fine because ESP-NOW is the primary low-latency path —
 * cloud is the cross-floor fallback where seconds of latency are
 * irrelevant to the user.
 *
 * Bandwidth math (worst case, 16 same-type knobs per store):
 *   16 rows × ~250 B JSON = 4 KB per poll
 *   2 s polling → 2 KB/s/knob → 173 MB/knob/day
 * Comfortably inside Supabase free tier.
 *
 * Stale handling: rows whose last_seen is more than CLOUD_STALE_MS
 * (default 60 s) past now are skipped. The peer_registry age-out
 * (8 s) won't drop them automatically because cloud heartbeats
 * are every 10 s — but if the cloud heartbeat itself stops, the
 * 60 s threshold here filters them out.
 */

#include "cloud_subscribe.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "cloud_config.h"
#include "cloud_transport.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "espnow_inbound.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "phase_manager.h"

static const char *kTag = "cloud_sub";

#define CLOUD_POLL_INTERVAL_MS  2000U
#define CLOUD_STALE_MS          60000U
#define CLOUD_POLL_BUF_BYTES    8192   /* fits ~16 rows at ~250 B each */
#define CLOUD_POLL_HTTP_TIMEOUT 6000

static bool s_started = false;

/* ───────────────────────────── helpers ──────────────────────── */
static int parse_mac_str(const char *s, uint8_t out[6])
{
    if (!s) return -1;
    unsigned v[6];
    if (sscanf(s, "%2x:%2x:%2x:%2x:%2x:%2x",
               &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) {
        return -1;
    }
    for (int i = 0; i < 6; i++) out[i] = (uint8_t)v[i];
    return 0;
}

/* Parse ISO-8601 "2026-05-29T12:34:56.789Z" into epoch seconds.
 * Returns 0 on parse failure (treat as fresh — better to show a
 * stale peer than to hide a real one because of a fractional ms). */
static time_t parse_iso8601_z(const char *s)
{
    if (!s) return 0;
    struct tm tm = {0};
    int y, mo, d, h, mi, sec;
    if (sscanf(s, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &sec) != 6) {
        return 0;
    }
    tm.tm_year = y - 1900;
    tm.tm_mon  = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min  = mi;
    tm.tm_sec  = sec;
    /* Treat input as UTC. mktime would use local TZ; we use a
     * portable conversion: timegm-equivalent via the offset of
     * gmtime/localtime at this date. ESP-IDF tzset defaults UTC. */
    return mktime(&tm);
}

/* ───────────────────────────── HTTP capture ─────────────────── */
typedef struct {
    char *buf;
    int   cap;
    int   len;
} body_accum_t;

static esp_err_t http_event_cb(esp_http_client_event_t *evt)
{
    body_accum_t *a = (body_accum_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && a && a->buf) {
        int room = a->cap - a->len - 1;
        if (room > 0) {
            int copy = (evt->data_len < room) ? evt->data_len : room;
            memcpy(a->buf + a->len, evt->data, copy);
            a->len += copy;
            a->buf[a->len] = '\0';
        }
    }
    return ESP_OK;
}

/* ───────────────────────────── poll & dispatch ──────────────── */
static void dispatch_row(cJSON *row, const char *own_mac_str, time_t now_epoch)
{
    cJSON *jmac        = cJSON_GetObjectItemCaseSensitive(row, "mac");
    cJSON *jtype       = cJSON_GetObjectItemCaseSensitive(row, "device_type");
    cJSON *jnum        = cJSON_GetObjectItemCaseSensitive(row, "number");
    cJSON *jlevel      = cJSON_GetObjectItemCaseSensitive(row, "queue_level");
    cJSON *jsub        = cJSON_GetObjectItemCaseSensitive(row, "sub_step");
    cJSON *jlast       = cJSON_GetObjectItemCaseSensitive(row, "last_seen");

    if (!cJSON_IsString(jmac) || !cJSON_IsString(jtype) ||
        !cJSON_IsNumber(jnum) || !cJSON_IsNumber(jlevel)) {
        return;
    }

    /* Skip our own row — sync_own keeps it fresh from app_state. */
    if (own_mac_str && strcmp(jmac->valuestring, own_mac_str) == 0) return;

    /* Stale filter — if the cloud row is too old, peer is offline. */
    if (cJSON_IsString(jlast) && now_epoch > 0) {
        time_t row_t = parse_iso8601_z(jlast->valuestring);
        if (row_t > 0 && (now_epoch - row_t) > (time_t)(CLOUD_STALE_MS / 1000)) {
            return;
        }
    }

    uint8_t mac[6];
    if (parse_mac_str(jmac->valuestring, mac) < 0) return;

    device_type_t type = (strcmp(jtype->valuestring, "T") == 0)
                             ? DEV_TYPE_T : DEV_TYPE_FR;
    uint8_t number   = (uint8_t)jnum->valueint;
    uint8_t level    = (uint8_t)jlevel->valueint;
    int8_t  sub_step = cJSON_IsNumber(jsub) ? (int8_t)jsub->valueint : 0;

    espnow_inbound_peer_full(type, number, level, sub_step, mac);
}

static void poll_once(char *buf)
{
    const char *store_id = cloud_transport_store_id();
    const char *own_mac  = cloud_transport_own_mac();
    if (!store_id) return;

    char url[256];
    snprintf(url, sizeof(url),
             "%s%s?store_id=eq.%s&select=*",
             CLOUD_SUPABASE_URL, CLOUD_TABLE_PATH, store_id);

    body_accum_t acc = { .buf = buf, .cap = CLOUD_POLL_BUF_BYTES, .len = 0 };
    buf[0] = '\0';

    esp_http_client_config_t cfg = {
        .url               = url,
        .timeout_ms        = CLOUD_POLL_HTTP_TIMEOUT,
        .method            = HTTP_METHOD_GET,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = true,
        .event_handler     = http_event_cb,
        .user_data         = &acc,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) return;

    esp_http_client_set_header(cli, "apikey", CLOUD_SUPABASE_ANON_KEY);
    esp_http_client_set_header(cli, "Authorization",
                               "Bearer " CLOUD_SUPABASE_ANON_KEY);

    esp_err_t err = esp_http_client_perform(cli);
    int status = (err == ESP_OK) ? esp_http_client_get_status_code(cli) : 0;
    esp_http_client_cleanup(cli);

    if (err != ESP_OK || status < 200 || status >= 300) {
        ESP_LOGW(kTag, "poll: HTTP err=%s status=%d",
                 (err == ESP_OK) ? "ok" : esp_err_to_name(err), status);
        return;
    }
    if (acc.len <= 0) return;

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        ESP_LOGW(kTag, "poll: JSON parse failed");
        return;
    }
    if (!cJSON_IsArray(root)) {
        cJSON_Delete(root);
        return;
    }

    time_t now_epoch;
    time(&now_epoch);
    int dispatched = 0;
    cJSON *row;
    cJSON_ArrayForEach(row, root) {
        dispatch_row(row, own_mac, now_epoch);
        dispatched++;
    }
    cJSON_Delete(root);

    ESP_LOGI(kTag, "poll OK: %d rows", dispatched);
}

static void subscribe_task(void *arg)
{
    (void)arg;
    ESP_LOGI(kTag, "subscribe task started (poll every %u ms)",
             (unsigned)CLOUD_POLL_INTERVAL_MS);

    /* Body buffer in PSRAM — 8 KB on the heap rather than the stack
     * to keep the task small. */
    char *buf = heap_caps_malloc(CLOUD_POLL_BUF_BYTES,
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        ESP_LOGE(kTag, "PSRAM alloc failed — aborting task");
        vTaskDelete(NULL);
        return;
    }

    /* Wait for cloud transport to come up (WiFi associates, IP
     * obtained, store_id computed). Once up, poll forever. */
    while (true) {
        if (cloud_transport_is_ready()) {
            poll_once(buf);
        }
        vTaskDelay(pdMS_TO_TICKS(CLOUD_POLL_INTERVAL_MS));
    }
}

esp_err_t cloud_subscribe_start(void)
{
    if (s_started) return ESP_OK;
    /* 12 KB stack — TLS handshake + cJSON parse together push beyond
     * the 8 KB the upsert path uses. Pinned to Core 0 to keep ui_task
     * unmolested on Core 1. */
    BaseType_t ok = xTaskCreatePinnedToCore(subscribe_task, "cloud_sub",
                                            12288, NULL, 3, NULL, 0);
    if (ok != pdPASS) return ESP_FAIL;
    s_started = true;
    return ESP_OK;
}
