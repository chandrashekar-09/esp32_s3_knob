#ifndef MESH_MANAGER_H
#define MESH_MANAGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_mesh.h"
#include "esp_wifi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*mesh_manager_rx_cb_t)(const mesh_addr_t *from,
                                     const uint8_t *data,
                                     size_t len,
                                     void *ctx);

typedef struct {
    uint8_t mesh_id[6];
    const char *router_ssid;
    const char *router_pass;
    const char *mesh_ap_pass;
    uint8_t channel; /* 0 = auto */
    uint8_t max_layer;
    esp_mesh_topology_t topology;
    wifi_auth_mode_t ap_authmode;
    uint8_t ap_connections;
    uint8_t non_mesh_connections;
    uint16_t route_table_size;
    bool enable_ps;
    bool enable_demo_p2p;
    size_t tx_task_stack;
    size_t rx_task_stack;
    int tx_task_prio;
    int rx_task_prio;
    mesh_manager_rx_cb_t rx_cb;
    void *rx_cb_ctx;
} mesh_manager_config_t;

esp_err_t mesh_manager_start(const mesh_manager_config_t *config);
esp_err_t mesh_manager_stop(void);

bool mesh_manager_is_root(void);
int mesh_manager_get_layer(void);

esp_err_t mesh_manager_send(const mesh_addr_t *to,
                            const uint8_t *data,
                            size_t len,
                            bool p2p);

esp_err_t mesh_manager_send_broadcast(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* MESH_MANAGER_H */
