#include "mesh_ota.h"

#include <errno.h>
#include <inttypes.h>
#include <string.h>

#include "esp_crc.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_mesh.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif

#include "mesh_manager.h"

#define MESH_OTA_MAGIC 0x4D4F5441U /* 'MOTA' */
#define MESH_OTA_MAX_DEVICE_ID 32
#define MESH_OTA_DEFAULT_CHUNK 1024
#define MESH_OTA_DEFAULT_STACK 8192
#define MESH_OTA_DEFAULT_PRIO 5
#define MESH_OTA_BOOT_ACK_RETRIES 10
#define MESH_OTA_HTTP_READ_RETRIES 20
#define MESH_OTA_HTTP_READ_DELAY_MS 250
#define MESH_OTA_HTTP_CONNECT_RETRIES 5
#define MESH_OTA_HTTP_CONNECT_DELAY_MS 500
#define MESH_OTA_HTTP_EAGAIN_RETRIES 10
#define MESH_OTA_WAIT_MESH_DELAY_MS 500
#define MESH_OTA_WAIT_IP_DELAY_MS 1000

typedef enum {
    MESH_OTA_MSG_BEGIN = 1,
    MESH_OTA_MSG_CHUNK = 2,
    MESH_OTA_MSG_END = 3,
    MESH_OTA_MSG_ACK = 4,
    MESH_OTA_MSG_BOOT_ACK = 5
} mesh_ota_msg_type_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t type;
    uint8_t reserved;
    uint16_t length;
} mesh_ota_msg_hdr_t;

typedef struct __attribute__((packed)) {
    int32_t fw_version;
    uint32_t total_size;
    uint16_t chunk_size;
    uint16_t reserved;
} mesh_ota_begin_payload_t;

typedef struct __attribute__((packed)) {
    uint32_t offset;
    uint16_t data_len;
    uint16_t seq;
} mesh_ota_chunk_payload_t;

typedef struct __attribute__((packed)) {
    uint32_t total_size;
    uint32_t crc32;
} mesh_ota_end_payload_t;

typedef struct __attribute__((packed)) {
    int32_t fw_version;
    uint8_t status;
    uint8_t reason;
} mesh_ota_ack_payload_t;

typedef struct __attribute__((packed)) {
    int32_t fw_version;
    uint8_t id_len;
    char device_id[MESH_OTA_MAX_DEVICE_ID];
} mesh_ota_boot_ack_payload_t;

typedef struct {
    bool in_progress;
    int incoming_version;
    uint32_t expected_size;
    uint32_t received;
    uint32_t crc32;
    esp_ota_handle_t handle;
    const esp_partition_t *partition;
} mesh_ota_state_t;

typedef struct {
    mesh_addr_t addr;
    bool acked;
    bool success;
} mesh_ota_node_ack_t;

static const char *kTag = "mesh_ota";

static mesh_ota_config_t s_cfg = {};
static mesh_ota_state_t s_state = {};
static TaskHandle_t s_task = NULL;
static bool s_initialized = false;

static void mesh_ota_task(void *arg);
static void mesh_ota_handle_rx(const mesh_addr_t *from, const uint8_t *data, size_t len);

static size_t mesh_ota_chunk_size()
{
    if (s_cfg.chunk_size >= 256 && s_cfg.chunk_size <= 1200) {
        return s_cfg.chunk_size;
    }
    return MESH_OTA_DEFAULT_CHUNK;
}

static void mesh_ota_wait_for_mesh(void)
{
    while (!mesh_manager_is_connected()) {
        vTaskDelay(MESH_OTA_WAIT_MESH_DELAY_MS / portTICK_PERIOD_MS);
    }
}

static void mesh_ota_wait_for_router_ip(void)
{
    while (!mesh_manager_is_router_connected()) {
        vTaskDelay(MESH_OTA_WAIT_IP_DELAY_MS / portTICK_PERIOD_MS);
    }
}

static esp_err_t mesh_ota_http_open(const char *url,
                                   uint32_t start_offset,
                                   esp_http_client_handle_t *out_client,
                                   int *out_content_length)
{
    if (!url || !out_client || !out_content_length) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 30000,
        .keep_alive_enable = false,
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
        .crt_bundle_attach = esp_crt_bundle_attach,
#endif
        .skip_cert_common_name_check = true,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return ESP_ERR_NO_MEM;
    }

    if (start_offset > 0) {
        char range[48] = {0};
        snprintf(range, sizeof(range), "bytes=%" PRIu32 "-", start_offset);
        esp_http_client_set_header(client, "Range", range);
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return err;
    }

    int content_length = esp_http_client_fetch_headers(client);
    if (content_length <= 0) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    int status = esp_http_client_get_status_code(client);
    if (status != 200 && status != 206) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    *out_client = client;
    *out_content_length = content_length;
    return ESP_OK;
}

static void mesh_ota_build_device_id(char *out, size_t out_len)
{
    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        snprintf(out, out_len, "mesh-%02x%02x%02x%02x%02x%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    } else {
        strncpy(out, "mesh-unknown", out_len - 1);
        out[out_len - 1] = '\0';
    }
}

static void mesh_ota_log_addr(const char *label, const mesh_addr_t *addr)
{
    if (!addr) {
        return;
    }
    ESP_LOGI(kTag, "%s %02x:%02x:%02x:%02x:%02x:%02x",
             label,
             addr->addr[0], addr->addr[1], addr->addr[2],
             addr->addr[3], addr->addr[4], addr->addr[5]);
}

static esp_err_t mesh_ota_send_msg(const mesh_addr_t *to,
                                  mesh_ota_msg_type_t type,
                                  const uint8_t *payload,
                                  uint16_t payload_len)
{
    if (!to) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t total_len = sizeof(mesh_ota_msg_hdr_t) + payload_len;
    uint8_t *buf = (uint8_t *)malloc(total_len);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }

    mesh_ota_msg_hdr_t hdr = {};
    hdr.magic = MESH_OTA_MAGIC;
    hdr.type = (uint8_t)type;
    hdr.length = payload_len;
    memcpy(buf, &hdr, sizeof(hdr));

    if (payload_len > 0 && payload) {
        memcpy(buf + sizeof(hdr), payload, payload_len);
    }

    esp_err_t err = mesh_manager_send(to, buf, total_len, true);
    free(buf);
    return err;
}

static esp_err_t mesh_ota_send_boot_ack_http(const char *device_id, int fw_version)
{
    if (!s_cfg.firebase_boot_ack_base_url || s_cfg.firebase_boot_ack_base_url[0] == '\0') {
        ESP_LOGW(kTag, "BOOT_ACK: base URL missing");
        return ESP_ERR_INVALID_ARG;
    }

    char url[256] = {0};
    snprintf(url, sizeof(url), "%s/%s.json", s_cfg.firebase_boot_ack_base_url, device_id);
    if (s_cfg.firebase_auth_token && s_cfg.firebase_auth_token[0] != '\0') {
        strncat(url, "?auth=", sizeof(url) - strlen(url) - 1);
        strncat(url, s_cfg.firebase_auth_token, sizeof(url) - strlen(url) - 1);
    }

    char body[256] = {0};
    snprintf(body, sizeof(body),
             "{\"device_id\":\"%s\",\"fw_version\":%d,\"state\":\"BOOT\",\"is_root\":%s}",
             device_id,
             fw_version,
             esp_mesh_is_root() ? "true" : "false");

    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 10000,
        .method = HTTP_METHOD_PUT,
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
        .crt_bundle_attach = esp_crt_bundle_attach,
#endif
        .skip_cert_common_name_check = true,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(kTag, "BOOT_ACK: http client init failed");
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        ESP_LOGI(kTag, "BOOT_ACK: sent (HTTP %d)", status);
    } else {
        ESP_LOGE(kTag, "BOOT_ACK: send failed (%d)", err);
    }

    esp_http_client_cleanup(client);
    return err;
}

static esp_err_t mesh_ota_send_boot_ack_mesh(void)
{
    mesh_addr_t root = {};
    esp_err_t err = mesh_manager_get_root_addr(&root);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "BOOT_ACK: root addr unavailable (%d)", err);
        return err;
    }

    char device_id[MESH_OTA_MAX_DEVICE_ID] = {0};
    if (s_cfg.device_id && s_cfg.device_id[0] != '\0') {
        strncpy(device_id, s_cfg.device_id, sizeof(device_id) - 1);
    } else {
        mesh_ota_build_device_id(device_id, sizeof(device_id));
    }

    mesh_ota_boot_ack_payload_t payload = {};
    payload.fw_version = s_cfg.current_version;
    payload.id_len = (uint8_t)strnlen(device_id, sizeof(payload.device_id));
    memcpy(payload.device_id, device_id, payload.id_len);

    return mesh_ota_send_msg(&root,
                             MESH_OTA_MSG_BOOT_ACK,
                             (const uint8_t *)&payload,
                             sizeof(payload.fw_version) + sizeof(payload.id_len) + payload.id_len);
}

static void mesh_ota_send_boot_ack(void)
{
    if (esp_mesh_is_root()) {
        if (!mesh_manager_is_router_connected()) {
            ESP_LOGW(kTag, "BOOT_ACK: waiting for router IP");
            mesh_ota_wait_for_router_ip();
        }
        char device_id[MESH_OTA_MAX_DEVICE_ID] = {0};
        if (s_cfg.device_id && s_cfg.device_id[0] != '\0') {
            strncpy(device_id, s_cfg.device_id, sizeof(device_id) - 1);
        } else {
            mesh_ota_build_device_id(device_id, sizeof(device_id));
        }
        mesh_ota_send_boot_ack_http(device_id, s_cfg.current_version);
    } else {
        for (int attempt = 0; attempt < MESH_OTA_BOOT_ACK_RETRIES; ++attempt) {
            if (mesh_ota_send_boot_ack_mesh() == ESP_OK) {
                return;
            }
            vTaskDelay(500 / portTICK_PERIOD_MS);
        }
        ESP_LOGW(kTag, "BOOT_ACK: mesh send failed after retries");
    }
}

static esp_err_t mesh_ota_fetch_version(int *out_version)
{
    if (!s_cfg.version_url || s_cfg.version_url[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    esp_http_client_config_t config = {
        .url = s_cfg.version_url,
        .timeout_ms = 15000,
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
        .crt_bundle_attach = esp_crt_bundle_attach,
#endif
        .skip_cert_common_name_check = true,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return err;
    }

    int status = esp_http_client_fetch_headers(client);
    if (status < 0) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    char buf[32] = {0};
    int read = esp_http_client_read(client, buf, sizeof(buf) - 1);
    if (read <= 0) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    buf[read] = '\0';
    int version = atoi(buf);

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    *out_version = version;
    return ESP_OK;
}

static esp_err_t mesh_ota_send_begin(const mesh_addr_t *nodes, int count, int new_version, uint32_t total_size)
{
    mesh_ota_begin_payload_t payload = {};
    payload.fw_version = new_version;
    payload.total_size = total_size;
    payload.chunk_size = (uint16_t)mesh_ota_chunk_size();

    for (int i = 0; i < count; ++i) {
        mesh_ota_send_msg(&nodes[i], MESH_OTA_MSG_BEGIN, (const uint8_t *)&payload, sizeof(payload));
        vTaskDelay(20 / portTICK_PERIOD_MS);
    }

    return ESP_OK;
}

static esp_err_t mesh_ota_send_end(const mesh_addr_t *nodes, int count, uint32_t total_size, uint32_t crc32)
{
    mesh_ota_end_payload_t payload = {};
    payload.total_size = total_size;
    payload.crc32 = crc32;

    for (int i = 0; i < count; ++i) {
        mesh_ota_send_msg(&nodes[i], MESH_OTA_MSG_END, (const uint8_t *)&payload, sizeof(payload));
        vTaskDelay(20 / portTICK_PERIOD_MS);
    }

    return ESP_OK;
}

static esp_err_t mesh_ota_stream_firmware(int new_version)
{
    if (!s_cfg.firmware_url || s_cfg.firmware_url[0] == '\0') {
        ESP_LOGE(kTag, "OTA: firmware URL missing");
        return ESP_ERR_INVALID_ARG;
    }

    esp_http_client_handle_t client = NULL;
    int content_length = 0;
    esp_err_t err = ESP_OK;
    for (int attempt = 0; attempt < MESH_OTA_HTTP_CONNECT_RETRIES; ++attempt) {
        err = mesh_ota_http_open(s_cfg.firmware_url, 0, &client, &content_length);
        if (err == ESP_OK) {
            break;
        }
        vTaskDelay(MESH_OTA_HTTP_CONNECT_DELAY_MS / portTICK_PERIOD_MS);
    }
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "OTA: open failed (%d)", err);
        return err;
    }

    const esp_partition_t *partition = esp_ota_get_next_update_partition(NULL);
    if (!partition) {
        ESP_LOGE(kTag, "OTA: no update partition");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    esp_ota_handle_t ota_handle = 0;
    err = esp_ota_begin(partition, content_length, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "OTA: ota begin failed (%d)", err);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return err;
    }

    int table_size = esp_mesh_get_routing_table_size();
    if (table_size <= 0) {
        table_size = 1;
    }

    mesh_addr_t *nodes = (mesh_addr_t *)malloc(sizeof(mesh_addr_t) * table_size);
    int node_count = 0;
    if (nodes) {
        esp_mesh_get_routing_table(nodes, sizeof(mesh_addr_t) * table_size, &node_count);
    }

    ESP_LOGI(kTag, "OTA: streaming to %d nodes", node_count);

    mesh_ota_send_begin(nodes, node_count, new_version, (uint32_t)content_length);

    size_t chunk_size = mesh_ota_chunk_size();
    size_t buf_size = sizeof(mesh_ota_msg_hdr_t) + sizeof(mesh_ota_chunk_payload_t) + chunk_size;
    uint8_t *tx_buf = (uint8_t *)malloc(buf_size);
    if (!tx_buf) {
        esp_ota_abort(ota_handle);
        if (nodes) {
            free(nodes);
        }
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    mesh_ota_msg_hdr_t *hdr = (mesh_ota_msg_hdr_t *)tx_buf;
    hdr->magic = MESH_OTA_MAGIC;
    hdr->type = MESH_OTA_MSG_CHUNK;

    mesh_ota_chunk_payload_t *chunk = (mesh_ota_chunk_payload_t *)(tx_buf + sizeof(mesh_ota_msg_hdr_t));
    uint8_t *chunk_data = tx_buf + sizeof(mesh_ota_msg_hdr_t) + sizeof(mesh_ota_chunk_payload_t);

    uint32_t crc32 = 0;
    uint32_t offset = 0;
    uint16_t seq = 0;
    uint32_t total_length = (uint32_t)content_length;
    while (offset < total_length) {
        int to_read = (int)chunk_size;
        if (offset + (uint32_t)to_read > total_length) {
            to_read = (int)(total_length - offset);
        }

        if (!client) {
            err = mesh_ota_http_open(s_cfg.firmware_url, offset, &client, &content_length);
            if (err != ESP_OK) {
                ESP_LOGE(kTag, "OTA: reconnect failed (%d)", err);
                break;
            }
        }

        int read = esp_http_client_read(client, (char *)chunk_data, to_read);
        if (read <= 0) {
            bool eagain_recovered = false;
            for (int wait = 0; wait < MESH_OTA_HTTP_EAGAIN_RETRIES; ++wait) {
                if (errno == EAGAIN) {
                    vTaskDelay(MESH_OTA_HTTP_READ_DELAY_MS / portTICK_PERIOD_MS);
                    read = esp_http_client_read(client, (char *)chunk_data, to_read);
                    if (read > 0) {
                        eagain_recovered = true;
                        break;
                    }
                } else {
                    break;
                }
            }
            if (eagain_recovered) {
                goto ota_chunk_ok;
            }

            bool recovered = false;
            for (int retry = 0; retry < MESH_OTA_HTTP_READ_RETRIES; ++retry) {
                if (client) {
                    esp_http_client_close(client);
                    esp_http_client_cleanup(client);
                    client = NULL;
                }

                if (!mesh_manager_is_router_connected()) {
                    mesh_ota_wait_for_router_ip();
                }

                vTaskDelay(MESH_OTA_HTTP_READ_DELAY_MS / portTICK_PERIOD_MS);

                int remaining = 0;
                err = mesh_ota_http_open(s_cfg.firmware_url, offset, &client, &remaining);
                if (err == ESP_OK) {
                    read = esp_http_client_read(client, (char *)chunk_data, to_read);
                    if (read > 0) {
                        recovered = true;
                        break;
                    }
                }
            }
            if (!recovered) {
                ESP_LOGE(kTag, "OTA: read failed at offset %u", offset);
                break;
            }
        }

ota_chunk_ok:

        err = esp_ota_write(ota_handle, chunk_data, read);
        if (err != ESP_OK) {
            ESP_LOGE(kTag, "OTA: ota write failed (%d)", err);
            break;
        }

        crc32 = esp_crc32_le(crc32, chunk_data, read);

        chunk->offset = offset;
        chunk->data_len = (uint16_t)read;
        chunk->seq = seq++;
        hdr->length = sizeof(mesh_ota_chunk_payload_t) + (uint16_t)read;

        size_t send_len = sizeof(mesh_ota_msg_hdr_t) + hdr->length;
        for (int i = 0; i < node_count; ++i) {
            mesh_manager_send(&nodes[i], tx_buf, send_len, true);
        }

        offset += read;
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }

    free(tx_buf);

    bool success = (offset == total_length);
    if (success) {
        err = esp_ota_end(ota_handle);
        if (err == ESP_OK) {
            err = esp_ota_set_boot_partition(partition);
            success = (err == ESP_OK);
        } else {
            success = false;
        }
    } else {
        esp_ota_abort(ota_handle);
    }

    mesh_ota_send_end(nodes, node_count, total_length, crc32);

    if (nodes) {
        free(nodes);
    }

    if (client) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }

    if (success) {
        ESP_LOGI(kTag, "OTA: root update prepared");
        return ESP_OK;
    }

    ESP_LOGE(kTag, "OTA: root update failed");
    return ESP_FAIL;
}

static void mesh_ota_handle_begin(const mesh_addr_t *from, const uint8_t *payload, size_t len)
{
    if (len < sizeof(mesh_ota_begin_payload_t)) {
        return;
    }

    const mesh_ota_begin_payload_t *begin = (const mesh_ota_begin_payload_t *)payload;
    if (begin->fw_version <= s_cfg.current_version) {
        ESP_LOGI(kTag, "OTA: skip version %d", begin->fw_version);
        return;
    }

    if (s_state.in_progress) {
        ESP_LOGW(kTag, "OTA: already in progress");
        return;
    }

    s_state.partition = esp_ota_get_next_update_partition(NULL);
    if (!s_state.partition) {
        ESP_LOGE(kTag, "OTA: no update partition");
        return;
    }

    esp_err_t err = esp_ota_begin(s_state.partition, begin->total_size, &s_state.handle);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "OTA: ota begin failed (%d)", err);
        return;
    }

    s_state.in_progress = true;
    s_state.incoming_version = begin->fw_version;
    s_state.expected_size = begin->total_size;
    s_state.received = 0;
    s_state.crc32 = 0;

    mesh_ota_log_addr("OTA: begin from", from);
}

static void mesh_ota_send_ack(const mesh_addr_t *to, uint8_t status, uint8_t reason, int version)
{
    mesh_ota_ack_payload_t payload = {};
    payload.fw_version = version;
    payload.status = status;
    payload.reason = reason;
    mesh_ota_send_msg(to, MESH_OTA_MSG_ACK, (const uint8_t *)&payload, sizeof(payload));
}

static void mesh_ota_handle_chunk(const mesh_addr_t *from, const uint8_t *payload, size_t len)
{
    if (!s_state.in_progress) {
        return;
    }

    if (len < sizeof(mesh_ota_chunk_payload_t)) {
        return;
    }

    const mesh_ota_chunk_payload_t *chunk = (const mesh_ota_chunk_payload_t *)payload;
    const uint8_t *data = payload + sizeof(mesh_ota_chunk_payload_t);
    size_t data_len = chunk->data_len;
    if (sizeof(mesh_ota_chunk_payload_t) + data_len > len) {
        return;
    }

    esp_err_t err = esp_ota_write(s_state.handle, data, data_len);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "OTA: ota write failed (%d)", err);
        mesh_ota_send_ack(from, 0, 1, s_state.incoming_version);
        esp_ota_abort(s_state.handle);
        s_state.in_progress = false;
        return;
    }

    s_state.received += data_len;
    s_state.crc32 = esp_crc32_le(s_state.crc32, data, data_len);
}

static void mesh_ota_handle_end(const mesh_addr_t *from, const uint8_t *payload, size_t len)
{
    if (!s_state.in_progress || len < sizeof(mesh_ota_end_payload_t)) {
        return;
    }

    const mesh_ota_end_payload_t *end = (const mesh_ota_end_payload_t *)payload;
    if (s_state.received != s_state.expected_size) {
        ESP_LOGE(kTag, "OTA: size mismatch %u/%u", s_state.received, s_state.expected_size);
        mesh_ota_send_ack(from, 0, 2, s_state.incoming_version);
        esp_ota_abort(s_state.handle);
        s_state.in_progress = false;
        return;
    }

    if (end->crc32 != 0 && end->crc32 != s_state.crc32) {
        ESP_LOGE(kTag, "OTA: crc mismatch");
        mesh_ota_send_ack(from, 0, 3, s_state.incoming_version);
        esp_ota_abort(s_state.handle);
        s_state.in_progress = false;
        return;
    }

    esp_err_t err = esp_ota_end(s_state.handle);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "OTA: ota end failed (%d)", err);
        mesh_ota_send_ack(from, 0, 4, s_state.incoming_version);
        s_state.in_progress = false;
        return;
    }

    err = esp_ota_set_boot_partition(s_state.partition);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "OTA: set boot partition failed (%d)", err);
        mesh_ota_send_ack(from, 0, 5, s_state.incoming_version);
        s_state.in_progress = false;
        return;
    }

    mesh_ota_send_ack(from, 1, 0, s_state.incoming_version);
    ESP_LOGI(kTag, "OTA: update ready, rebooting");
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    esp_restart();
}

static void mesh_ota_handle_ack(const mesh_addr_t *from, const uint8_t *payload, size_t len)
{
    if (len < sizeof(mesh_ota_ack_payload_t)) {
        return;
    }

    const mesh_ota_ack_payload_t *ack = (const mesh_ota_ack_payload_t *)payload;
    ESP_LOGI(kTag, "OTA: ack from %02x:%02x:%02x:%02x:%02x:%02x ver=%d status=%d reason=%d",
             from->addr[0], from->addr[1], from->addr[2],
             from->addr[3], from->addr[4], from->addr[5],
             ack->fw_version, ack->status, ack->reason);
}

static void mesh_ota_handle_boot_ack(const mesh_addr_t *from, const uint8_t *payload, size_t len)
{
    if (len < sizeof(int32_t) + sizeof(uint8_t)) {
        return;
    }

    const mesh_ota_boot_ack_payload_t *ack = (const mesh_ota_boot_ack_payload_t *)payload;
    uint8_t id_len = ack->id_len;
    if (id_len >= MESH_OTA_MAX_DEVICE_ID) {
        return;
    }

    char device_id[MESH_OTA_MAX_DEVICE_ID] = {0};
    memcpy(device_id, ack->device_id, id_len);
    device_id[id_len] = '\0';

    ESP_LOGI(kTag, "BOOT_ACK: from %02x:%02x:%02x:%02x:%02x:%02x id=%s ver=%d",
             from->addr[0], from->addr[1], from->addr[2],
             from->addr[3], from->addr[4], from->addr[5],
             device_id, ack->fw_version);

    if (esp_mesh_is_root()) {
        mesh_ota_send_boot_ack_http(device_id, ack->fw_version);
    }
}

static void mesh_ota_handle_rx(const mesh_addr_t *from, const uint8_t *data, size_t len)
{
    if (!from || !data || len < sizeof(mesh_ota_msg_hdr_t)) {
        return;
    }

    const mesh_ota_msg_hdr_t *hdr = (const mesh_ota_msg_hdr_t *)data;
    if (hdr->magic != MESH_OTA_MAGIC) {
        return;
    }

    if (sizeof(mesh_ota_msg_hdr_t) + hdr->length > len) {
        return;
    }

    const uint8_t *payload = data + sizeof(mesh_ota_msg_hdr_t);
    size_t payload_len = hdr->length;

    switch (hdr->type) {
    case MESH_OTA_MSG_BEGIN:
        mesh_ota_handle_begin(from, payload, payload_len);
        break;
    case MESH_OTA_MSG_CHUNK:
        mesh_ota_handle_chunk(from, payload, payload_len);
        break;
    case MESH_OTA_MSG_END:
        mesh_ota_handle_end(from, payload, payload_len);
        break;
    case MESH_OTA_MSG_ACK:
        mesh_ota_handle_ack(from, payload, payload_len);
        break;
    case MESH_OTA_MSG_BOOT_ACK:
        mesh_ota_handle_boot_ack(from, payload, payload_len);
        break;
    default:
        break;
    }
}

static void mesh_ota_task(void *arg)
{
    while (esp_mesh_get_layer() <= 0) {
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }

    mesh_ota_wait_for_mesh();

    mesh_ota_send_boot_ack();

    if (!esp_mesh_is_root()) {
        vTaskDelete(NULL);
        return;
    }

    mesh_ota_wait_for_router_ip();

    int latest = s_cfg.current_version;
    esp_err_t err = mesh_ota_fetch_version(&latest);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "OTA: version check failed (%d)", err);
        vTaskDelete(NULL);
        return;
    }

    if (latest <= s_cfg.current_version) {
        ESP_LOGI(kTag, "OTA: up to date (%d)", latest);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(kTag, "OTA: update available %d -> %d", s_cfg.current_version, latest);
    err = mesh_ota_stream_firmware(latest);
    if (err == ESP_OK) {
        ESP_LOGI(kTag, "OTA: mesh update sent, rebooting root");
        vTaskDelay(2000 / portTICK_PERIOD_MS);
        esp_restart();
    }

    vTaskDelete(NULL);
}

esp_err_t mesh_ota_init(const mesh_ota_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    s_cfg = *config;
    s_initialized = true;
    return ESP_OK;
}

esp_err_t mesh_ota_start(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_task) {
        return ESP_OK;
    }

    size_t stack = s_cfg.task_stack > 0 ? s_cfg.task_stack : MESH_OTA_DEFAULT_STACK;
    int prio = s_cfg.task_prio > 0 ? s_cfg.task_prio : MESH_OTA_DEFAULT_PRIO;

    if (xTaskCreate(mesh_ota_task, "mesh_ota", stack, NULL, prio, &s_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

void mesh_ota_rx_cb(const mesh_addr_t *from,
                    const uint8_t *data,
                    size_t len,
                    void *ctx)
{
    (void)ctx;
    mesh_ota_handle_rx(from, data, len);
}
