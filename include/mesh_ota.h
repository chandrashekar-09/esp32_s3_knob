#ifndef MESH_OTA_H
#define MESH_OTA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_mesh.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int current_version;
    const char *version_url;
    const char *firmware_url;
    const char *device_id;
    const char *firebase_boot_ack_base_url;
    const char *firebase_auth_token;
    size_t chunk_size;
    size_t task_stack;
    int task_prio;
} mesh_ota_config_t;

esp_err_t mesh_ota_init(const mesh_ota_config_t *config);

esp_err_t mesh_ota_start(void);

void mesh_ota_rx_cb(const mesh_addr_t *from,
                    const uint8_t *data,
                    size_t len,
                    void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* MESH_OTA_H */
