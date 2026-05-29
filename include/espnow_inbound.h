#ifndef ESPNOW_INBOUND_H
#define ESPNOW_INBOUND_H

#include <stdbool.h>
#include <stdint.h>

#include "phase_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/* DORMANT inbound handlers — entry points the (future) ESP-NOW
 * receive layer will call when it parses a frame. Implemented now
 * so the moment mesh ships, master commands work without any
 * further code in this file or the UI. See
 * [[project-sorting-architecture]] §1d "Master remote command".
 *
 * Frames are validated by the caller (HMAC / version / wire-shape).
 * These handlers assume the parsed values are trustworthy. */

/* Peer broadcast → registry upsert. Called once per ~1 Hz frame
 * from any same-type or cross-type peer. The advisor sees the
 * updated registry on the next render tick.
 *
 * `mac` is the peer's STA MAC (origin_mac of the mesh frame) and
 * acts as the registry's primary key — required so a peer that
 * changes its `number` (rank rebalance after a join/leave) updates
 * its existing slot in place rather than orphaning the old number. */
void espnow_inbound_peer_full(device_type_t type, uint8_t number,
                              uint8_t queue_level, int8_t sub_step,
                              const uint8_t mac[6]);

/* Legacy 3-arg + 4-arg shims kept for any caller that doesn't
 * have a MAC yet (e.g. local-only stubs). These no-op safely if
 * MAC is unavailable since the registry now requires MAC. */
void espnow_inbound_peer(device_type_t type, uint8_t number,
                         uint8_t queue_level);

/* Master command → unconditional level write (bypasses CSI /
 * QueVision gates) when the addressed slot is THIS knob's identity.
 * No-op when target_slot != own_number || target_type != own_type.
 *
 *   level: 1..5 (5-state taxonomy; matches our wire format)
 *
 * Per the spec, master commands DO NOT stamp last_user_input_us —
 * they're remote-driven, not staff-driven, so they don't open the
 * staff-authority window. */
void espnow_inbound_master_cmd(device_type_t target_type,
                               uint8_t target_slot,
                               uint8_t level);

/* Peer left the mesh (timeout / explicit leave). */
void espnow_inbound_peer_offline(device_type_t type, uint8_t number);

#ifdef __cplusplus
}
#endif

#endif /* ESPNOW_INBOUND_H */
