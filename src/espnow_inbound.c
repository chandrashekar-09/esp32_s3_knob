/* ESP-NOW inbound dispatch — dormant until mesh layer ships.
 *
 * These functions are entry points the future ESP-NOW receive task
 * will call after parsing a frame. They centralise the "what
 * happens when a mesh event lands" logic so the actual mesh code
 * (when it arrives) is small and protocol-only.
 *
 * Implemented now, called by nothing now. When mesh layer is
 * added, it does NOT need to know about peer_registry / phase_
 * manager / advisor — it just calls these.
 */

#include "espnow_inbound.h"

#include "advisor.h"
#include "esp_log.h"
#include "peer_registry.h"
#include "phase_manager.h"

static const char *kTag = "espnow_in";

void espnow_inbound_peer(device_type_t type, uint8_t number,
                         uint8_t queue_level)
{
    if (number < 1 || number > 16) return;
    if (queue_level < 1 || queue_level > 5) return;
    peer_t p = {
        .online      = true,
        .type        = type,
        .number      = number,
        .queue_level = queue_level,
        /* trend fields ignored by upsert — registry maintains them */
    };
    peer_registry_upsert(&p);
    /* Advisor pickup happens on the next ui_engine render tick
     * (apply_home calls peer_registry_sync_own + recompute path).
     * No explicit notify needed — the render cadence is fast
     * enough that a peer broadcast lands visibly within ~100 ms. */
    ESP_LOGD(kTag, "peer %s%u level=%u",
             type == DEV_TYPE_FR ? "FR" : "T",
             (unsigned)number, (unsigned)queue_level);
}

void espnow_inbound_master_cmd(device_type_t target_type,
                               uint8_t target_slot,
                               uint8_t level)
{
    app_state_t st;
    phase_manager_get_state(&st);
    if (target_type != st.device_type) return;        /* not for us */
    if (target_slot != st.device_number) return;      /* not for us */
    if (level < 1 || level > 5) return;
    ESP_LOGI(kTag, "master command → level %u (was %u)",
             (unsigned)level, (unsigned)st.queue_level);
    /* Unconditional level write via the dedicated reset-style API.
     * This bypasses the encoder's cumulative-progress accumulator
     * because the master command is an absolute level set, not a
     * relative nudge. Per spec, do NOT stamp last_user_input_us. */
    phase_manager_set_queue_level(level);
}

void espnow_inbound_peer_offline(device_type_t type, uint8_t number)
{
    peer_registry_set_offline(type, number);
    ESP_LOGD(kTag, "peer %s%u offline",
             type == DEV_TYPE_FR ? "FR" : "T", (unsigned)number);
}
