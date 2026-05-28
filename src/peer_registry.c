/* Peer registry — knob-mesh participant list.
 *
 * Slot 0 is reserved for the OWN device, kept in sync from
 * phase_manager state each render via peer_registry_sync_own(). Slots
 * 1..31 are populated by the (future) mesh layer via peer_registry_
 * upsert(). The registry is intentionally simple: linear array,
 * (type, number) as the natural key, no allocator, no notifications.
 * The UI calls peer_iter_start/next() each render to walk same-type
 * peers in number-ascending order.
 *
 * Today only the own device exists; the iterator yields exactly one
 * entry. When mesh ships, the iterator is what naturally drives the
 * tier (count_of_type) and the per-ring rendering loop — no changes
 * needed in the renderer when peers join/leave, just the registry
 * contents change.
 */

#include "peer_registry.h"

#include <string.h>

#include "esp_timer.h"

/* Trend tracker tuning — mirrors advisor constants. Lives here
 * rather than in advisor.h so peer_trend() can be a pure registry
 * concern that the advisor (and any future consumer) calls. */
#define TREND_WINDOW_MS   30000U
#define TREND_DELTA       2

static peer_t s_peers[PEER_REGISTRY_CAPACITY];

static uint32_t now_ms_(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

/* On a level CHANGE (curr != incoming), shift the trend window:
 * the old curr becomes prev. No-op if level unchanged. */
static void apply_level_change(peer_t *p, uint8_t new_level)
{
    if (p->online && p->queue_level == new_level) return;
    uint32_t now = now_ms_();
    p->prev_level    = p->online ? p->queue_level : new_level;
    p->prev_level_ms = p->online ? p->curr_level_ms : now;
    p->curr_level_ms = now;
}

void peer_registry_init(void)
{
    memset(s_peers, 0, sizeof(s_peers));
}

/* Find slot containing (type, number); -1 if not present. */
static int find_slot(device_type_t type, uint8_t number)
{
    for (int i = 0; i < PEER_REGISTRY_CAPACITY; i++) {
        if (s_peers[i].online &&
            s_peers[i].type == type &&
            s_peers[i].number == number) {
            return i;
        }
    }
    return -1;
}

void peer_registry_sync_own(const app_state_t *st)
{
    if (!st) return;
    /* Slot 0 is reserved for own device. We write it directly
     * rather than going through upsert() so the own-device slot
     * is stable across type toggles (FR3 → T3 via long-press
     * doesn't migrate to a different slot). Level changes are
     * detected here too so the trend tracker works for own. */
    apply_level_change(&s_peers[0], st->queue_level);
    s_peers[0].online      = true;
    s_peers[0].type        = st->device_type;
    s_peers[0].number      = st->device_number;
    s_peers[0].queue_level = st->queue_level;
    s_peers[0].sub_step    = st->queue_sub_step;
}

int peer_registry_upsert(const peer_t *p)
{
    if (!p) return -1;
    int idx = find_slot(p->type, p->number);
    if (idx < 0) {
        /* Skip slot 0 — reserved for own device. */
        for (int i = 1; i < PEER_REGISTRY_CAPACITY; i++) {
            if (!s_peers[i].online) { idx = i; break; }
        }
        if (idx < 0) return -1;  /* full */
    }
    /* Shift trend window BEFORE overwriting the slot so apply_level
     * _change sees the old level. The trend fields in *p are
     * ignored (caller doesn't compute them; we maintain them
     * locally). */
    apply_level_change(&s_peers[idx], p->queue_level);
    s_peers[idx].online      = true;
    s_peers[idx].type        = p->type;
    s_peers[idx].number      = p->number;
    s_peers[idx].queue_level = p->queue_level;
    s_peers[idx].sub_step    = p->sub_step;
    return idx;
}

void peer_registry_set_offline(device_type_t type, uint8_t number)
{
    int idx = find_slot(type, number);
    if (idx > 0) {  /* never offline slot 0 (own) here */
        s_peers[idx].online = false;
    }
}

uint8_t peer_registry_count_of_type(device_type_t type)
{
    uint8_t n = 0;
    for (int i = 0; i < PEER_REGISTRY_CAPACITY; i++) {
        if (s_peers[i].online && s_peers[i].type == type) n++;
    }
    return n;
}

/* Iterator yields in NUMBER-ASCENDING order. Internally we scan the
 * array for the smallest .number > cursor on each call. O(N²) total
 * for a full iteration but N ≤ 16 so the cost is trivial — and we
 * avoid keeping the array sorted (which would complicate upsert). */
void peer_iter_start(peer_iter_t *it, device_type_t type)
{
    if (!it) return;
    it->type   = type;
    it->cursor = 0;
}

const peer_t *peer_iter_next(peer_iter_t *it)
{
    if (!it) return NULL;
    const peer_t *best = NULL;
    for (int i = 0; i < PEER_REGISTRY_CAPACITY; i++) {
        const peer_t *p = &s_peers[i];
        if (!p->online || p->type != it->type) continue;
        if (p->number <= it->cursor) continue;
        if (!best || p->number < best->number) best = p;
    }
    if (best) {
        it->cursor = best->number;
    }
    return best;
}

int peer_trend(const peer_t *p, uint32_t now_ms)
{
    if (!p || !p->online) return 0;
    if ((now_ms - p->prev_level_ms) > TREND_WINDOW_MS) return 0;
    int delta = (int)p->queue_level - (int)p->prev_level;
    if (delta >=  TREND_DELTA) return +1;
    if (delta <= -TREND_DELTA) return -1;
    return 0;
}
