/* Peer-registry simulator — testing-mode only.
 *
 * Picks a random count of same-type peers (1..15 extra) at random
 * queue levels (1..5) and stuffs them into peer_registry. Every
 * boot is different (esp_random is hw-backed, non-deterministic).
 *
 * Disable by flipping APP_PEER_SIM to 0 in app_config.h — this
 * file's only call site is gated by that macro, so without the
 * flag the registry stays empty (own-device only) and the advisor
 * waits for real mesh data.
 */

#include "peer_sim.h"

#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "peer_registry.h"

static const char *kTag = "peer_sim";

void peer_sim_populate(const app_state_t *own)
{
    if (!own) return;

    /* Fresh-seed semantics: clear the entire registry first, then
     * re-add the own device + new random peers of own's CURRENT
     * type. Lets us call this both at boot AND on every type
     * toggle (FR↔T) to demo the multi-ring layout in whichever
     * mode the user just switched into. */
    peer_registry_init();
    peer_registry_sync_own(own);

    /* Random extra-peer count in [1, 15] — total fleet size 2..16,
     * which covers all four advisor tiers (FULL/HALF/THIRD/QUARTER).
     * Skewing slightly toward higher counts so multi-ring layouts
     * get exercised on most boots. */
    uint8_t extra = (uint8_t)((esp_random() % 15) + 1);

    /* SEQUENTIAL numbering 1..N (instead of random scatter). With
     * N=4 the fleet is exactly {1,2,3,4} — so a 4-ring layout reads
     * 1,2,3,4 in slot order, which is what the user expects for any
     * tier (FULL/HALF/THIRD/QUARTER). N = max(extra+1, own.number)
     * so own's configured number always fits inside the contiguous
     * range; if own=1, N=extra+1, no clamp. Capped at 16. */
    uint8_t total = (uint8_t)(extra + 1);
    if (own->device_number > total) total = own->device_number;
    if (total > 16) total = 16;

    /* Pass 1 — seed every expected number. */
    for (uint8_t num = 1; num <= total; num++) {
        if (num == own->device_number) continue;   /* own already added */

        uint8_t level = (uint8_t)((esp_random() % 5) + 1);
        /* Random sub-step -2..+2 so the advisor's sub_step-aware
         * FULL rejection has variety to exercise. */
        int8_t  sub   = (int8_t)((int)(esp_random() % 5) - 2);
        peer_t p = {
            .online      = true,
            .type        = own->device_type,
            .number      = num,
            .queue_level = level,
            .sub_step    = sub,
        };
        /* Synthesize a stable fake MAC from (type, num) so the
         * registry's MAC-keyed upsert works. Prefix 0xFE so it
         * can't collide with a real STA MAC (which has bit 0 of
         * byte 0 clear). Type in byte 4, number in byte 5 ensures
         * uniqueness across sim peers within this fleet. */
        p.mac[0] = 0xFE;
        p.mac[1] = 0xFE;
        p.mac[2] = 0x00;
        p.mac[3] = 0x00;
        p.mac[4] = (uint8_t)own->device_type;
        p.mac[5] = num;
        if (peer_registry_upsert(&p) < 0) {
            ESP_LOGE(kTag, "upsert FAILED for %s%u (pass 1)",
                     own->device_type == DEV_TYPE_FR ? "FR" : "T",
                     (unsigned)num);
        }
    }

    /* Pass 2 — VERIFY every expected number 1..total is now present
     * in the registry. Walk the iter and tick each seen number; any
     * gap means an upsert silently dropped (would explain the
     * "13 instead of 12, 16 alone" pattern). Retry the missing ones
     * so the rendered layout is guaranteed sequential 1..N. */
    bool seen[17] = { false };
    {
        peer_iter_t vi;
        peer_iter_start(&vi, own->device_type);
        const peer_t *vp;
        while ((vp = peer_iter_next(&vi)) != NULL) {
            if (vp->number >= 1 && vp->number <= 16) {
                seen[vp->number] = true;
            }
        }
    }
    for (uint8_t num = 1; num <= total; num++) {
        if (seen[num]) continue;
        ESP_LOGW(kTag, "peer %s%u missing after pass 1 — retrying",
                 own->device_type == DEV_TYPE_FR ? "FR" : "T",
                 (unsigned)num);
        uint8_t level = (uint8_t)((esp_random() % 5) + 1);
        /* Random sub-step -2..+2 so the advisor's sub_step-aware
         * FULL rejection has variety to exercise. */
        int8_t  sub   = (int8_t)((int)(esp_random() % 5) - 2);
        peer_t p = {
            .online      = true,
            .type        = own->device_type,
            .number      = num,
            .queue_level = level,
            .sub_step    = sub,
        };
        /* Same fake-MAC scheme as pass 1 so retry hits the existing
         * slot in place. */
        p.mac[0] = 0xFE;
        p.mac[1] = 0xFE;
        p.mac[4] = (uint8_t)own->device_type;
        p.mac[5] = num;
        if (peer_registry_upsert(&p) < 0) {
            ESP_LOGE(kTag, "retry FAILED for %s%u — registry probably full",
                     own->device_type == DEV_TYPE_FR ? "FR" : "T",
                     (unsigned)num);
        }
    }

    /* Final tally — count online peers of own's type and compare
     * against expected. Any mismatch is a serious bug; log loudly. */
    uint8_t got = peer_registry_count_of_type(own->device_type);
    if (got != total) {
        ESP_LOGE(kTag, "EXPECTED %u peers, REGISTRY HAS %u",
                 (unsigned)total, (unsigned)got);
    }

    ESP_LOGI(kTag, "TESTING MODE: %s seeded %u %s peers (own=%s%u, expected=%u)",
             got == total ? "OK" : "FAIL",
             (unsigned)got,
             own->device_type == DEV_TYPE_FR ? "FR" : "T",
             own->device_type == DEV_TYPE_FR ? "FR" : "T",
             (unsigned)own->device_number,
             (unsigned)total);

    /* Roster — every line shows the actual number stored so a missing
     * or duplicated number would jump out immediately in the serial
     * monitor. */
    peer_iter_t it;
    peer_iter_start(&it, own->device_type);
    const peer_t *p;
    while ((p = peer_iter_next(&it)) != NULL) {
        ESP_LOGI(kTag, "  %s%u  level=%u",
                 p->type == DEV_TYPE_FR ? "FR" : "T",
                 (unsigned)p->number, (unsigned)p->queue_level);
    }
}
