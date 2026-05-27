#ifndef OTA_SERVICE_H
#define OTA_SERVICE_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Spawn the OTA polling task. Returns immediately. The task lives on
 * Core 0 so it never contends with the UI task on Core 1, waits for
 * WiFi to come up, then loops: every OTA_POLL_INTERVAL_MS it fetches
 * APP_OTA_VERSION_URL, compares to APP_OTA_CURRENT_VERSION, and if a
 * newer version is published runs esp_https_ota against
 * APP_OTA_FIRMWARE_URL. A successful flash triggers esp_restart(). */
esp_err_t ota_service_start(void);

#ifdef __cplusplus
}
#endif

#endif /* OTA_SERVICE_H */
