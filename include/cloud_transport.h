#ifndef CLOUD_TRANSPORT_H
#define CLOUD_TRANSPORT_H

#include "esp_err.h"
#include "phase_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Online-fallback transport for the QueSort mesh.
 *
 * Runs in PARALLEL with the ESP-NOW mesh transport. Knobs continue
 * to broadcast over ESP-NOW for low-latency local coordination;
 * this module additionally upserts their state into a Supabase
 * Postgres table so knobs that can't reach each other over RF can
 * still see each other via the cloud.
 *
 * Inbound peer states (other knobs' rows) arrive via the Realtime
 * subscription path (separate task — wired in a follow-up). Both
 * transports feed the SAME peer_registry; whichever delivers first
 * wins, the other is dedup'd via MAC match.
 *
 * All calls are best-effort and non-blocking from the caller's
 * perspective — failures are logged but don't propagate. If WiFi
 * is down or Supabase is unreachable, the ESP-NOW path keeps the
 * local mesh alive.
 */

/* Initialise the HTTPS client and cache the per-fleet store_id.
 * store_id is the SHA-256 of the configured SSID (first 8 hex
 * chars). Idempotent — second call is a no-op. */
esp_err_t cloud_transport_init(void);

/* True iff the module has been initialised AND the device has an
 * IP address (WiFi connected). When false, callers skip the upsert. */
bool cloud_transport_is_ready(void);

/* POST a single upsert row for our own state. Blocks until the
 * HTTPS request completes (typically ~200-500 ms over WiFi). Safe
 * to call from any task; an internal mutex serialises concurrent
 * calls. Returns ESP_OK on HTTP 2xx, ESP_FAIL otherwise. */
esp_err_t cloud_transport_upsert(const app_state_t *st);

/* Read-only accessors used by cloud_subscribe to build poll URLs
 * and filter own-MAC rows out of the response. Returns NULL before
 * cloud_transport_init() has been called. */
const char *cloud_transport_store_id(void);
const char *cloud_transport_own_mac(void);

#ifdef __cplusplus
}
#endif

#endif /* CLOUD_TRANSPORT_H */
