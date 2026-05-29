#ifndef MESH_SERVICE_H
#define MESH_SERVICE_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Boots the QueSort mesh stack:
 *   1. Initialise WiFi STA (no AP) + LR PHY + fixed channel.
 *   2. Initialise ESP-NOW broadcast peer + RX task.
 *   3. Run auto-slot-claim: listen ~4 s for occupied slots, pick
 *      a free one (preferring NVS-persisted or APP_DEVICE_NUMBER),
 *      commit to phase_manager, persist to NVS.
 *   4. Start the 1 Hz broadcaster task that pushes our state to
 *      the mesh every second.
 *
 * Replaces wifi_manager_init + ota_service_start when the build
 * flag FEATURE_ZERO_CONFIG_MESH is set. Production knobs run
 * offline — no router, no internet, no cloud.
 *
 * Idempotent: second call returns ESP_OK immediately. */
esp_err_t mesh_service_start(void);

#ifdef __cplusplus
}
#endif

#endif /* MESH_SERVICE_H */
