# UI build: screen + touch + encoder + LVGL UI pipeline + WiFi/OTA.
idf_component_register(SRCS "main.c"
                            "display_driver.c"
                            "touch_cst816.c"
                            "input_encoder.c"
                            "app_mutex.c"
                            "lvgl_port.c"
                            "ui_engine.c"
                            "phase_manager.c"
                            "wifi_manager.c"
                            "ota_service.c"
                            "inactivity_alert.c"
                            "peer_registry.c"
                            "advisor.c"
                            "espnow_inbound.c"
                            "peer_sim.c"
                            "sd_card.c"
                            "screenshot.c"
                       INCLUDE_DIRS "../include"
                       PRIV_REQUIRES driver esp_driver_gpio esp_driver_i2c
                                     esp_driver_spi esp_driver_pcnt
                                     # esp_driver_i2s drives the inactivity
                                     # alert beep on the manufacturer's
                                     # audio pins.
                                     esp_driver_i2s
                                     # SDMMC + FAT for the on-board microSD;
                                     # backs the triple-tap screenshot feature.
                                     esp_driver_sdmmc sdmmc fatfs vfs
                                     esp_hw_support esp_mm nvs_flash log
                                     esp_timer esp_system freertos
                                     # WiFi + OTA stack. esp_http_client +
                                     # esp_https_ota pull in mbedtls/esp-tls;
                                     # mbedtls is named explicitly so the
                                     # esp_crt_bundle header resolves.
                                     esp_wifi esp_event esp_netif
                                     esp_http_client esp_https_ota
                                     app_update mbedtls)
