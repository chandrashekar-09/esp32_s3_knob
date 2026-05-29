/* Mesh orchestration: bring up the transport, broadcast state at
 * 1 Hz, periodically renumber based on MAC-rank consensus, age out
 * peers that stopped broadcasting.
 *
 * RANK CONSENSUS (replaces NVS-persisted slots):
 *   Each knob's `number` (FR1..16) is a DERIVED position in the
 *   MAC-sorted list of online same-type peers. Lowest MAC = rank 1.
 *   Every knob runs the same comparison on the same registry
 *   snapshot, so they converge on a contiguous 1..N renumbering
 *   without any central coordinator. When a peer joins → ranks
 *   shift, new knob gets the top number. When a peer leaves or
 *   type-toggles → registry loses it (age-out), ranks compact down.
 *
 * Sequence per broadcaster tick (~1 Hz):
 *   1. age_out: peers not heard in MESH_PEER_TIMEOUT_MS go offline.
 *   2. compute_rank: my MAC's position in same-type live set.
 *   3. If rank changed → phase_manager_set_device_number(new_rank).
 *   4. Broadcast our state (with the possibly-new number).
 *
 * No NVS slot persistence — the rank IS the identity, computed
 * fresh every cycle. A power-cycle just means we're briefly rank 1
 * (alone), then converge once peer broadcasts arrive.
 */

#include "mesh_service.h"

#include <string.h>

#include "app_config.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mesh_transport.h"
#include "nvs.h"
#include "peer_registry.h"
#include "phase_manager.h"

#if FEATURE_CLOUD_FALLBACK
#include "cloud_config.h"
#include "cloud_subscribe.h"
#include "cloud_transport.h"
#include "knob_config.h"
#endif

#define MESH_NVS_NAMESPACE  "mesh"
#define MESH_NVS_KEY_NUMBER "fr_num"   /* persisted FR/T rank */

static uint8_t nvs_load_number(void)
{
    nvs_handle_t h;
    if (nvs_open(MESH_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return 0;
    uint8_t n = 0;
    nvs_get_u8(h, MESH_NVS_KEY_NUMBER, &n);
    nvs_close(h);
    return (n >= 1 && n <= 16) ? n : 0;
}

static void nvs_save_number(uint8_t n)
{
    if (n < 1 || n > 16) return;
    nvs_handle_t h;
    if (nvs_open(MESH_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, MESH_NVS_KEY_NUMBER, n);
    nvs_commit(h);
    nvs_close(h);
}

static const char *kTag = "mesh_svc";

#define MESH_BROADCAST_INTERVAL_MS  1000U   /* per spec: ~1 Hz */
/* Broadcast jitter: each tick sleeps INTERVAL ± JITTER. Prevents
 * lockstep collisions where two simultaneous frames collide. */
#define MESH_BROADCAST_JITTER_MS    150U
/* Burst on level change: when own's queue_level changes (encoder
 * commit, master command), send N frames spaced BURST_GAP_MS so a
 * single dropped frame doesn't strand peers on stale state.
 * Multi-floor / cluttered RF makes single-frame propagation
 * fragile; 3 redundant frames at 70 ms spacing covers typical
 * loss bursts without overwhelming the channel. */
#define MESH_BURST_FRAMES           3U
#define MESH_BURST_GAP_MS           70U
/* Grace period: a peer goes "stale" (50 % opacity in the UI) after
 * MESH_PEER_STALE_MS of silence, and is fully removed after
 * MESH_PEER_TIMEOUT_MS. Multi-story retail loses occasional frames
 * to walls, so the grace window lets a peer recover without
 * triggering a rank-reshuffle on every brief dropout. */
#define MESH_PEER_STALE_MS          3000U   /* ~3 missed beats → fade */
#define MESH_PEER_TIMEOUT_MS        8000U   /* ~8 missed beats → drop */
#define MESH_INITIAL_LISTEN_MS      3000U   /* listen before our first broadcast */

static bool s_started = false;

/* ───────────────────────────── broadcaster task ─────────────────── */
static void broadcaster_task(void *arg)
{
    (void)arg;
    ESP_LOGI(kTag, "broadcaster started (every %u ms, peer timeout %u ms)",
             (unsigned)MESH_BROADCAST_INTERVAL_MS,
             (unsigned)MESH_PEER_TIMEOUT_MS);

    /* Initial quiet listen — give existing mesh members a chance to
     * announce themselves before we pick our rank, so we don't
     * briefly broadcast as rank 1 when established peers are
     * already online. After this window the rank-stable algorithm
     * places us at the END of the rank list (max+1). */
    vTaskDelay(pdMS_TO_TICKS(MESH_INITIAL_LISTEN_MS));

    /* Load any persisted number from NVS. If found, this knob keeps
     * its previous identity through reboots and moves between rooms.
     * Only changes via collision resolution. */
    uint8_t last_broadcast_number = nvs_load_number();
    if (last_broadcast_number) {
        ESP_LOGI(kTag, "loaded persisted number FR%u from NVS",
                 (unsigned)last_broadcast_number);
        /* Push to phase_manager immediately so the UI shows the
         * correct identity from the first frame, not "FR1 alone". */
        phase_manager_set_device_number(last_broadcast_number);
    } else {
        ESP_LOGI(kTag, "no persisted number — will pick max+1 after listen");
    }

    /* Track last broadcast level so we can detect a commit and
     * fire a redundancy burst. Initialised to a sentinel that
     * triggers the burst on first iteration so peers learn our
     * starting level fast. */
    uint8_t last_broadcast_level = 0;
    TickType_t next_stats = xTaskGetTickCount() + pdMS_TO_TICKS(60000);

    while (true) {
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);

        /* Age out stale peers first so rank doesn't include ghosts. */
        uint8_t aged = peer_registry_age_out(now_ms, MESH_PEER_TIMEOUT_MS);
        if (aged) {
            ESP_LOGI(kTag, "aged out %u stale peer(s)", (unsigned)aged);
        }

        /* Recompute our rank. Pass our LAST broadcast number so
         * established rank is preserved when a new knob joins. */
        app_state_t st;
        phase_manager_get_state(&st);
        uint8_t rank = peer_registry_compute_rank(st.device_type,
                                                  last_broadcast_number);
        if (rank != last_broadcast_number) {
            ESP_LOGI(kTag, "rank: %u → %u (%s) — persisting to NVS",
                     (unsigned)last_broadcast_number, (unsigned)rank,
                     st.device_type == DEV_TYPE_FR ? "FR" : "T");
            last_broadcast_number = rank;
            nvs_save_number(rank);
        }
        if (rank >= 1 && rank <= 16 && rank != st.device_number) {
            phase_manager_set_device_number(rank);
            st.device_number = rank;
        }

        if (st.device_number >= 1 && st.device_number <= 16) {
            bool level_committed = (st.queue_level != last_broadcast_level);

            /* ESP-NOW path (primary). Level change → burst for
             * redundancy. Otherwise single heartbeat each tick. */
            if (level_committed) {
                ESP_LOGI(kTag, "level commit %u → %u — burst x%u",
                         (unsigned)last_broadcast_level,
                         (unsigned)st.queue_level,
                         (unsigned)MESH_BURST_FRAMES);
                for (uint32_t b = 0; b < MESH_BURST_FRAMES; b++) {
                    mesh_transport_broadcast_state(&st);
                    if (b + 1 < MESH_BURST_FRAMES) {
                        vTaskDelay(pdMS_TO_TICKS(MESH_BURST_GAP_MS));
                    }
                }
                last_broadcast_level = st.queue_level;
            } else {
                esp_err_t err = mesh_transport_broadcast_state(&st);
                if (err != ESP_OK) {
                    ESP_LOGD(kTag, "broadcast err %s", esp_err_to_name(err));
                }
            }

#if FEATURE_CLOUD_FALLBACK
            /* Cloud path (secondary). Skip entirely if the user
             * has online_mode=OFF or no WiFi creds saved. */
            if (knob_config_get()->online_mode) {
                static uint32_t last_cloud_post_ms = 0;
                bool heartbeat_due =
                    (now_ms - last_cloud_post_ms) >= CLOUD_HEARTBEAT_MS;
                if ((level_committed || heartbeat_due) &&
                    cloud_transport_is_ready()) {
                    if (cloud_transport_upsert(&st) == ESP_OK) {
                        last_cloud_post_ms = now_ms;
                    }
                }
            }
#endif
        }

        if (xTaskGetTickCount() >= next_stats) {
            mesh_transport_stats_t s = {0};
            mesh_transport_get_stats(&s);
            ESP_LOGI(kTag, "stats: tx ok=%u fail=%u | rx total=%u dup=%u bad=%u",
                     (unsigned)s.tx_ok, (unsigned)s.tx_fail,
                     (unsigned)s.rx_total, (unsigned)s.rx_dup,
                     (unsigned)s.rx_bad);
            next_stats = xTaskGetTickCount() + pdMS_TO_TICKS(60000);
        }

        /* Sleep INTERVAL ± JITTER — see MESH_BROADCAST_JITTER_MS. */
        uint32_t jitter = esp_random() % (2 * MESH_BROADCAST_JITTER_MS + 1);
        uint32_t sleep_ms = MESH_BROADCAST_INTERVAL_MS
                          - MESH_BROADCAST_JITTER_MS
                          + jitter;
        vTaskDelay(pdMS_TO_TICKS(sleep_ms));
    }
}

/* ───────────────────────────── public API ───────────────────────── */
esp_err_t mesh_service_start(void)
{
    if (s_started) return ESP_OK;

    esp_err_t err = mesh_transport_init();
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "transport init failed: %s", esp_err_to_name(err));
        return err;
    }

#if FEATURE_CLOUD_FALLBACK
    /* Cloud uplink + downlink only run when the user has online
     * mode enabled AND configured WiFi credentials. With online
     * mode OFF the knob is pure ESP-NOW — saves power, never
     * touches the internet, no cloud bills. */
    if (knob_config_get()->online_mode && knob_config_has_wifi()) {
        cloud_transport_init();
        cloud_subscribe_start();
    } else {
        ESP_LOGI(kTag, "cloud disabled (online_mode=%s, ssid=%s)",
                 knob_config_get()->online_mode ? "ON" : "OFF",
                 knob_config_has_wifi() ? "set" : "empty");
    }
#endif

    /* If NVS has a persisted number, use it immediately so the
     * very first paint shows the correct identity. Otherwise the
     * broadcaster will pick max+1 after its initial listen window. */
    uint8_t persisted = nvs_load_number();
    phase_manager_set_device_number(persisted ? persisted : 1);

    /* 8192-byte stack — small enough for ESP-NOW broadcasts alone
     * (3 KB would do), but the cloud_transport_upsert path runs in
     * this same task and mbedTLS / TLS handshake needs ~30-50 KB of
     * peak stack. Without this, the first HTTPS POST to Supabase
     * overflows and the chip reboots in a loop. */
    BaseType_t ok = xTaskCreatePinnedToCore(broadcaster_task, "mesh_tx",
                                            8192, NULL, 4, NULL, 0);
    if (ok != pdPASS) {
        ESP_LOGE(kTag, "broadcaster task create failed");
        return ESP_FAIL;
    }

    s_started = true;
    ESP_LOGI(kTag, "mesh service started");
    return ESP_OK;
}
