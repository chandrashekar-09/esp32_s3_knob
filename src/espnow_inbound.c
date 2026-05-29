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

#include <string.h>

#include "advisor.h"
#include "esp_log.h"
#include "peer_registry.h"
#include "phase_manager.h"

static const char *kTag = "espnow_in";

void espnow_inbound_peer_full(device_type_t type, uint8_t number,
                              uint8_t queue_level, int8_t sub_step,
                              const uint8_t mac[6])
{
    if (!mac) return;
    if (number < 1 || number > 16) return;
    if (queue_level < 1 || queue_level > 5) return;
    if (sub_step < -2 || sub_step > 2) sub_step = 0;
    peer_t p = {
        .online      = true,
        .type        = type,
        .number      = number,
        .queue_level = queue_level,
        .sub_step    = sub_step,
        /* trend fields + last_seen_ms maintained by upsert */
    };
    memcpy(p.mac, mac, 6);
    peer_registry_upsert(&p);
    /* Trigger an advisor re-evaluation here, not just on the next
     * own-side level change. Without this, a peer's queue movement
     * (e.g. peer FR2 → LONG QUE) would reach our registry but the
     * SEND>X advice cached on app_state would stay frozen at the
     * value computed during our last local input. */
    phase_manager_recompute_advisor();
    ESP_LOGI(kTag, "peer %s%u level=%u sub=%d mac=%02x:%02x:%02x:%02x:%02x:%02x",
             type == DEV_TYPE_FR ? "FR" : "T",
             (unsigned)number, (unsigned)queue_level, (int)sub_step,
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/* Legacy entry — no MAC available, no-op (upsert requires MAC now).
 * Kept so any local stub callers don't break the build. */
void espnow_inbound_peer(device_type_t type, uint8_t number,
                         uint8_t queue_level)
{
    (void)type; (void)number; (void)queue_level;
    ESP_LOGW(kTag, "legacy espnow_inbound_peer ignored — MAC required");
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
