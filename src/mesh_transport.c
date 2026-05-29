/* QueSort ESP-NOW mesh transport.
 *
 * Owns WiFi (STA, no-AP) + ESP-NOW broadcast. Replaces
 * wifi_manager + ota_service when FEATURE_ZERO_CONFIG_MESH is on
 * — production knobs talk only to peers, not the internet. The
 * wifi+OTA path is still selectable via the build flag for lab use.
 *
 * Architecture:
 *   - WiFi STA brought up with no AP association (just MAC + radio
 *     enabled). LR PHY for ~6-10 dB extra link budget; max TX power.
 *   - Channel pinned via esp_wifi_set_channel() after start. Channel
 *     comes from NVS if set, else MESH_DEFAULT_CHANNEL (=6 — quieter
 *     than the crowded default 1 in retail spaces).
 *   - ESP-NOW broadcast peer added (FF:FF:FF:FF:FF:FF). All frames
 *     sent to that peer; all peers receive. No per-peer registration
 *     needed (we'd run out of the 20-peer cap with a 32-knob fleet).
 *   - RX callback pushes raw frames to a queue. A dedicated task
 *     drains the queue: validates magic+version, dedups via
 *     (origin_mac, seq), dispatches by type into espnow_inbound.
 *   - TX is fire-and-forget; ESP-NOW's send callback updates stats.
 *
 * Dedup cache: ring buffer of (mac, seq) tuples, last 32 entries.
 * Cheap and sufficient — broadcast cadence is ~1 Hz per knob,
 * 32 entries covers ~30 s of cross-fleet traffic comfortably.
 */

#include "mesh_transport.h"

#include <string.h>

#include "app_config.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "espnow_inbound.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "knob_config.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "peer_registry.h"

static const char *kTag = "mesh_tx";

#define MESH_DEFAULT_CHANNEL    6        /* quieter than the crowded 1 */
#define MESH_BROADCAST_INTERVAL_MS  1000 /* ~1 Hz per-knob state push */
#define MESH_RX_QUEUE_DEPTH     16
#define MESH_DEDUP_CACHE_SIZE   32
#define MESH_NVS_NAMESPACE      "mesh"
#define MESH_NVS_KEY_CHANNEL    "ch"

/* ───────────────────────────── wire format ──────────────────────── */
typedef struct __attribute__((packed)) {
    uint32_t magic;         /* MESH_PROTO_MAGIC */
    uint8_t  version;       /* MESH_PROTO_VERSION */
    uint8_t  type;          /* mesh_msg_type_t */
    uint8_t  origin_mac[6];
    uint16_t seq;
    uint8_t  ttl;
} mesh_frame_hdr_t;

typedef struct __attribute__((packed)) {
    uint8_t device_type;    /* device_type_t */
    uint8_t number;         /* 1..16 */
    uint8_t queue_level;    /* 1..5 */
    int8_t  sub_step;       /* -(STEP-1)..+(STEP-1) */
} mesh_peer_state_t;

typedef struct __attribute__((packed)) {
    uint8_t device_type;
    uint8_t want_slot;
    uint8_t claim_phase;    /* 0 = listening, 1 = claiming, 2 = committed */
} mesh_slot_claim_t;

/* ───────────────────────────── module state ─────────────────────── */
static bool s_inited = false;
static uint8_t s_channel = MESH_DEFAULT_CHANNEL;
static uint8_t s_own_mac[6];
static uint16_t s_seq = 0;
static QueueHandle_t s_rx_queue = NULL;
static mesh_transport_stats_t s_stats = {0};

/* Dedup ring buffer — last N (mac, seq) pairs seen. Linear scan;
 * 32 entries is too small to bother indexing. */
typedef struct { uint8_t mac[6]; uint16_t seq; bool valid; } dedup_entry_t;
static dedup_entry_t s_dedup[MESH_DEDUP_CACHE_SIZE];
static uint8_t       s_dedup_cursor = 0;

/* Queued RX frame — bounded copy so the ESP-NOW callback returns fast. */
#define MESH_MAX_FRAME 64
typedef struct {
    uint8_t data[MESH_MAX_FRAME];
    uint8_t len;
} rx_item_t;

/* ───────────────────────────── helpers ──────────────────────────── */
static bool dedup_check_and_record(const uint8_t mac[6], uint16_t seq)
{
    for (int i = 0; i < MESH_DEDUP_CACHE_SIZE; i++) {
        if (!s_dedup[i].valid) continue;
        if (s_dedup[i].seq == seq &&
            memcmp(s_dedup[i].mac, mac, 6) == 0) {
            return true;  /* duplicate */
        }
    }
    /* Not a dup — record at cursor (LRU-ish ring). */
    memcpy(s_dedup[s_dedup_cursor].mac, mac, 6);
    s_dedup[s_dedup_cursor].seq   = seq;
    s_dedup[s_dedup_cursor].valid = true;
    s_dedup_cursor = (uint8_t)((s_dedup_cursor + 1) % MESH_DEDUP_CACHE_SIZE);
    return false;
}

static esp_err_t nvs_read_channel(uint8_t *out)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(MESH_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    err = nvs_get_u8(h, MESH_NVS_KEY_CHANNEL, out);
    nvs_close(h);
    return err;
}

/* ───────────────────────────── WiFi mesh-mode init ──────────────── */
#if FEATURE_CLOUD_FALLBACK
/* Auto-reconnect with EXPONENTIAL BACKOFF. Without backoff, a knob
 * that boots out of range of its AP retries esp_wifi_connect() in
 * a tight loop — every retry triggers a fresh scan that locks the
 * radio for ~100 ms, starving ESP-NOW + the UI render loop. Result:
 * IDLE-task watchdog firing every few seconds.
 *
 * Backoff: 1s → 2s → 4s → 8s → 16s → 30s (cap). Resets to 1s on
 * each successful association (GOT_IP). */
#define RECONNECT_INITIAL_MS  1000U
#define RECONNECT_CAP_MS      30000U
static esp_timer_handle_t s_reconnect_timer = NULL;
static uint32_t           s_reconnect_delay_ms = RECONNECT_INITIAL_MS;

static void reconnect_timer_cb(void *arg)
{
    (void)arg;
    ESP_LOGI(kTag, "WiFi reconnect attempt (backoff %u ms)",
             (unsigned)s_reconnect_delay_ms);
    esp_wifi_connect();
}

static void schedule_reconnect(void)
{
    if (!s_reconnect_timer) return;
    esp_timer_stop(s_reconnect_timer);
    esp_timer_start_once(s_reconnect_timer,
                         (uint64_t)s_reconnect_delay_ms * 1000ULL);
    /* Double the delay for next time, capped. */
    uint32_t next = s_reconnect_delay_ms * 2;
    if (next > RECONNECT_CAP_MS) next = RECONNECT_CAP_MS;
    s_reconnect_delay_ms = next;
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id,
                          void *data)
{
    (void)arg; (void)data;
    if (base != WIFI_EVENT) return;
    switch (id) {
    case WIFI_EVENT_STA_START:
        ESP_LOGI(kTag, "WiFi STA start — connecting to '%s'",
                 APP_MESH_ROUTER_SSID);
        esp_wifi_connect();
        break;
    case WIFI_EVENT_STA_DISCONNECTED:
        ESP_LOGW(kTag, "WiFi STA disconnected — backoff retry in %u ms",
                 (unsigned)s_reconnect_delay_ms);
        schedule_reconnect();
        break;
    default:
        break;
    }
}
static void sntp_start_once(void)
{
    static bool s_sntp_started = false;
    if (s_sntp_started) return;
    s_sntp_started = true;
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.google.com");
    esp_sntp_init();
    ESP_LOGI(kTag, "SNTP started — last_seen timestamps will be real once synced");
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;
    if (id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
        ESP_LOGI(kTag, "WiFi STA got IP " IPSTR " — cloud transport unblocks",
                 IP2STR(&evt->ip_info.ip));
        s_reconnect_delay_ms = RECONNECT_INITIAL_MS;
        /* Kick SNTP now that we have a route to the internet. Without
         * a real clock, last_seen sent to Supabase reads as 1970-01-01
         * (epoch 0) and the dashboard renders it as "56 years ago". */
        sntp_start_once();
    }
}
#endif

static esp_err_t wifi_mesh_start(void)
{
    /* NVS — required by esp_wifi. wifi_manager normally does this
     * but in FEATURE_ZERO_CONFIG_MESH builds wifi_manager isn't
     * called, so we init NVS here ourselves. Idempotent. */
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
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

#if FEATURE_CLOUD_FALLBACK
    /* Register event handlers + create reconnect-backoff timer
     * BEFORE esp_wifi_start so the first WIFI_EVENT_STA_START
     * actually finds our handler. Skipped entirely when the user
     * has set online_mode=OFF — in that case we don't even
     * register the events, no STA association attempted. */
    const knob_config_t *kc = knob_config_get();
    bool want_sta = kc->online_mode && knob_config_has_wifi();
    if (want_sta) {
        ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                   on_wifi_event, NULL));
        ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                   on_ip_event, NULL));
        esp_timer_create_args_t targs = {
            .callback        = reconnect_timer_cb,
            .name            = "wifi_reconnect",
            .dispatch_method = ESP_TIMER_TASK,
        };
        ESP_ERROR_CHECK(esp_timer_create(&targs, &s_reconnect_timer));
    } else {
        ESP_LOGI(kTag, "WiFi STA disabled (online_mode=%s, ssid=%s)",
                 kc->online_mode ? "ON" : "OFF",
                 kc->wifi_ssid[0] ? "set" : "empty");
    }
#endif

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

#if FEATURE_CLOUD_FALLBACK
    if (want_sta) {
        wifi_config_t sta_cfg = {0};
        strncpy((char *)sta_cfg.sta.ssid, kc->wifi_ssid,
                sizeof(sta_cfg.sta.ssid) - 1);
        strncpy((char *)sta_cfg.sta.password, kc->wifi_pass,
                sizeof(sta_cfg.sta.password) - 1);
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    }
#endif

    ESP_ERROR_CHECK(esp_wifi_start());

    /* Standard 802.11b/g/n PHY (NOT LR). LR's 250 kbps theoretical
     * 6-10 dB link-budget advantage doesn't survive dense obstacles
     * — concrete walls + metal racks in retail murder LR's narrow
     * spectral mask. Standard 802.11b at 1 Mbps gives the best
     * obstacle penetration in practice because:
     *   - DSSS spreading at 11 chips/bit = effective 10.4 dB
     *     processing gain over noise floor
     *   - CCK is well-understood by every WiFi chip
     *   - 1 Mbps is the most-robust rate the radio supports
     *
     * Enable all of b/g/n so the chip picks whichever is best for
     * each frame — but our broadcast peer is pinned to 1 Mbps via
     * esp_now_set_peer_rate_config below for maximum reach. */
    err = esp_wifi_set_protocol(WIFI_IF_STA,
                                WIFI_PROTOCOL_11B |
                                WIFI_PROTOCOL_11G |
                                WIFI_PROTOCOL_11N);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "PHY set failed (%s) — using driver default",
                 esp_err_to_name(err));
    } else {
        ESP_LOGI(kTag, "PHY: 802.11b/g/n (broadcast pinned to 1 Mbps CCK)");
    }

    /* Max TX power. ESP-IDF unit is quarter-dBm: 84 = 21 dBm peak.
     * Most regulatory domains allow ≥20 dBm in the 2.4 GHz ISM
     * band; the chip silently clamps if the region requires lower.
     * Bumping from 80 → 84 squeezes the extra 1 dB allowed in EU
     * conducted-power rules where the antenna gain is negligible. */
    ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(84));

#if FEATURE_CLOUD_FALLBACK
    if (want_sta) {
        ESP_LOGI(kTag, "WiFi STA → connecting to '%s' (channel will follow AP)",
                 kc->wifi_ssid);
    } else {
        /* online_mode OFF or no creds → pin to our static mesh
         * channel like the FEATURE_CLOUD_FALLBACK=0 path. */
        ESP_ERROR_CHECK(esp_wifi_set_channel(s_channel, WIFI_SECOND_CHAN_NONE));
        ESP_LOGI(kTag, "WiFi STA idle (offline mesh only) — channel pinned to %u",
                 s_channel);
    }
#else
    /* Build-time disabled — pin to mesh channel always. */
    ESP_ERROR_CHECK(esp_wifi_set_channel(s_channel, WIFI_SECOND_CHAN_NONE));
    ESP_LOGI(kTag, "WiFi STA up: channel=%u, no AP association", s_channel);
#endif
    return ESP_OK;
}

/* ───────────────────────────── ESP-NOW callbacks ────────────────── */
static void on_send_cb(const wifi_tx_info_t *info, esp_now_send_status_t status)
{
    (void)info;
    if (status == ESP_NOW_SEND_SUCCESS) s_stats.tx_ok++;
    else                                s_stats.tx_fail++;
}

static void on_recv_cb(const esp_now_recv_info_t *info,
                       const uint8_t *data, int len)
{
    if (!info || !data || len <= 0 || len > MESH_MAX_FRAME) {
        s_stats.rx_bad++;
        return;
    }
    rx_item_t item;
    item.len = (uint8_t)len;
    memcpy(item.data, data, len);
    /* Non-blocking: drop if queue full so we don't stall the RX
     * thread. Dropped frames retransmit next 1 s cycle. */
    BaseType_t ok = xQueueSendFromISR(s_rx_queue, &item, NULL);
    if (ok != pdTRUE) s_stats.rx_bad++;
}

/* ───────────────────────────── RX task ──────────────────────────── */
static void dispatch_peer_state(const mesh_frame_hdr_t *hdr,
                                const mesh_peer_state_t *p)
{
    /* Sanity range. */
    if (p->device_type > DEV_TYPE_T) return;
    if (p->number < 1 || p->number > 16) return;
    if (p->queue_level < 1 || p->queue_level > 5) return;
    if (p->sub_step < -2 || p->sub_step > 2) return;

    /* Ignore frames whose origin MAC equals our own — own state is
     * synced from app_state every render, no need to reflect mesh
     * echoes back to ourselves. */
    if (memcmp(hdr->origin_mac, s_own_mac, 6) == 0) return;

    espnow_inbound_peer_full((device_type_t)p->device_type,
                             p->number, p->queue_level, p->sub_step,
                             hdr->origin_mac);
}

static void rx_task(void *arg)
{
    (void)arg;
    rx_item_t item;
    while (xQueueReceive(s_rx_queue, &item, portMAX_DELAY) == pdTRUE) {
        s_stats.rx_total++;
        if (item.len < (int)sizeof(mesh_frame_hdr_t)) { s_stats.rx_bad++; continue; }
        const mesh_frame_hdr_t *hdr = (const mesh_frame_hdr_t *)item.data;
        if (hdr->magic != MESH_PROTO_MAGIC)   { s_stats.rx_bad++; continue; }
        if (hdr->version != MESH_PROTO_VERSION) { s_stats.rx_bad++; continue; }

        if (dedup_check_and_record(hdr->origin_mac, hdr->seq)) {
            s_stats.rx_dup++;
            continue;
        }

        const uint8_t *payload = item.data + sizeof(mesh_frame_hdr_t);
        int            paylen  = item.len - (int)sizeof(mesh_frame_hdr_t);

        /* MULTI-HOP RELAY — if the frame still has TTL to spare AND
         * it didn't originate from us, rebroadcast it. The (origin,
         * seq) dedup cache on every other knob silently drops the
         * relayed copy if they already heard the original directly,
         * so this only costs airtime when needed. Floor-to-floor
         * reach: floor-1 → floor-2 (direct) → floor-3 (relayed). */
        if (hdr->ttl > 1 &&
            memcmp(hdr->origin_mac, s_own_mac, 6) != 0) {
            uint8_t relay[MESH_MAX_FRAME];
            memcpy(relay, item.data, item.len);
            ((mesh_frame_hdr_t *)relay)->ttl = (uint8_t)(hdr->ttl - 1);
            uint8_t bcast_addr[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
            esp_now_send(bcast_addr, relay, item.len);
        }

        switch (hdr->type) {
        case MESH_MSG_PEER_STATE:
            if (paylen >= (int)sizeof(mesh_peer_state_t)) {
                dispatch_peer_state(hdr, (const mesh_peer_state_t *)payload);
            }
            break;
        case MESH_MSG_SLOT_CLAIM:
            /* Slot-claim handling is wired up in slot_claim.c — for
             * now it observes via the registry. Future: dedicated
             * dispatch table. */
            break;
        default:
            break;
        }
    }
}

/* ───────────────────────────── public API ───────────────────────── */
esp_err_t mesh_transport_init(void)
{
    if (s_inited) return ESP_OK;

    /* Channel: NVS override beats default. */
    uint8_t persisted = 0;
    if (nvs_read_channel(&persisted) == ESP_OK &&
        persisted >= 1 && persisted <= 13) {
        s_channel = persisted;
        ESP_LOGI(kTag, "channel from NVS: %u", s_channel);
    } else {
        s_channel = MESH_DEFAULT_CHANNEL;
        ESP_LOGI(kTag, "channel = default %u (no NVS override)", s_channel);
    }

    s_rx_queue = xQueueCreate(MESH_RX_QUEUE_DEPTH, sizeof(rx_item_t));
    if (!s_rx_queue) return ESP_ERR_NO_MEM;
    memset(s_dedup, 0, sizeof(s_dedup));

    esp_err_t err = wifi_mesh_start();
    if (err != ESP_OK) return err;

    /* Grab our STA MAC for origin-tagging and self-echo suppression.
     * Also publish to peer_registry so sync_own + compute_rank can
     * use it. */
    ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_STA, s_own_mac));
    peer_registry_set_own_mac(s_own_mac);
    ESP_LOGI(kTag, "STA MAC %02x:%02x:%02x:%02x:%02x:%02x",
             s_own_mac[0], s_own_mac[1], s_own_mac[2],
             s_own_mac[3], s_own_mac[4], s_own_mac[5]);

    /* ESP-NOW. */
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_send_cb(on_send_cb));
    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_recv_cb));

    /* Broadcast peer — receives every frame we send. */
    esp_now_peer_info_t bcast = {0};
    memset(bcast.peer_addr, 0xFF, 6);
    bcast.ifidx   = WIFI_IF_STA;
    bcast.channel = 0;   /* 0 = use current */
    bcast.encrypt = false;
    if (!esp_now_is_peer_exist(bcast.peer_addr)) {
        ESP_ERROR_CHECK(esp_now_add_peer(&bcast));
    }

    /* Pin broadcast peer to 1 Mbps 802.11b CCK for max obstacle
     * penetration. Slowest standard-PHY rate the chip supports;
     * each frame takes ~1 ms (our payload is < 64 bytes). The
     * tradeoff vs higher rates is irrelevant at 1 Hz cadence. */
    {
        esp_now_rate_config_t rate_cfg = {
            .phymode  = WIFI_PHY_MODE_11B,
            .rate     = WIFI_PHY_RATE_1M_L,   /* 1 Mbps long-preamble CCK */
            .ersu     = false,
            .dcm      = false,
        };
        esp_err_t rerr = esp_now_set_peer_rate_config(bcast.peer_addr, &rate_cfg);
        if (rerr != ESP_OK) {
            ESP_LOGW(kTag, "broadcast 1Mbps rate set failed (%s) — using default",
                     esp_err_to_name(rerr));
        } else {
            ESP_LOGI(kTag, "broadcast peer pinned to 1 Mbps CCK for wall penetration");
        }
    }

    /* RX task — small stack, priority just below UI. */
    BaseType_t ok = xTaskCreatePinnedToCore(rx_task, "mesh_rx", 4096,
                                            NULL, 4, NULL, 0);
    if (ok != pdPASS) {
        ESP_LOGE(kTag, "rx task create failed");
        return ESP_FAIL;
    }

    s_inited = true;
    ESP_LOGI(kTag, "mesh transport ready");
    return ESP_OK;
}

uint8_t mesh_transport_get_channel(void) { return s_channel; }

esp_err_t mesh_transport_set_channel_nvs(uint8_t ch)
{
    if (ch < 1 || ch > 13) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open(MESH_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, MESH_NVS_KEY_CHANNEL, ch);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) {
        ESP_LOGI(kTag, "channel %u persisted to NVS (effective next boot)", ch);
    }
    return err;
}

static esp_err_t send_raw(const uint8_t *buf, size_t len)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    return esp_now_send(bcast, buf, len);
}

esp_err_t mesh_transport_broadcast_state(const app_state_t *st)
{
    if (!st) return ESP_ERR_INVALID_ARG;
    if (st->device_number < 1 || st->device_number > 16) return ESP_ERR_INVALID_ARG;

    uint8_t buf[sizeof(mesh_frame_hdr_t) + sizeof(mesh_peer_state_t)];
    mesh_frame_hdr_t *hdr = (mesh_frame_hdr_t *)buf;
    hdr->magic   = MESH_PROTO_MAGIC;
    hdr->version = MESH_PROTO_VERSION;
    hdr->type    = MESH_MSG_PEER_STATE;
    memcpy(hdr->origin_mac, s_own_mac, 6);
    hdr->seq     = ++s_seq;
    hdr->ttl     = 3;   /* allows 2 rebroadcasts → 3-hop reach */

    mesh_peer_state_t *p = (mesh_peer_state_t *)(buf + sizeof(*hdr));
    p->device_type = (uint8_t)st->device_type;
    p->number      = st->device_number;
    p->queue_level = st->queue_level;
    p->sub_step    = st->queue_sub_step;

    return send_raw(buf, sizeof(buf));
}

esp_err_t mesh_transport_broadcast_slot_claim(device_type_t type, uint8_t want_slot)
{
    if (want_slot < 1 || want_slot > 16) return ESP_ERR_INVALID_ARG;

    uint8_t buf[sizeof(mesh_frame_hdr_t) + sizeof(mesh_slot_claim_t)];
    mesh_frame_hdr_t *hdr = (mesh_frame_hdr_t *)buf;
    hdr->magic   = MESH_PROTO_MAGIC;
    hdr->version = MESH_PROTO_VERSION;
    hdr->type    = MESH_MSG_SLOT_CLAIM;
    memcpy(hdr->origin_mac, s_own_mac, 6);
    hdr->seq     = ++s_seq;
    hdr->ttl     = 3;   /* allows 2 rebroadcasts → 3-hop reach */

    mesh_slot_claim_t *c = (mesh_slot_claim_t *)(buf + sizeof(*hdr));
    c->device_type = (uint8_t)type;
    c->want_slot   = want_slot;
    c->claim_phase = 1;

    return send_raw(buf, sizeof(buf));
}

void mesh_transport_get_stats(mesh_transport_stats_t *out)
{
    if (out) *out = s_stats;
}
