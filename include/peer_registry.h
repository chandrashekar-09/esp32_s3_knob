#ifndef PEER_REGISTRY_H
#define PEER_REGISTRY_H

#include <stdbool.h>
#include <stdint.h>

#include "phase_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A single peer entry in the mesh-wide knob registry. Up to 32 peers
 * total — 16 FR + 16 T — share one mesh; HOME on each knob filters
 * by type so only same-type peers feed the queue-ring layout.
 *
 * The prev_level / *_ms fields drive the trend tracker — the advisor
 * uses (level, trend) pairs to decide which peers count as senders /
 * receivers (a clearing sender is left alone; a filling receiver
 * isn't piled onto). Trend = sign(curr - prev) if |delta| >= 2 AND
 * (now - prev_level_ms) <= ADV_TREND_WINDOW_MS (30 s); else 0. */
typedef struct {
    bool          online;
    device_type_t type;
    uint8_t       number;          /* 1..16 */
    uint8_t       queue_level;     /* 1..5  */
    uint8_t       prev_level;      /* previous distinct level (trend) */
    uint32_t      prev_level_ms;   /* when prev_level became prev    */
    uint32_t      curr_level_ms;   /* when curr level became current */
} peer_t;

#define PEER_REGISTRY_CAPACITY  32

/* Initialise empty. Should be followed by peer_registry_sync_own()
 * to seed slot 0 with this knob's own identity from app_state. */
void peer_registry_init(void);

/* Insert / update the own device from the current phase_manager
 * state. Called every frame so the registry sees long-press type
 * toggles and queue-level rotations without explicit notify. */
void peer_registry_sync_own(const app_state_t *st);

/* Insert or update a peer by (type, number) key. Returns slot
 * index used, or -1 if registry is full. To be wired into the
 * mesh layer when it lands. */
int peer_registry_upsert(const peer_t *p);

/* Mark a peer offline (e.g. mesh saw it leave). The slot remains
 * allocated so re-joining doesn't churn indices. */
void peer_registry_set_offline(device_type_t type, uint8_t number);

/* How many ONLINE peers of given type exist. Used by the UI to
 * pick the tier (FULL / HALF / THIRD / QUARTER). */
uint8_t peer_registry_count_of_type(device_type_t type);

/* Type-filtered iterator. Yields online peers of the given type in
 * NUMBER-ASCENDING order so render order matches identity (FR1
 * outermost, FR4 innermost in 1-4 tier; same logic generalises to
 * higher tiers). Returns NULL when done. */
typedef struct {
    device_type_t type;
    uint8_t       cursor;
} peer_iter_t;

void peer_iter_start(peer_iter_t *it, device_type_t type);
const peer_t *peer_iter_next(peer_iter_t *it);

/* Trend for one peer — used by the advisor's sender/receiver
 * filtering. Returns:
 *   +1 if level rose by >= 2 within the last 30 s (filling)
 *   -1 if level fell by >= 2 within the last 30 s (clearing)
 *    0 otherwise (stable, or trend stale)
 * Pass current monotonic time in ms (esp_timer-derived). */
int peer_trend(const peer_t *p, uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* PEER_REGISTRY_H */
