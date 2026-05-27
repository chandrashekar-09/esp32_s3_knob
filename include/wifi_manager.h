#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initialise NVS (idempotent), netif, event loop, and start WiFi STA
 * connecting to APP_MESH_ROUTER_SSID / APP_MESH_ROUTER_PASS. Returns
 * once the WiFi driver is started — connection happens asynchronously
 * with auto-reconnect on disconnect. Safe to call multiple times. */
esp_err_t wifi_manager_init(void);

/* True once the station has an IP from the AP. Polled by the OTA task
 * before issuing HTTPS requests. */
bool wifi_manager_is_connected(void);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_MANAGER_H */
