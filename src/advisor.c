/* QueSort redirect advisor.
 *
 * Pure-logic over the peer_registry snapshot. Decides — for the
 * own device — whether to redirect customers to a peer with spare
 * capacity, and if so, which peer. Every knob runs the same
 * algorithm on the same (eventually-consistent) snapshot, so the
 * fleet's redirect graph stabilises without a coordinator.
 *
 * See project_sorting_architecture.md for the full spec including
 * the 8-state → 5-state constant mapping rationale.
 *
 * Constants (5-state values):
 *   ADV_SENDER_FROM     = 4   QUE — may redirect away
 *   ADV_RECEIVER_TO     = 2   FREE — may absorb a redirect
 *   ADV_MIN_GAP         = 2   sender − receiver gap floor
 *   ADV_URGENT_FROM     = 5   LONG QUE → urgent render
 *   ADV_HYSTERESIS_MS   = 20000  hold target 20 s to prevent flicker
 *
 * Receiver caps (max sends a receiver can absorb per cycle):
 *   EMPTY (1) → 3
 *   FREE  (2) → 2
 */

#include "advisor.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "peer_registry.h"

static const char *kTag = "advisor";

#define ADV_SENDER_FROM     4
#define ADV_RECEIVER_TO     2
#define ADV_MIN_GAP         2
#define ADV_URGENT_FROM     5
#define ADV_HYSTERESIS_MS   20000U

/* Snapshot capacity = peer registry capacity. */
#define SNAPSHOT_MAX        32

typedef struct {
    uint8_t number;
    uint8_t level;
    int8_t  trend;
    bool    is_self;
} fleet_node_t;

typedef struct {
    uint8_t number;
    uint8_t level;
    int8_t  trend;
    bool    is_self;
} sender_entry_t;

typedef struct {
    uint8_t number;
    uint8_t level;
    int8_t  trend;
    uint8_t cap;
} receiver_entry_t;

/* Hysteresis: hold a chosen target for ADV_HYSTERESIS_MS so brief
 * fluctuations in receiver levels don't flicker the SEND>X line.
 * Cleared on inactive recompute. */
static uint8_t  s_last_target_num = 0;
static uint32_t s_last_target_ms  = 0;

static uint32_t now_ms_(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static uint8_t receiver_cap(uint8_t level)
{
    if (level == 1) return 3;   /* EMPTY — most absorptive */
    if (level == 2) return 2;   /* FREE */
    return 0;                    /* FULL/QUE/LONG QUE not receivers */
}

void advisor_init(void)
{
    s_last_target_num = 0;
    s_last_target_ms  = 0;
}

void advisor_recompute(const app_state_t *st, advisor_advice_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!st) return;

    uint32_t now = now_ms_();
    uint8_t  self_level = st->queue_level;

    /* ── Step 1: Hysteresis check ────────────────────────────────
     * If the previous target is recent AND still a valid receiver
     * (online, still ≤ FREE, gap still ≥ 2, same type), hold it. */
    if (s_last_target_num != 0 &&
        (now - s_last_target_ms) < ADV_HYSTERESIS_MS) {
        peer_iter_t it;
        peer_iter_start(&it, st->device_type);
        const peer_t *p;
        const peer_t *held = NULL;
        while ((p = peer_iter_next(&it)) != NULL) {
            if (p->number == s_last_target_num) { held = p; break; }
        }
        if (held && held->online &&
            held->queue_level <= ADV_RECEIVER_TO &&
            (int)self_level - (int)held->queue_level >= ADV_MIN_GAP) {
            out->active        = true;
            out->target_number = held->number;
            out->target_level  = held->queue_level;
            out->urgent        = (self_level >= ADV_URGENT_FROM);
            out->trend_down    = (peer_trend(held, now) < 0);
            return;
        }
    }

    /* ── Step 2: Build fleet snapshot (same-type peers + self) ── */
    fleet_node_t fleet[SNAPSHOT_MAX];
    int n_fleet = 0;
    peer_iter_t it;
    peer_iter_start(&it, st->device_type);
    const peer_t *p;
    while ((p = peer_iter_next(&it)) != NULL && n_fleet < SNAPSHOT_MAX) {
        if (p->queue_level < 1 || p->queue_level > 5) continue;
        fleet[n_fleet].number  = p->number;
        fleet[n_fleet].level   = p->queue_level;
        fleet[n_fleet].trend   = (int8_t)peer_trend(p, now);
        fleet[n_fleet].is_self = (p->type == st->device_type &&
                                   p->number == st->device_number);
        n_fleet++;
    }

    /* Need at least 2 nodes for a redirect to make sense. */
    if (n_fleet < 2) {
        s_last_target_num = 0;
        return;
    }

    /* ── Step 3: Build sender list ──────────────────────────────
     * level >= SENDER_FROM AND trend >= 0 (don't redirect from a
     * knob that's already clearing). */
    sender_entry_t senders[SNAPSHOT_MAX];
    int n_senders = 0;
    for (int i = 0; i < n_fleet; i++) {
        if (fleet[i].level >= ADV_SENDER_FROM && fleet[i].trend >= 0) {
            senders[n_senders].number  = fleet[i].number;
            senders[n_senders].level   = fleet[i].level;
            senders[n_senders].trend   = fleet[i].trend;
            senders[n_senders].is_self = fleet[i].is_self;
            n_senders++;
        }
    }
    if (n_senders == 0) {
        s_last_target_num = 0;
        return;
    }
    /* Sort senders: level DESC (most overloaded first), number ASC tiebreak. */
    for (int i = 1; i < n_senders; i++) {
        sender_entry_t key = senders[i];
        int j = i - 1;
        while (j >= 0 &&
               (senders[j].level < key.level ||
                (senders[j].level == key.level && senders[j].number > key.number))) {
            senders[j + 1] = senders[j];
            j--;
        }
        senders[j + 1] = key;
    }

    /* ── Step 4: Build receiver list ────────────────────────────
     * level <= RECEIVER_TO AND trend <= 0 (don't pile onto a
     * filling receiver). Excludes self. Each gets a cap. */
    receiver_entry_t receivers[SNAPSHOT_MAX];
    int n_receivers = 0;
    for (int i = 0; i < n_fleet; i++) {
        if (fleet[i].is_self) continue;
        if (fleet[i].level <= ADV_RECEIVER_TO && fleet[i].trend <= 0) {
            receivers[n_receivers].number = fleet[i].number;
            receivers[n_receivers].level  = fleet[i].level;
            receivers[n_receivers].trend  = fleet[i].trend;
            receivers[n_receivers].cap    = receiver_cap(fleet[i].level);
            n_receivers++;
        }
    }

    /* ── Step 5: Greedy assignment ──────────────────────────────
     * For each sender (most overloaded first), find best-ranked
     * receiver with cap > 0 and gap >= MIN_GAP. Receiver ranking:
     *   1. level ASC                 (most absorptive first)
     *   2. |number - sender_number| ASC  (proximity)
     *   3. number ASC                (deterministic tiebreak)
     * Track the assignment for self only — that's all we render. */
    uint8_t self_target_num    = 0;
    uint8_t self_target_level  = 0;
    int8_t  self_target_trend  = 0;

    for (int si = 0; si < n_senders; si++) {
        int best_ri = -1;
        for (int ri = 0; ri < n_receivers; ri++) {
            if (receivers[ri].cap == 0) continue;
            int gap = (int)senders[si].level - (int)receivers[ri].level;
            if (gap < ADV_MIN_GAP) continue;
            if (best_ri < 0) { best_ri = ri; continue; }
            /* Compare ri vs best_ri using the 3-key ranking. */
            const receiver_entry_t *cur  = &receivers[ri];
            const receiver_entry_t *best = &receivers[best_ri];
            int cur_dist  = (int)cur->number  - (int)senders[si].number;
            if (cur_dist  < 0) cur_dist  = -cur_dist;
            int best_dist = (int)best->number - (int)senders[si].number;
            if (best_dist < 0) best_dist = -best_dist;

            bool replace = false;
            if (cur->level < best->level)                                     replace = true;
            else if (cur->level == best->level && cur_dist < best_dist)       replace = true;
            else if (cur->level == best->level && cur_dist == best_dist &&
                     cur->number < best->number)                              replace = true;
            if (replace) best_ri = ri;
        }
        if (best_ri >= 0) {
            receivers[best_ri].cap--;
            if (senders[si].is_self) {
                self_target_num   = receivers[best_ri].number;
                self_target_level = receivers[best_ri].level;
                self_target_trend = receivers[best_ri].trend;
            }
        }
    }

    /* ── Step 6 + 7: Emit advice + stamp hysteresis ──────────── */
    if (self_target_num != 0) {
        out->active        = true;
        out->target_number = self_target_num;
        out->target_level  = self_target_level;
        out->urgent        = (self_level >= ADV_URGENT_FROM);
        out->trend_down    = (self_target_trend < 0);
        if (s_last_target_num != self_target_num) {
            ESP_LOGI(kTag, "redirect → %u (level=%u%s)",
                     self_target_num, self_target_level,
                     out->urgent ? ", urgent" : "");
        }
        s_last_target_num = self_target_num;
        s_last_target_ms  = now;
    } else {
        if (s_last_target_num != 0) {
            ESP_LOGI(kTag, "redirect cleared");
        }
        s_last_target_num = 0;
    }
}
