#include "mesh_ota.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_crc.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mesh.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"

#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif

#include "mesh_manager.h"
#include "ui_engine.h"

#define MESH_OTA_MAGIC 0x4D4F5441U
#define MESH_OTA_MAX_DEVICE_ID 32
#define MESH_OTA_DEFAULT_CHUNK 1024
#define MESH_OTA_DEFAULT_STACK 8192
#define MESH_OTA_DEFAULT_PRIO 5
#define MESH_OTA_POLL_INTERVAL_MS 10000

typedef enum {
    MESH_OTA_MSG_BEGIN = 1,
    MESH_OTA_MSG_CHUNK = 2,
    MESH_OTA_MSG_END = 3,
    MESH_OTA_MSG_ACK = 4
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

typedef struct {
    bool in_progress;
    int incoming_version;
    uint32_t expected_size;
    uint32_t received;
    uint32_t crc32;
    esp_ota_handle_t handle;
    const esp_partition_t *partition;
} mesh_ota_state_t;

static const char *kTag = "mesh_ota";

static mesh_ota_config_t s_cfg = {};
static mesh_ota_state_t s_state = {};
static TaskHandle_t s_task = NULL;
static SemaphoreHandle_t s_state_lock = NULL;
static bool s_initialized = false;
static bool s_mesh_ready = false;
static bool s_net_ready = false;
static bool s_is_root = false;

static size_t mesh_ota_chunk_size(void)
{
    if (s_cfg.chunk_size >= 256 && s_cfg.chunk_size <= 1200) {
        return s_cfg.chunk_size;
    }
    return MESH_OTA_DEFAULT_CHUNK;
}

static void mesh_ota_lock(void)
{
    if (s_state_lock) {
        xSemaphoreTake(s_state_lock, pdMS_TO_TICKS(200));
    }
}

static void mesh_ota_unlock(void)
{
    if (s_state_lock) {
        xSemaphoreGive(s_state_lock);
    }
}

static bool mesh_ota_is_in_progress(void)
{
    bool in_progress = false;
    mesh_ota_lock();
    in_progress = s_state.in_progress;
    mesh_ota_unlock();
    return in_progress;
}

static void mesh_ota_sha256_hex(const uint8_t *hash, char *out, size_t out_len)
{
    static const char kHex[] = "0123456789abcdef";
    if (out_len < 65) {
        return;
    }

    for (int i = 0; i < 32; ++i) {
        out[i * 2] = kHex[(hash[i] >> 4) & 0xF];
        out[i * 2 + 1] = kHex[hash[i] & 0xF];
    }
    out[64] = '\0';
}

static bool mesh_ota_verify_hash(const uint8_t *hash)
{
    if (!s_cfg.expected_sha256 || s_cfg.expected_sha256[0] == '\0') {
        ESP_LOGW(kTag, "OTA: SHA256 not provided, skipping verification");
        return true;
    }

    char hex[65] = {0};
    mesh_ota_sha256_hex(hash, hex, sizeof(hex));
    return strcasecmp(hex, s_cfg.expected_sha256) == 0;
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

static void mesh_ota_send_ack(const mesh_addr_t *to, uint8_t status, uint8_t reason, int version)
{
    mesh_ota_ack_payload_t payload = {};
    payload.fw_version = version;
    payload.status = status;
    payload.reason = reason;
    mesh_ota_send_msg(to, MESH_OTA_MSG_ACK, (const uint8_t *)&payload, sizeof(payload));
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

static int mesh_ota_read_with_retry(esp_http_client_handle_t client, uint8_t *buf, int len)
{
    for (int attempt = 0; attempt < 3; ++attempt) {
        int read = esp_http_client_read(client, (char *)buf, len);
        if (read > 0) {
            return read;
        }
        if (read == 0 && esp_http_client_is_complete_data_received(client)) {
            return 0;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return -1;
}

static esp_err_t mesh_ota_stream_and_broadcast(int new_version)
{
    esp_http_client_config_t config = {
        .url = s_cfg.firmware_url,
        .timeout_ms = 30000,
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

    int content_length = esp_http_client_fetch_headers(client);
    if (content_length <= 0) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    const esp_partition_t *partition = esp_ota_get_next_update_partition(NULL);
    if (!partition) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    esp_ota_handle_t ota_handle = 0;
    err = esp_ota_begin(partition, content_length, &ota_handle);
    if (err != ESP_OK) {
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

    mesh_ota_begin_payload_t begin = {};
    begin.fw_version = new_version;
    begin.total_size = (uint32_t)content_length;
    begin.chunk_size = (uint16_t)mesh_ota_chunk_size();
    for (int i = 0; i < node_count; ++i) {
        mesh_ota_send_msg(&nodes[i], MESH_OTA_MSG_BEGIN, (const uint8_t *)&begin, sizeof(begin));
    }

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

    uint8_t sha_hash[32] = {0};
    mbedtls_sha256_context sha_ctx;
    mbedtls_sha256_init(&sha_ctx);
    mbedtls_sha256_starts(&sha_ctx, 0);

    uint32_t crc32 = 0;
    uint32_t offset = 0;
    uint16_t seq = 0;
    uint8_t last_pct = 0;
    while (offset < (uint32_t)content_length) {
        int to_read = (int)chunk_size;
        if (offset + (uint32_t)to_read > (uint32_t)content_length) {
            to_read = (int)((uint32_t)content_length - offset);
        }

        int read = mesh_ota_read_with_retry(client, chunk_data, to_read);
        if (read <= 0) {
            ESP_LOGE(kTag, "OTA: read failed at offset %" PRIu32, offset);
            break;
        }

        err = esp_ota_write(ota_handle, chunk_data, read);
        if (err != ESP_OK) {
            ESP_LOGE(kTag, "OTA: ota write failed (%d)", err);
            break;
        }

        mbedtls_sha256_update(&sha_ctx, chunk_data, read);
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
        uint8_t pct = (uint8_t)((offset * 100U) / (uint32_t)content_length);
        if (pct >= last_pct + 10 || pct == 100) {
            char status[32] = {0};
            snprintf(status, sizeof(status), "OTA TX %u%%", (unsigned)pct);
            ui_engine_set_ota_status(status);
            last_pct = pct;
        }
        vTaskDelay(5 / portTICK_PERIOD_MS);
    }

    mbedtls_sha256_finish(&sha_ctx, sha_hash);
    mbedtls_sha256_free(&sha_ctx);

    free(tx_buf);

    bool success = (offset == (uint32_t)content_length) && mesh_ota_verify_hash(sha_hash);
    if (success) {
        err = esp_ota_end(ota_handle);
        if (err == ESP_OK) {
            err = esp_ota_set_boot_partition(partition);
            success = (err == ESP_OK);
        }
    } else {
        esp_ota_abort(ota_handle);
        ui_engine_set_ota_status("OTA TX FAIL");
    }

    mesh_ota_end_payload_t end = {};
    if (success) {
        end.total_size = (uint32_t)content_length;
        end.crc32 = crc32;
    }
    for (int i = 0; i < node_count; ++i) {
        mesh_ota_send_msg(&nodes[i], MESH_OTA_MSG_END, (const uint8_t *)&end, sizeof(end));
    }

    if (nodes) {
        free(nodes);
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    return success ? ESP_OK : ESP_FAIL;
}

static void mesh_ota_handle_begin(const mesh_addr_t *from, const uint8_t *payload, size_t len)
{
    if (len < sizeof(mesh_ota_begin_payload_t)) {
        return;
    }

    const mesh_ota_begin_payload_t *begin = (const mesh_ota_begin_payload_t *)payload;
    if (begin->fw_version <= s_cfg.current_version || begin->total_size == 0 || begin->chunk_size == 0) {
        return;
    }
    mesh_ota_lock();
    if (s_state.in_progress) {
        mesh_ota_unlock();
        return;
    }

    s_state.partition = esp_ota_get_next_update_partition(NULL);
    if (!s_state.partition) {
        mesh_ota_unlock();
        return;
    }

    esp_err_t err = esp_ota_begin(s_state.partition, begin->total_size, &s_state.handle);
    if (err != ESP_OK) {
        mesh_ota_unlock();
        return;
    }

    s_state.in_progress = true;
    s_state.incoming_version = begin->fw_version;
    s_state.expected_size = begin->total_size;
    s_state.received = 0;
    s_state.crc32 = 0;
    mesh_ota_unlock();
    ui_engine_set_ota_status("OTA RX 0%");

    (void)from;
}

static void mesh_ota_handle_chunk(const mesh_addr_t *from, const uint8_t *payload, size_t len)
{
    if (len < sizeof(mesh_ota_chunk_payload_t)) {
        return;
    }

    mesh_ota_lock();
    if (!s_state.in_progress) {
        mesh_ota_unlock();
        return;
    }

    const mesh_ota_chunk_payload_t *chunk = (const mesh_ota_chunk_payload_t *)payload;
    const uint8_t *data = payload + sizeof(mesh_ota_chunk_payload_t);
    size_t data_len = chunk->data_len;
    if (sizeof(mesh_ota_chunk_payload_t) + data_len > len) {
        mesh_ota_unlock();
        return;
    }

    if (chunk->offset != s_state.received) {
        mesh_ota_send_ack(from, 0, 7, s_state.incoming_version);
        esp_ota_abort(s_state.handle);
        s_state.in_progress = false;
        mesh_ota_unlock();
        ui_engine_set_ota_status("OTA OFFSET FAIL");
        return;
    }

    esp_err_t err = esp_ota_write(s_state.handle, data, data_len);
    if (err != ESP_OK) {
        mesh_ota_send_ack(from, 0, 1, s_state.incoming_version);
        esp_ota_abort(s_state.handle);
        s_state.in_progress = false;
        mesh_ota_unlock();
        return;
    }

    s_state.received += data_len;
    s_state.crc32 = esp_crc32_le(s_state.crc32, data, data_len);
    if (s_state.expected_size > 0) {
        uint8_t pct = (uint8_t)((s_state.received * 100U) / s_state.expected_size);
        if (pct == 100 || (chunk->seq % 32) == 0) {
            char status[32] = {0};
            snprintf(status, sizeof(status), "OTA RX %u%%", (unsigned)pct);
            ui_engine_set_ota_status(status);
        }
    }
    mesh_ota_unlock();
}

static void mesh_ota_handle_end(const mesh_addr_t *from, const uint8_t *payload, size_t len)
{
    if (len < sizeof(mesh_ota_end_payload_t)) {
        return;
    }

    mesh_ota_lock();
    if (!s_state.in_progress) {
        mesh_ota_unlock();
        return;
    }

    const mesh_ota_end_payload_t *end = (const mesh_ota_end_payload_t *)payload;
    if (end->total_size == 0) {
        mesh_ota_send_ack(from, 0, 6, s_state.incoming_version);
        esp_ota_abort(s_state.handle);
        s_state.in_progress = false;
        mesh_ota_unlock();
        ui_engine_set_ota_status("OTA ABORT");
        return;
    }

    if (s_state.received != s_state.expected_size) {
        mesh_ota_send_ack(from, 0, 2, s_state.incoming_version);
        esp_ota_abort(s_state.handle);
        s_state.in_progress = false;
        mesh_ota_unlock();
        ui_engine_set_ota_status("OTA RX FAIL");
        return;
    }

    if (end->crc32 != 0 && end->crc32 != s_state.crc32) {
        mesh_ota_send_ack(from, 0, 3, s_state.incoming_version);
        esp_ota_abort(s_state.handle);
        s_state.in_progress = false;
        mesh_ota_unlock();
        ui_engine_set_ota_status("OTA CRC FAIL");
        return;
    }

    esp_err_t err = esp_ota_end(s_state.handle);
    if (err != ESP_OK) {
        mesh_ota_send_ack(from, 0, 4, s_state.incoming_version);
        s_state.in_progress = false;
        mesh_ota_unlock();
        ui_engine_set_ota_status("OTA END FAIL");
        return;
    }

    err = esp_ota_set_boot_partition(s_state.partition);
    if (err != ESP_OK) {
        mesh_ota_send_ack(from, 0, 5, s_state.incoming_version);
        s_state.in_progress = false;
        mesh_ota_unlock();
        ui_engine_set_ota_status("OTA BOOT FAIL");
        return;
    }

    mesh_ota_send_ack(from, 1, 0, s_state.incoming_version);
    mesh_ota_unlock();
    ui_engine_set_ota_status("OTA RESTART");
    vTaskDelay(500 / portTICK_PERIOD_MS);
    esp_restart();
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
    default:
        break;
    }
}

static void mesh_ota_task(void *arg)
{
    (void)arg;
    ui_engine_set_ota_status("OTA WAITING");

    while (true) {
        if (!s_mesh_ready || !s_is_root) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if (!s_net_ready || !mesh_manager_is_router_connected()) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if (mesh_ota_is_in_progress()) {
            vTaskDelay(pdMS_TO_TICKS(MESH_OTA_POLL_INTERVAL_MS));
            continue;
        }

        int latest = s_cfg.current_version;
        ui_engine_set_ota_status("OTA CHECKING");
        esp_err_t err = mesh_ota_fetch_version(&latest);
        if (err == ESP_OK) {
            if (latest > s_cfg.current_version) {
                ESP_LOGI(kTag, "OTA: version %d available, current %d", latest, s_cfg.current_version);
                ui_engine_set_ota_status("OTA UPDATE");
                if (mesh_ota_stream_and_broadcast(latest) == ESP_OK) {
                    ui_engine_set_ota_status("OTA RESTART");
                    vTaskDelay(pdMS_TO_TICKS(1000));
                    esp_restart();
                }
                ui_engine_set_ota_status("OTA RETRY 10S");
            } else {
                ui_engine_set_ota_status("OTA UP TO DATE");
            }
        } else {
            ESP_LOGW(kTag, "OTA: version check failed (%s)", esp_err_to_name(err));
            ui_engine_set_ota_status("OTA CHECK FAIL");
        }

        vTaskDelay(pdMS_TO_TICKS(MESH_OTA_POLL_INTERVAL_MS));
    }
}

esp_err_t mesh_ota_init(const mesh_ota_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    s_cfg = *config;
    if (!s_state_lock) {
        s_state_lock = xSemaphoreCreateMutex();
        if (!s_state_lock) {
            return ESP_ERR_NO_MEM;
        }
    }
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

    if (xTaskCreatePinnedToCore(mesh_ota_task, "mesh_ota", stack, NULL, prio, &s_task, 0) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

void mesh_ota_notify_mesh_ready(bool is_root)
{
    s_mesh_ready = true;
    s_is_root = is_root;
}

void mesh_ota_notify_net_ready(void)
{
    s_net_ready = true;
}

void mesh_ota_mark_running_valid(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running) {
        return;
    }

    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    if (esp_ota_get_state_partition(running, &state) == ESP_OK) {
        if (state == ESP_OTA_IMG_PENDING_VERIFY) {
            esp_ota_mark_app_valid_cancel_rollback();
        }
    }
}

void mesh_ota_rx_cb(const mesh_addr_t *from,
                    const uint8_t *data,
                    size_t len,
                    void *ctx)
{
    (void)ctx;
    mesh_ota_handle_rx(from, data, len);
}
