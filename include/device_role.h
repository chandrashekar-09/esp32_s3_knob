#ifndef DEVICE_ROLE_H
#define DEVICE_ROLE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Returns true when this physical device should ALWAYS run in
 * peer-simulation demo mode (random peers generated locally),
 * regardless of the APP_PEER_SIM compile flag.
 *
 * Match is by factory MAC address against a hard-coded allow-list
 * in src/device_role.c. Useful for keeping a single "demo knob"
 * permanently showing the multi-ring layouts even after the rest
 * of the fleet OTA-updates to a build with APP_PEER_SIM=0.
 *
 * Cached on first call — MAC lookup is one-shot. */
bool device_role_is_demo(void);

#ifdef __cplusplus
}
#endif

#endif /* DEVICE_ROLE_H */
