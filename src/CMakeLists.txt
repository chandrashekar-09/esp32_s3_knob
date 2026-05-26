# UI build: screen + touch + encoder + LVGL UI pipeline.
idf_component_register(SRCS "main.c"
                            "display_driver.c"
                            "touch_cst816.c"
                            "input_encoder.c"
                            "app_mutex.c"
                            "lvgl_port.c"
                            "ui_engine.c"
                            "phase_manager.c"
                       INCLUDE_DIRS "../include"
                       PRIV_REQUIRES driver esp_driver_gpio esp_driver_i2c
                                     esp_driver_spi esp_driver_pcnt
                                     esp_hw_support esp_mm nvs_flash log
                                     esp_timer esp_system freertos)
