#ifndef PROVISIONING_H
#define PROVISIONING_H

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Provisioning mode — SoftAP + captive portal HTTP server.
 *
 * Triggered by a 10-second long-press on the screen while in HOME.
 * Lifecycle:
 *   1. WiFi switches to APSTA mode; SoftAP comes up with SSID
 *      "QueSort-FR<N>-<XXYY>" (no password, channel 6).
 *   2. HTTP server on 192.168.4.1 serves /, /scan, /save.
 *   3. UI's apply_provisioning() reads provisioning_ssid() and
 *      provisioning_qr_string() to render the toggle + QR.
 *   4. Operator scans QR, opens browser, picks WiFi + sets online
 *      mode, taps Save → knob_config NVS write + esp_restart().
 *
 * Exit paths:
 *   - Captive portal Save → reboot into normal operation with new creds.
 *   - Another 10-s long-press in PH_PROVISIONING → tear down, reboot.
 *   - 5-minute idle timeout → auto-cancel + reboot.
 */
esp_err_t provisioning_start(void);
void      provisioning_stop(void);
bool      provisioning_is_active(void);

/* For ui_engine apply_provisioning() to render. NULL before start. */
const char *provisioning_softap_ssid(void);

/* WiFi-QR connect string. iOS / Android camera natively parses
 * this and offers a one-tap "Join WiFi" prompt. Format:
 *   WIFI:S:<ssid>;T:nopass;P:;;
 */
const char *provisioning_qr_string(void);

#ifdef __cplusplus
}
#endif

#endif /* PROVISIONING_H */
