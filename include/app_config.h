#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include <stdint.h>

#define APP_PIN_ENC_A 2
#define APP_PIN_ENC_B 1
#define APP_PIN_ENC_BTN 0

#define APP_LCD_PIN_CLK 11
#define APP_LCD_PIN_CS 12
#define APP_LCD_PIN_SIO0 13
#define APP_LCD_PIN_SIO1 14
#define APP_LCD_PIN_SIO2 15
#define APP_LCD_PIN_SIO3 16
#define APP_LCD_PIN_RST 17
/* TE line is on GPIO 18 in the K718 schematic but the panel does not
 * actually pulse it on this board revision. The Arduino bring-up test
 * (Arduino_GFX, no TE wiring) confirms the display works fine without
 * vsync. Setting -1 disables our TE ISR + per-frame wait. */
#define APP_LCD_PIN_TE  (-1)
#define APP_LCD_PIN_BL 21

#define APP_TOUCH_PIN_SDA 9
#define APP_TOUCH_PIN_SCL 10
#define APP_TOUCH_PIN_INT 7
#define APP_TOUCH_PIN_RST 8

/* Audio output (I2S) — matches manufacturer's pincfg.h verbatim. The
 * board's audio amp lives on these pins; MUTE is active LOW
 * ("低电平静音" in the original pincfg). Used by inactivity_alert.c
 * to produce the alert beep. If no speaker/amp is wired the pins
 * just toggle silently — no harm. */
#define APP_AUDIO_I2S_BCK   3
#define APP_AUDIO_I2S_WS    45
#define APP_AUDIO_I2S_DO    42
#define APP_AUDIO_MUTE_PIN  46

/* Device identity. The mesh supports up to 32 knobs total: 16 fitting
 * rooms (FR1..FR16) + 16 tills (T1..T16). Each knob is one of those.
 * The HOME screen only shows peers of the SAME TYPE as the own
 * device.
 *
 *   APP_DEVICE_TYPE   — 0 = FR (fitting room), 1 = T (till).
 *                       Encoder long-press toggles this at runtime
 *                       so the same flashed firmware can become
 *                       either type without a rebuild; this define
 *                       is just the boot-time default.
 *   APP_DEVICE_NUMBER — 1..16. Slot within the chosen type. Drives
 *                       the queue-ring colour via the 16-colour
 *                       device palette and the label suffix
 *                       (FR1..FR16 / T1..T16). */
#define APP_DEVICE_TYPE     0   /* 0 = FR, 1 = T */
#define APP_DEVICE_NUMBER   1

/* Peer simulation toggle — testing mode.
 *
 *   1 = on every boot, populate the peer registry with a random
 *       number of same-type peers (1..15 extra peers, total 2..16)
 *       at random queue levels (1..5). Static values — no dynamic
 *       changes. Lets you see how the UI looks across different
 *       tier / fill configurations without a real mesh.
 *
 *   0 = no peer simulation. Only the own device renders, advisor
 *       returns inactive until real mesh data populates the
 *       registry via espnow_inbound_peer().
 *
 * Flip to 0 when ready for the real mesh integration. */
#define APP_PEER_SIM        1

#define APP_DISPLAY_WIDTH 360
#define APP_DISPLAY_HEIGHT 360
/* 50 MHz matches the manufacturer's ST77916_LVGL_DEMO/scr_st77916.h
 * `TFT_SPI_FREQ_HZ` exactly. The panel module is qualified at this rate. */
#define APP_DISPLAY_PCLK_HZ (50 * 1000 * 1000)

//#define APP_MESH_ROUTER_SSID "IIIT-Guest"
//#define APP_MESH_ROUTER_PASS "f6s68VHJ89mC"

#define APP_MESH_ROUTER_SSID "TEAMPLAYER 9060"
#define APP_MESH_ROUTER_PASS "7mA82;58"


#define APP_MESH_AP_PASS "mesh_ap_pass"

#define APP_OTA_CURRENT_VERSION 4
#define APP_OTA_VERSION_URL "https://raw.githubusercontent.com/chandrashekar-09/esp32_s3_knob/main/version.txt"
#define APP_OTA_FIRMWARE_URL "https://raw.githubusercontent.com/chandrashekar-09/esp32_s3_knob/main/.pio/build/esp32-s3/firmware.bin"
#define APP_OTA_DEVICE_ID "knob-002"
#define APP_OTA_BOOT_ACK_BASE_URL "https://techlora-369-default-rtdb.asia-southeast1.firebasedatabase.app/boot_ack"
#define APP_OTA_BOOT_ACK_AUTH ""
#define APP_OTA_EXPECTED_SHA256 ""

#define APP_SKIP_NET_UI 1
#define APP_SKIP_TIME_UI 1

#endif /* APP_CONFIG_H */
