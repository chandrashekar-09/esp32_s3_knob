/* Peer registry — knob-mesh participant list.
 *
 * Slot 0 is reserved for the OWN device, kept in sync from
 * phase_manager state each render via peer_registry_sync_own().
 * Slots 1..31 are populated by the mesh transport via
 * peer_registry_upsert(), keyed by MAC.
 *
 * MAC as primary key: a peer's `number` is a DERIVED rank (their
 * position in the MAC-sorted live-peer list) that can change as
 * the mesh population shifts. Slot lookup keys by MAC so a rank
 * change updates the existing slot in place instead of creating
 * an orphan at the old number. Iterator still yields in number
 * ascending order, so caller (UI/advisor) sees a clean 1..N list.
 *
 * Age-out: callers (mesh_service) call peer_registry_age_out()
 * periodically; peers whose last_seen_ms is older than the
 * timeout are marked offline. This is how a knob that powered
 * off / went out of range disappears from the displayed ring.
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
static uint8_t s_own_mac[6] = {0};
static bool    s_own_mac_set = false;

static uint32_t now_ms_(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static bool mac_is_zero(const uint8_t mac[6])
{
    for (int i = 0; i < 6; i++) if (mac[i] != 0) return false;
    return true;
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

void peer_registry_set_own_mac(const uint8_t mac[6])
{
    if (!mac) return;
    memcpy(s_own_mac, mac, 6);
    s_own_mac_set = true;
}

/* Find slot containing this MAC (online OR offline); -1 if not present. */
static int find_slot_by_mac(const uint8_t mac[6])
{
    for (int i = 0; i < PEER_REGISTRY_CAPACITY; i++) {
        if (s_peers[i].online && memcmp(s_peers[i].mac, mac, 6) == 0) {
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
     * stays at index 0 across type toggles. */
    apply_level_change(&s_peers[0], st->queue_level);
    s_peers[0].online      = true;
    s_peers[0].type        = st->device_type;
    s_peers[0].number      = st->device_number;
    s_peers[0].queue_level = st->queue_level;
    s_peers[0].sub_step    = st->queue_sub_step;
    s_peers[0].last_seen_ms = now_ms_();
    if (s_own_mac_set) {
        memcpy(s_peers[0].mac, s_own_mac, 6);
    }
}

int peer_registry_upsert(const peer_t *p)
{
    if (!p) return -1;
    if (mac_is_zero(p->mac)) return -1;   /* MAC required as primary key */

    int idx = find_slot_by_mac(p->mac);
    if (idx < 0) {
        /* Skip slot 0 — reserved for own device. */
        for (int i = 1; i < PEER_REGISTRY_CAPACITY; i++) {
            if (!s_peers[i].online) { idx = i; break; }
        }
        if (idx < 0) return -1;  /* full */
    }
    /* Shift trend window BEFORE overwriting the slot so apply_level
     * _change sees the old level. */
    apply_level_change(&s_peers[idx], p->queue_level);
    s_peers[idx].online        = true;
    s_peers[idx].type          = p->type;
    memcpy(s_peers[idx].mac, p->mac, 6);
    s_peers[idx].number        = p->number;
    s_peers[idx].queue_level   = p->queue_level;
    s_peers[idx].sub_step      = p->sub_step;
    s_peers[idx].last_seen_ms  = now_ms_();
    return idx;
}

void peer_registry_set_offline(device_type_t type, uint8_t number)
{
    /* Legacy API kept for espnow_inbound_peer_offline(). Best-effort
     * scan by (type, number) — MAC isn't available here. */
    for (int i = 1; i < PEER_REGISTRY_CAPACITY; i++) {
        if (s_peers[i].online &&
            s_peers[i].type == type &&
            s_peers[i].number == number) {
            s_peers[i].online = false;
            return;
        }
    }
}

bool peer_is_stale(const peer_t *p, uint32_t now_ms, uint32_t stale_ms)
{
    if (!p || !p->online) return false;
    return (now_ms - p->last_seen_ms) > stale_ms;
}

uint8_t peer_registry_age_out(uint32_t now_ms, uint32_t timeout_ms)
{
    uint8_t aged = 0;
    /* Skip slot 0 (own — sync_own keeps last_seen fresh anyway). */
    for (int i = 1; i < PEER_REGISTRY_CAPACITY; i++) {
        if (!s_peers[i].online) continue;
        if ((now_ms - s_peers[i].last_seen_ms) > timeout_ms) {
            s_peers[i].online = false;
            aged++;
        }
    }
    return aged;
}

uint8_t peer_registry_compute_rank(device_type_t type,
                                   uint8_t current_number)
{
    if (!s_own_mac_set) return 1;

    /* STICKY numbering with collision-only re-rank.
     *
     * Goal: once a knob is assigned a number (in the office, or by
     * NVS load at boot), it KEEPS that number through reboots,
     * moves between rooms, and topology changes. The only thing
     * that can change it is a direct collision with another knob
     * claiming the same number — and even then only the higher-MAC
     * knob moves.
     *
     * Compaction (shifting down to fill a gap left by a departing
     * peer) is deliberately NOT done — the user explicitly wants
     * numbers stable across the office → floor distribution.
     *
     * Three cases:
     *
     *   1. FIRST EVER (current_number == 0, no NVS yet):
     *      Pick max(visible_numbers) + 1, or lowest free if 16
     *      is hit. New office knobs get contiguous 1..N.
     *
     *   2. COLLISION (peer with my number AND lower MAC):
     *      I lose the tiebreak. Pick next free number above (or
     *      below if 16 is hit). The lower-MAC peer keeps its
     *      number. Multi-way collisions settle deterministically
     *      across a few ticks.
     *
     *   3. STABLE (everything else):
     *      Keep current_number. Even if the only other knob in
     *      the room is FR1 and I'm FR5, I stay FR5. */

    /* Build the "numbers taken by other same-type online knobs"
     * bitmap and detect any collision at current_number. */
    bool taken[17] = { false };
    bool collision_lose = false;   /* peer at my number with LOWER MAC */
    uint8_t max_seen = 0;

    for (int i = 0; i < PEER_REGISTRY_CAPACITY; i++) {
        if (!s_peers[i].online) continue;
        if (s_peers[i].type != type) continue;
        if (memcmp(s_peers[i].mac, s_own_mac, 6) == 0) continue;

        uint8_t n = s_peers[i].number;
        if (n >= 1 && n <= 16) {
            taken[n] = true;
            if (n > max_seen) max_seen = n;
        }
        if (n == current_number && current_number != 0 &&
            memcmp(s_peers[i].mac, s_own_mac, 6) < 0) {
            collision_lose = true;
        }
    }

    /* Case 1: first ever — pick max+1 or lowest free. */
    if (current_number == 0) {
        uint8_t pick = (uint8_t)(max_seen + 1);
        if (pick < 1 || pick > 16 || taken[pick]) {
            pick = 0;
            for (uint8_t n = 1; n <= 16; n++) {
                if (!taken[n]) { pick = n; break; }
            }
            if (pick == 0) pick = 1;   /* truly full, degenerate */
        }
        return pick;
    }

    /* Case 2: collision and we lose tiebreak — move to next free. */
    if (collision_lose) {
        for (uint8_t n = (uint8_t)(current_number + 1); n >= 1 && n <= 16; n++) {
            if (!taken[n]) return n;
        }
        for (uint8_t n = 1; n < current_number; n++) {
            if (!taken[n]) return n;
        }
        return current_number;   /* full, stuck */
    }

    /* Case 3: stable — keep our number. */
    return current_number;
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
