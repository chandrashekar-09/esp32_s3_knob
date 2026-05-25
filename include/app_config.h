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
#define APP_LCD_PIN_TE 18
#define APP_LCD_PIN_BL 21

#define APP_TOUCH_PIN_SDA 9
#define APP_TOUCH_PIN_SCL 10
#define APP_TOUCH_PIN_INT 7
#define APP_TOUCH_PIN_RST 8

#define APP_DISPLAY_WIDTH 360
#define APP_DISPLAY_HEIGHT 360
#define APP_DISPLAY_PCLK_HZ (40 * 1000 * 1000)

#define APP_MESH_ROUTER_SSID "IIIT-Guest"
#define APP_MESH_ROUTER_PASS "f6s68VHJ89mC"
#define APP_MESH_AP_PASS "mesh_ap_pass"

#define APP_OTA_CURRENT_VERSION 2
#define APP_OTA_VERSION_URL "https://raw.githubusercontent.com/chandrashekar-09/esp32_s3_knob/main/version.txt"
#define APP_OTA_FIRMWARE_URL "https://raw.githubusercontent.com/chandrashekar-09/esp32_s3_knob/main/.pio/build/esp32-s3/firmware.bin"
#define APP_OTA_DEVICE_ID "knob-002"
#define APP_OTA_BOOT_ACK_BASE_URL "https://techlora-369-default-rtdb.asia-southeast1.firebasedatabase.app/boot_ack"
#define APP_OTA_BOOT_ACK_AUTH ""
#define APP_OTA_EXPECTED_SHA256 ""

#define APP_SKIP_NET_UI 1
#define APP_SKIP_TIME_UI 1

#endif /* APP_CONFIG_H */
