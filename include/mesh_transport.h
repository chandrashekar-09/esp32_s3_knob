#ifndef MESH_TRANSPORT_H
#define MESH_TRANSPORT_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "phase_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/* QueSort ESP-NOW mesh transport.
 *
 * Wire format (all little-endian, packed):
 *
 *   struct mesh_frame_hdr_t {
 *       uint32_t magic;          // 'QSRT' = 0x54525351
 *       uint8_t  version;        // MESH_PROTO_VERSION
 *       uint8_t  type;           // mesh_msg_type_t
 *       uint8_t  origin_mac[6];  // sender's STA MAC
 *       uint16_t seq;            // monotonic per-origin
 *       uint8_t  ttl;            // hops remaining; decremented per rebroadcast
 *   };  // 15 bytes
 *
 * Followed by a type-specific payload. Total stays under the
 * ESP-NOW 250-byte limit by a wide margin (largest payload today
 * is ~6 bytes).
 *
 * Dedup: (origin_mac, seq) cached in RAM; duplicates dropped. No
 * rebroadcast yet — single-hop only for now. Multi-hop arrives
 * when ttl is wired through (currently always set to 1).
 */

#define MESH_PROTO_MAGIC    0x54525351U   /* 'QSRT' little-endian */
#define MESH_PROTO_VERSION  1

typedef enum {
    MESH_MSG_PEER_STATE  = 1,   /* periodic broadcast: my level + sub_step */
    MESH_MSG_SLOT_CLAIM  = 2,   /* "I want slot N" during boot claim phase */
    MESH_MSG_SLOT_RELEASE = 3,  /* leaving / yielding slot (unused yet) */
} mesh_msg_type_t;

/* Initialise WiFi (STA, LR PHY, fixed channel) and ESP-NOW, then
 * start the RX dispatch task. Idempotent — second call is a no-op.
 * Channel comes from NVS if set, otherwise MESH_DEFAULT_CHANNEL. */
esp_err_t mesh_transport_init(void);

/* Get / set the operating channel. Persists to NVS. Caller must
 * call mesh_transport_init() before set; set takes effect on next
 * boot OR can be applied live via mesh_transport_apply_channel(). */
uint8_t mesh_transport_get_channel(void);
esp_err_t mesh_transport_set_channel_nvs(uint8_t ch);

/* Broadcast our current state (queue_level + sub_step + type +
 * number) to the mesh. Called periodically by the broadcaster task;
 * callers can also force-send on a level commit to cut latency. */
esp_err_t mesh_transport_broadcast_state(const app_state_t *st);

/* Broadcast a slot-claim frame during the auto-claim phase. */
esp_err_t mesh_transport_broadcast_slot_claim(device_type_t type,
                                              uint8_t want_slot);

/* Stats for diagnostics (logged each minute). */
typedef struct {
    uint32_t tx_ok;
    uint32_t tx_fail;
    uint32_t rx_total;
    uint32_t rx_dup;
    uint32_t rx_bad;
} mesh_transport_stats_t;
void mesh_transport_get_stats(mesh_transport_stats_t *out);

#ifdef __cplusplus
}
#endif

#endif /* MESH_TRANSPORT_H */
