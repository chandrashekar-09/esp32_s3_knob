#ifndef CLOUD_SUBSCRIBE_H
#define CLOUD_SUBSCRIBE_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Cloud-side mesh DOWNLINK.
 *
 * Spawns a low-priority task that polls
 *   GET /rest/v1/knob_states?store_id=eq.<store>&select=*
 * every CLOUD_POLL_INTERVAL_MS (default 2 s) and dispatches each
 * row (except own MAC) into peer_registry via espnow_inbound_peer_full.
 *
 * From the renderer + advisor's perspective, cloud peers are
 * indistinguishable from ESP-NOW peers — they land in the same
 * registry via the same upsert call. MAC is the primary key, so a
 * peer that's reachable on BOTH paths is still one row (the most
 * recent update wins).
 *
 * Idempotent. Safe to call before WiFi is up — the task waits
 * until cloud_transport_is_ready(). */
esp_err_t cloud_subscribe_start(void);

#ifdef __cplusplus
}
#endif

#endif /* CLOUD_SUBSCRIBE_H */
