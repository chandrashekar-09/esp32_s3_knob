/* Per-device role/identity decisions that survive OTA.
 *
 * The fleet ships a single firmware binary, but specific physical
 * knobs may need different behaviour (e.g. one is the "demo unit"
 * that always runs the peer simulator for showing scenarios). We
 * make those decisions by matching the factory MAC address against
 * a hard-coded table in this file — that survives every OTA cycle
 * because the table is part of the firmware itself, and the MAC
 * is permanent eFuse data tied to the silicon.
 *
 * Add a knob to the demo list by appending its MAC to kDemoMacs.
 */

#include "device_role.h"

#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"

static const char *kTag = "device_role";

/* MAC addresses of knobs that should ALWAYS run in demo (peer-sim)
 * mode. Lookup is via ESP_MAC_EFUSE_FACTORY so the value is fixed
 * at chip programming time and doesn't depend on which interface
 * (WiFi STA / AP / BT) is active. */
static const uint8_t kDemoMacs[][6] = {
    /* COM13 demo knob — confirmed via `esptool chip_id` 2026-05-28.
     * Add more entries below to enroll additional demo units. */
    { 0xac, 0xa7, 0x04, 0xef, 0x76, 0x08 },
};
#define DEMO_MAC_COUNT (sizeof(kDemoMacs) / sizeof(kDemoMacs[0]))

bool device_role_is_demo(void)
{
    static bool s_cached      = false;
    static bool s_is_demo     = false;
    if (s_cached) return s_is_demo;

    uint8_t mac[6] = {0};
    esp_err_t err = esp_read_mac(mac, ESP_MAC_EFUSE_FACTORY);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "esp_read_mac failed (%s) — treating as non-demo",
                 esp_err_to_name(err));
        s_cached  = true;
        s_is_demo = false;
        return false;
    }

    for (size_t i = 0; i < DEMO_MAC_COUNT; i++) {
        if (memcmp(mac, kDemoMacs[i], 6) == 0) {
            ESP_LOGI(kTag, "MAC %02x:%02x:%02x:%02x:%02x:%02x → DEMO mode",
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
            s_cached  = true;
            s_is_demo = true;
            return true;
        }
    }

    ESP_LOGI(kTag, "MAC %02x:%02x:%02x:%02x:%02x:%02x → production mode",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    s_cached  = true;
    s_is_demo = false;
    return false;
}
