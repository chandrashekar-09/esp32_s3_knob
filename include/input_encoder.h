#ifndef INPUT_ENCODER_H
#define INPUT_ENCODER_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int pin_a;
    int pin_b;
    int pin_btn;
    uint32_t glitch_filter_us;
} encoder_config_t;

typedef struct {
    int delta;
    bool pressed;
    uint32_t duration_ms;
} encoder_event_t;

typedef void (*encoder_event_cb_t)(const encoder_event_t *event, void *ctx);

esp_err_t encoder_init(const encoder_config_t *config, encoder_event_cb_t cb, void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* INPUT_ENCODER_H */
