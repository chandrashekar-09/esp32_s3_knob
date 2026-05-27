#ifndef PEER_SIM_H
#define PEER_SIM_H

#include "phase_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Populate the peer registry with a random number of same-type
 * peers at random queue levels. Called once at startup when
 * APP_PEER_SIM is 1. Uses esp_random() so each boot produces a
 * different configuration — flash + reboot cycles through tier
 * possibilities (FULL / HALF / THIRD / QUARTER) and varied fill
 * patterns. No dynamic updates — values are static for the boot
 * session. */
void peer_sim_populate(const app_state_t *own);

#ifdef __cplusplus
}
#endif

#endif /* PEER_SIM_H */
