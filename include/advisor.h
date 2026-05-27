#ifndef ADVISOR_H
#define ADVISOR_H

#include <stdbool.h>
#include <stdint.h>

#include "phase_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Redirect-advisor advice — populated by advisor_recompute() from
 * the current peer_registry snapshot. The UI renders the SEND>X
 * line iff active, tinted urgent if urgent, with a "↓" suffix if
 * trend_down.
 *
 * Per project_sorting_architecture.md — every knob in the mesh
 * runs the same advisor on the same fleet snapshot, so the result
 * is fleet-wide consistent without a central coordinator. Sender
 * sees "redirect to N", N's knob sees nothing (it's a receiver).
 */
typedef struct {
    bool    active;          /* true if self should redirect away */
    uint8_t target_number;   /* peer number (1..16) to redirect to */
    uint8_t target_level;    /* target's queue level — drives subtitle */
    bool    urgent;          /* self is LONG QUE → brighter red render */
    bool    trend_down;      /* target's queue clearing → "↓" marker */
} advisor_advice_t;

/* Reset internal hysteresis state. Call once at startup. */
void advisor_init(void);

/* Recompute advice from the current peer_registry + own state.
 * Walks the peer iterator, classifies senders/receivers, runs the
 * greedy assignment, and writes the result to *out. Idempotent —
 * safe to call every render or only on state change. */
void advisor_recompute(const app_state_t *st, advisor_advice_t *out);

#ifdef __cplusplus
}
#endif

#endif /* ADVISOR_H */
