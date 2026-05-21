#include <string.h>
#include <stdlib.h>
#include <inttypes.h>

#include "mesh_manager.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_mesh_internal.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "mesh_light.h"
#include "nvs_flash.h"

namespace {

static const char *kTag = "mesh_manager";

static mesh_manager_config_t s_cfg = {};
static bool s_running = false;
static bool s_mesh_connected = false;
static mesh_addr_t s_parent_addr = {};
static int s_mesh_layer = -1;
static esp_netif_t *s_netif_sta = NULL;
static TaskHandle_t s_tx_task = NULL;
static TaskHandle_t s_rx_task = NULL;

static const size_t kRxSize = 1500;
static const size_t kTxSize = 1460;
static uint8_t s_tx_buf[kTxSize] = {0};
static uint8_t s_rx_buf[kRxSize] = {0};

static mesh_light_ctl_t s_light_on = {
    .cmd = MESH_CONTROL_CMD,
    .on = 1,
    .token_id = MESH_TOKEN_ID,
    .token_value = MESH_TOKEN_VALUE,
};

static mesh_light_ctl_t s_light_off = {
    .cmd = MESH_CONTROL_CMD,
    .on = 0,
    .token_id = MESH_TOKEN_ID,
    .token_value = MESH_TOKEN_VALUE,
};

static int scan_router_channel(const char *ssid)
{
    if (!ssid || ssid[0] == '\0') {
        return 0;
    }

    wifi_scan_config_t scan_conf = {};
    scan_conf.ssid = 0;
    scan_conf.bssid = 0;
    scan_conf.channel = 0;
    scan_conf.show_hidden = true;
    scan_conf.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    scan_conf.scan_time.active.min = 100;
    scan_conf.scan_time.active.max = 300;

    if (esp_wifi_scan_start(&scan_conf, true) != ESP_OK) {
        return 0;
    }

    uint16_t ap_num = 0;
    if (esp_wifi_scan_get_ap_num(&ap_num) != ESP_OK || ap_num == 0) {
        return 0;
    }

    wifi_ap_record_t *ap_list = (wifi_ap_record_t *)malloc(sizeof(wifi_ap_record_t) * ap_num);
    if (!ap_list) {
        return 0;
    }

    if (esp_wifi_scan_get_ap_records(&ap_num, ap_list) != ESP_OK) {
        free(ap_list);
        return 0;
    }

    int channel = 0;
    for (int i = 0; i < ap_num; ++i) {
        if (strncmp((const char *)ap_list[i].ssid, ssid, sizeof(ap_list[i].ssid)) == 0) {
            channel = ap_list[i].primary;
            break;
        }
    }

    free(ap_list);
    return channel;
}

static void mesh_p2p_tx_task(void *arg)
{
    const uint16_t table_size = s_cfg.route_table_size > 0 ? s_cfg.route_table_size : 100;
    mesh_addr_t *route_table = (mesh_addr_t *)malloc(sizeof(mesh_addr_t) * table_size);
    if (!route_table) {
        ESP_LOGE(kTag, "route table alloc failed");
        vTaskDelete(NULL);
        return;
    }

    int send_count = 0;
    mesh_data_t data = {};
    data.data = s_tx_buf;
    data.size = sizeof(s_tx_buf);
    data.proto = MESH_PROTO_BIN;
    data.tos = MESH_TOS_P2P;

    while (s_running) {
        if (!esp_mesh_is_root()) {
            ESP_LOGI(kTag, "layer:%d, rtableSize:%d, %s",
                     s_mesh_layer,
                     esp_mesh_get_routing_table_size(),
                     s_mesh_connected ? "NODE" : "DISCONNECT");
            vTaskDelay(10 * 1000 / portTICK_PERIOD_MS);
            continue;
        }

        int route_table_size = 0;
        esp_mesh_get_routing_table(route_table,
                                   table_size * sizeof(mesh_addr_t),
                                   &route_table_size);
        if (send_count && !(send_count % 100)) {
            ESP_LOGI(kTag, "size:%d/%d,send_count:%d",
                     route_table_size,
                     esp_mesh_get_routing_table_size(),
                     send_count);
        }

        send_count++;
        s_tx_buf[25] = (send_count >> 24) & 0xff;
        s_tx_buf[24] = (send_count >> 16) & 0xff;
        s_tx_buf[23] = (send_count >> 8) & 0xff;
        s_tx_buf[22] = (send_count >> 0) & 0xff;

        if (send_count % 2) {
            memcpy(s_tx_buf, (uint8_t *)&s_light_on, sizeof(s_light_on));
        } else {
            memcpy(s_tx_buf, (uint8_t *)&s_light_off, sizeof(s_light_off));
        }

        for (int i = 0; i < route_table_size; i++) {
            esp_err_t err = esp_mesh_send(&route_table[i], &data, MESH_DATA_P2P, NULL, 0);
            if (err) {
                ESP_LOGE(kTag,
                         "[ROOT-2-UNICAST:%d][L:%d]parent:" MACSTR " to " MACSTR ", heap:%" PRId32 "[err:0x%x, proto:%d, tos:%d]",
                         send_count,
                         s_mesh_layer,
                         MAC2STR(s_parent_addr.addr),
                         MAC2STR(route_table[i].addr),
                         esp_get_minimum_free_heap_size(),
                         err,
                         data.proto,
                         data.tos);
            } else if (!(send_count % 100)) {
                ESP_LOGW(kTag,
                         "[ROOT-2-UNICAST:%d][L:%d][rtableSize:%d]parent:" MACSTR " to " MACSTR ", heap:%" PRId32 "[err:0x%x, proto:%d, tos:%d]",
                         send_count,
                         s_mesh_layer,
                         esp_mesh_get_routing_table_size(),
                         MAC2STR(s_parent_addr.addr),
                         MAC2STR(route_table[i].addr),
                         esp_get_minimum_free_heap_size(),
                         err,
                         data.proto,
                         data.tos);
            }
        }

        if (route_table_size < 10) {
            vTaskDelay(1 * 1000 / portTICK_PERIOD_MS);
        }
    }

    free(route_table);
    vTaskDelete(NULL);
}

static void mesh_p2p_rx_task(void *arg)
{
    int recv_count = 0;
    int send_count = 0;
    mesh_data_t data = {};
    int flag = 0;
    data.data = s_rx_buf;
    data.size = kRxSize;

    while (s_running) {
        data.size = kRxSize;
        mesh_addr_t from = {};
        esp_err_t err = esp_mesh_recv(&from, &data, portMAX_DELAY, &flag, NULL, 0);
        if (err != ESP_OK || !data.size) {
            ESP_LOGE(kTag, "err:0x%x, size:%d", err, data.size);
            continue;
        }

        if (data.size >= sizeof(send_count)) {
            send_count = (data.data[25] << 24) | (data.data[24] << 16)
                         | (data.data[23] << 8) | data.data[22];
        }

        recv_count++;

        if (s_cfg.rx_cb) {
            s_cfg.rx_cb(&from, data.data, data.size, s_cfg.rx_cb_ctx);
        } else if (s_cfg.enable_demo_p2p) {
            mesh_light_process(&from, data.data, data.size);
        }

        if (!(recv_count % 1)) {
            ESP_LOGW(kTag,
                     "[#RX:%d/%d][L:%d] parent:" MACSTR ", receive from " MACSTR ", size:%d, heap:%" PRId32 ", flag:%d[err:0x%x, proto:%d, tos:%d]",
                     recv_count,
                     send_count,
                     s_mesh_layer,
                     MAC2STR(s_parent_addr.addr),
                     MAC2STR(from.addr),
                     data.size,
                     esp_get_minimum_free_heap_size(),
                     flag,
                     err,
                     data.proto,
                     data.tos);
        }
    }

    vTaskDelete(NULL);
}

static void start_p2p_tasks()
{
    if (!s_cfg.enable_demo_p2p) {
        return;
    }
    if (!s_tx_task) {
        xTaskCreate(mesh_p2p_tx_task,
                    "MPTX",
                    s_cfg.tx_task_stack > 0 ? s_cfg.tx_task_stack : 6144,
                    NULL,
                    s_cfg.tx_task_prio > 0 ? s_cfg.tx_task_prio : 5,
                    &s_tx_task);
    }
    if (!s_rx_task) {
        xTaskCreate(mesh_p2p_rx_task,
                    "MPRX",
                    s_cfg.rx_task_stack > 0 ? s_cfg.rx_task_stack : 6144,
                    NULL,
                    s_cfg.rx_task_prio > 0 ? s_cfg.rx_task_prio : 5,
                    &s_rx_task);
    }
}

static void stop_p2p_tasks()
{
    if (s_tx_task) {
        vTaskDelete(s_tx_task);
        s_tx_task = NULL;
    }
    if (s_rx_task) {
        vTaskDelete(s_rx_task);
        s_rx_task = NULL;
    }
}

static void mesh_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    mesh_addr_t id = {};
    static uint16_t last_layer = 0;

    switch (event_id) {
    case MESH_EVENT_STARTED: {
        esp_mesh_get_id(&id);
        ESP_LOGI(kTag, "<MESH_EVENT_MESH_STARTED>ID:" MACSTR "", MAC2STR(id.addr));
        s_mesh_connected = false;
        s_mesh_layer = esp_mesh_get_layer();
    }
    break;
    case MESH_EVENT_STOPPED: {
        ESP_LOGI(kTag, "<MESH_EVENT_STOPPED>");
        s_mesh_connected = false;
        s_mesh_layer = esp_mesh_get_layer();
    }
    break;
    case MESH_EVENT_CHILD_CONNECTED: {
        mesh_event_child_connected_t *child_connected = (mesh_event_child_connected_t *)event_data;
        ESP_LOGI(kTag, "<MESH_EVENT_CHILD_CONNECTED>aid:%d, " MACSTR "",
                 child_connected->aid,
                 MAC2STR(child_connected->mac));
    }
    break;
    case MESH_EVENT_CHILD_DISCONNECTED: {
        mesh_event_child_disconnected_t *child_disconnected = (mesh_event_child_disconnected_t *)event_data;
        ESP_LOGI(kTag, "<MESH_EVENT_CHILD_DISCONNECTED>aid:%d, " MACSTR "",
                 child_disconnected->aid,
                 MAC2STR(child_disconnected->mac));
    }
    break;
    case MESH_EVENT_ROUTING_TABLE_ADD: {
        mesh_event_routing_table_change_t *routing_table = (mesh_event_routing_table_change_t *)event_data;
        ESP_LOGW(kTag, "<MESH_EVENT_ROUTING_TABLE_ADD>add %d, new:%d, layer:%d",
                 routing_table->rt_size_change,
                 routing_table->rt_size_new,
                 s_mesh_layer);
    }
    break;
    case MESH_EVENT_ROUTING_TABLE_REMOVE: {
        mesh_event_routing_table_change_t *routing_table = (mesh_event_routing_table_change_t *)event_data;
        ESP_LOGW(kTag, "<MESH_EVENT_ROUTING_TABLE_REMOVE>remove %d, new:%d, layer:%d",
                 routing_table->rt_size_change,
                 routing_table->rt_size_new,
                 s_mesh_layer);
    }
    break;
    case MESH_EVENT_NO_PARENT_FOUND: {
        mesh_event_no_parent_found_t *no_parent = (mesh_event_no_parent_found_t *)event_data;
        ESP_LOGI(kTag, "<MESH_EVENT_NO_PARENT_FOUND>scan times:%d", no_parent->scan_times);
    }
    break;
    case MESH_EVENT_PARENT_CONNECTED: {
        mesh_event_connected_t *connected = (mesh_event_connected_t *)event_data;
        esp_mesh_get_id(&id);
        s_mesh_layer = connected->self_layer;
        memcpy(&s_parent_addr.addr, connected->connected.bssid, 6);
        ESP_LOGI(kTag,
             "<MESH_EVENT_PARENT_CONNECTED>layer:%d-->%d, parent:" MACSTR "%s, ID:" MACSTR ", duty:%d",
                 last_layer,
                 s_mesh_layer,
                 MAC2STR(s_parent_addr.addr),
                 esp_mesh_is_root() ? "<ROOT>" :
                 (s_mesh_layer == 2) ? "<layer2>" : "",
                 MAC2STR(id.addr),
                 connected->duty);
        last_layer = s_mesh_layer;
        mesh_connected_indicator(s_mesh_layer);
        s_mesh_connected = true;
        if (esp_mesh_is_root()) {
            esp_netif_dhcpc_stop(s_netif_sta);
            esp_netif_dhcpc_start(s_netif_sta);
        }
        start_p2p_tasks();
    }
    break;
    case MESH_EVENT_PARENT_DISCONNECTED: {
        mesh_event_disconnected_t *disconnected = (mesh_event_disconnected_t *)event_data;
        ESP_LOGI(kTag, "<MESH_EVENT_PARENT_DISCONNECTED>reason:%d", disconnected->reason);
        s_mesh_connected = false;
        mesh_disconnected_indicator();
        s_mesh_layer = esp_mesh_get_layer();
        stop_p2p_tasks();
    }
    break;
    case MESH_EVENT_LAYER_CHANGE: {
        mesh_event_layer_change_t *layer_change = (mesh_event_layer_change_t *)event_data;
        s_mesh_layer = layer_change->new_layer;
        ESP_LOGI(kTag, "<MESH_EVENT_LAYER_CHANGE>layer:%d-->%d%s",
                 last_layer,
                 s_mesh_layer,
                 esp_mesh_is_root() ? "<ROOT>" :
                 (s_mesh_layer == 2) ? "<layer2>" : "");
        last_layer = s_mesh_layer;
        mesh_connected_indicator(s_mesh_layer);
    }
    break;
    case MESH_EVENT_ROOT_ADDRESS: {
        mesh_event_root_address_t *root_addr = (mesh_event_root_address_t *)event_data;
        ESP_LOGI(kTag, "<MESH_EVENT_ROOT_ADDRESS>root address:" MACSTR "", MAC2STR(root_addr->addr));
    }
    break;
    case MESH_EVENT_VOTE_STARTED: {
        mesh_event_vote_started_t *vote_started = (mesh_event_vote_started_t *)event_data;
        ESP_LOGI(kTag, "<MESH_EVENT_VOTE_STARTED>attempts:%d, reason:%d, rc_addr:" MACSTR "",
                 vote_started->attempts,
                 vote_started->reason,
                 MAC2STR(vote_started->rc_addr.addr));
    }
    break;
    case MESH_EVENT_VOTE_STOPPED: {
        ESP_LOGI(kTag, "<MESH_EVENT_VOTE_STOPPED>");
    }
    break;
    case MESH_EVENT_ROOT_SWITCH_REQ: {
        mesh_event_root_switch_req_t *switch_req = (mesh_event_root_switch_req_t *)event_data;
        ESP_LOGI(kTag, "<MESH_EVENT_ROOT_SWITCH_REQ>reason:%d, rc_addr:" MACSTR "",
                 switch_req->reason,
                 MAC2STR(switch_req->rc_addr.addr));
    }
    break;
    case MESH_EVENT_ROOT_SWITCH_ACK: {
        s_mesh_layer = esp_mesh_get_layer();
        esp_mesh_get_parent_bssid(&s_parent_addr);
        ESP_LOGI(kTag, "<MESH_EVENT_ROOT_SWITCH_ACK>layer:%d, parent:" MACSTR "",
                 s_mesh_layer,
                 MAC2STR(s_parent_addr.addr));
    }
    break;
    case MESH_EVENT_TODS_STATE: {
        mesh_event_toDS_state_t *toDs_state = (mesh_event_toDS_state_t *)event_data;
        ESP_LOGI(kTag, "<MESH_EVENT_TODS_REACHABLE>state:%d", *toDs_state);
    }
    break;
    case MESH_EVENT_ROOT_FIXED: {
        mesh_event_root_fixed_t *root_fixed = (mesh_event_root_fixed_t *)event_data;
        ESP_LOGI(kTag, "<MESH_EVENT_ROOT_FIXED>%s",
                 root_fixed->is_fixed ? "fixed" : "not fixed");
    }
    break;
    case MESH_EVENT_ROOT_ASKED_YIELD: {
        mesh_event_root_conflict_t *root_conflict = (mesh_event_root_conflict_t *)event_data;
        ESP_LOGI(kTag, "<MESH_EVENT_ROOT_ASKED_YIELD>" MACSTR ", rssi:%d, capacity:%d",
                 MAC2STR(root_conflict->addr),
                 root_conflict->rssi,
                 root_conflict->capacity);
    }
    break;
    case MESH_EVENT_CHANNEL_SWITCH: {
        mesh_event_channel_switch_t *channel_switch = (mesh_event_channel_switch_t *)event_data;
        ESP_LOGI(kTag, "<MESH_EVENT_CHANNEL_SWITCH>new channel:%d", channel_switch->channel);
    }
    break;
    case MESH_EVENT_SCAN_DONE: {
        mesh_event_scan_done_t *scan_done = (mesh_event_scan_done_t *)event_data;
        ESP_LOGI(kTag, "<MESH_EVENT_SCAN_DONE>number:%d", scan_done->number);
    }
    break;
    case MESH_EVENT_NETWORK_STATE: {
        mesh_event_network_state_t *network_state = (mesh_event_network_state_t *)event_data;
        ESP_LOGI(kTag, "<MESH_EVENT_NETWORK_STATE>is_rootless:%d", network_state->is_rootless);
    }
    break;
    case MESH_EVENT_STOP_RECONNECTION: {
        ESP_LOGI(kTag, "<MESH_EVENT_STOP_RECONNECTION>");
    }
    break;
    case MESH_EVENT_FIND_NETWORK: {
        mesh_event_find_network_t *find_network = (mesh_event_find_network_t *)event_data;
        ESP_LOGI(kTag, "<MESH_EVENT_FIND_NETWORK>new channel:%d, router BSSID:" MACSTR "",
                 find_network->channel,
                 MAC2STR(find_network->router_bssid));
    }
    break;
    case MESH_EVENT_ROUTER_SWITCH: {
        mesh_event_router_switch_t *router_switch = (mesh_event_router_switch_t *)event_data;
        ESP_LOGI(kTag, "<MESH_EVENT_ROUTER_SWITCH>new router:%s, channel:%d, " MACSTR "",
                 router_switch->ssid,
                 router_switch->channel,
                 MAC2STR(router_switch->bssid));
    }
    break;
    case MESH_EVENT_PS_PARENT_DUTY: {
        mesh_event_ps_duty_t *ps_duty = (mesh_event_ps_duty_t *)event_data;
        ESP_LOGI(kTag, "<MESH_EVENT_PS_PARENT_DUTY>duty:%d", ps_duty->duty);
    }
    break;
    case MESH_EVENT_PS_CHILD_DUTY: {
        mesh_event_ps_duty_t *ps_duty = (mesh_event_ps_duty_t *)event_data;
        ESP_LOGI(kTag, "<MESH_EVENT_PS_CHILD_DUTY>cidx:%d, " MACSTR ", duty:%d",
                 ps_duty->child_connected.aid - 1,
                 MAC2STR(ps_duty->child_connected.mac),
                 ps_duty->duty);
    }
    break;
    default:
        ESP_LOGI(kTag, "unknown id:%" PRId32 "", event_id);
        break;
    }
}

static void ip_event_handler(void *arg, esp_event_base_t event_base,
                             int32_t event_id, void *event_data)
{
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    ESP_LOGI(kTag, "<IP_EVENT_STA_GOT_IP>IP:" IPSTR, IP2STR(&event->ip_info.ip));
}

static esp_err_t init_nvs()
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

static esp_err_t init_netif()
{
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    if (!s_netif_sta) {
        err = esp_netif_create_default_wifi_mesh_netifs(&s_netif_sta, NULL);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            return err;
        }
    }

    return ESP_OK;
}

} // namespace

esp_err_t mesh_manager_start(const mesh_manager_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    s_cfg = *config;

    if (!s_cfg.router_ssid || s_cfg.router_ssid[0] == '\0') {
        ESP_LOGE(kTag, "router SSID is empty");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_ERROR_CHECK(init_nvs());
    ESP_ERROR_CHECK(init_netif());

    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &ip_event_handler, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));
    ESP_ERROR_CHECK(esp_wifi_start());

    int router_channel = 0;
    if (s_cfg.channel == 0) {
        router_channel = scan_router_channel(s_cfg.router_ssid);
        if (router_channel > 0) {
            ESP_LOGI(kTag, "router '%s' found on channel %d", s_cfg.router_ssid, router_channel);
        } else {
            ESP_LOGW(kTag, "router '%s' not found in scan; mesh will keep searching", s_cfg.router_ssid);
        }
    }

    ESP_ERROR_CHECK(esp_mesh_init());
    ESP_ERROR_CHECK(esp_event_handler_register(MESH_EVENT, ESP_EVENT_ANY_ID, &mesh_event_handler, NULL));
    ESP_ERROR_CHECK(esp_mesh_set_topology(s_cfg.topology));
    ESP_ERROR_CHECK(esp_mesh_set_max_layer(s_cfg.max_layer));
    ESP_ERROR_CHECK(esp_mesh_set_self_organized(true, true));
    ESP_ERROR_CHECK(esp_mesh_set_vote_percentage(1));
    ESP_ERROR_CHECK(esp_mesh_set_xon_qsize(128));

    if (s_cfg.enable_ps) {
        ESP_ERROR_CHECK(esp_mesh_enable_ps());
        ESP_ERROR_CHECK(esp_mesh_set_ap_assoc_expire(60));
        ESP_ERROR_CHECK(esp_mesh_set_announce_interval(600, 3300));
    } else {
        ESP_ERROR_CHECK(esp_mesh_disable_ps());
        ESP_ERROR_CHECK(esp_mesh_set_ap_assoc_expire(10));
    }

    mesh_cfg_t cfg = MESH_INIT_CONFIG_DEFAULT();
    memcpy((uint8_t *)&cfg.mesh_id, s_cfg.mesh_id, 6);

    cfg.channel = (s_cfg.channel == 0 && router_channel > 0) ? router_channel : s_cfg.channel;
    cfg.router.ssid_len = strlen(s_cfg.router_ssid);
    memcpy((uint8_t *)&cfg.router.ssid, s_cfg.router_ssid, cfg.router.ssid_len);
    if (s_cfg.router_pass) {
        memcpy((uint8_t *)&cfg.router.password, s_cfg.router_pass, strlen(s_cfg.router_pass));
    }

    ESP_ERROR_CHECK(esp_mesh_set_ap_authmode(s_cfg.ap_authmode));
    cfg.mesh_ap.max_connection = s_cfg.ap_connections;
    cfg.mesh_ap.nonmesh_max_connection = s_cfg.non_mesh_connections;
    if (s_cfg.mesh_ap_pass) {
        memcpy((uint8_t *)&cfg.mesh_ap.password, s_cfg.mesh_ap_pass, strlen(s_cfg.mesh_ap_pass));
    }

    ESP_ERROR_CHECK(esp_mesh_set_config(&cfg));
    ESP_ERROR_CHECK(esp_mesh_start());

    s_running = true;
    return ESP_OK;
}

esp_err_t mesh_manager_stop(void)
{
    s_running = false;
    stop_p2p_tasks();
    return esp_mesh_stop();
}

bool mesh_manager_is_root(void)
{
    return esp_mesh_is_root();
}

int mesh_manager_get_layer(void)
{
    return s_mesh_layer;
}

esp_err_t mesh_manager_send(const mesh_addr_t *to,
                            const uint8_t *data,
                            size_t len,
                            bool p2p)
{
    if (!to || !data || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    mesh_data_t mesh_data = {};
    mesh_data.data = (uint8_t *)data;
    mesh_data.size = len;
    mesh_data.proto = MESH_PROTO_BIN;
    mesh_data.tos = p2p ? MESH_TOS_P2P : MESH_TOS_DEF;

    return esp_mesh_send(to, &mesh_data, p2p ? MESH_DATA_P2P : MESH_DATA_FROMDS, NULL, 0);
}

esp_err_t mesh_manager_send_broadcast(const uint8_t *data, size_t len)
{
    if (!data || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    mesh_data_t mesh_data = {};
    mesh_data.data = (uint8_t *)data;
    mesh_data.size = len;
    mesh_data.proto = MESH_PROTO_BIN;
    mesh_data.tos = MESH_TOS_DEF;

    mesh_addr_t broadcast = {};
    memset(broadcast.addr, 0xFF, sizeof(broadcast.addr));
    return esp_mesh_send(&broadcast, &mesh_data, MESH_DATA_FROMDS, NULL, 0);
}
