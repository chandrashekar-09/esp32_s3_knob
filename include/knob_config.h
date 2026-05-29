#ifndef KNOB_CONFIG_H
#define KNOB_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Per-knob runtime configuration backed by NVS — survives reboots
 * and OTA updates. Replaces the compile-time APP_MESH_ROUTER_SSID
 * hardcoding so installers can re-provision a knob in the field
 * without recompiling.
 *
 * Mutated by the provisioning flow (SoftAP captive portal); read
 * by mesh_transport (WiFi STA bringup) and mesh_service (cloud
 * gating).
 *
 * Defaults on first boot (no NVS entries):
 *   online_mode = true   (matches the FEATURE_CLOUD_FALLBACK era)
 *   wifi_ssid   = ""     (empty — knob runs ESP-NOW only until set)
 *   wifi_pass   = ""
 */

#define KNOB_CFG_SSID_MAX  33     /* WPA2 SSID max + NUL */
#define KNOB_CFG_PASS_MAX  65     /* WPA2-PSK ASCII max + NUL */

typedef struct {
    bool online_mode;                       /* gate for WiFi STA + cloud */
    char wifi_ssid[KNOB_CFG_SSID_MAX];      /* "" = no creds set */
    char wifi_pass[KNOB_CFG_PASS_MAX];
} knob_config_t;

/* One-shot initialiser — opens NVS, loads cfg, caches in RAM.
 * Safe to call multiple times. Returns ESP_OK even on first boot
 * (defaults applied silently). */
esp_err_t knob_config_init(void);

/* Returns a pointer to the in-RAM cache. Never NULL after init.
 * Read-only — mutate via knob_config_set_* + knob_config_commit. */
const knob_config_t *knob_config_get(void);

/* Mutators — write to the in-RAM cache. Call knob_config_commit()
 * afterwards to persist. Safe to call before init (no-op). */
void knob_config_set_online_mode(bool on);
void knob_config_set_wifi(const char *ssid, const char *pass);

/* Persist the in-RAM cache to NVS. */
esp_err_t knob_config_commit(void);

/* True iff a non-empty SSID is currently set. Mesh transport uses
 * this to decide whether to attempt WiFi STA association at all. */
bool knob_config_has_wifi(void);

#ifdef __cplusplus
}
#endif

#endif /* KNOB_CONFIG_H */
