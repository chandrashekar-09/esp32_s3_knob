#ifndef APP_MUTEX_H
#define APP_MUTEX_H

#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#ifdef __cplusplus
extern "C" {
#endif

void app_mutex_init(void);
SemaphoreHandle_t app_mutex_get(void);
bool app_mutex_lock(TickType_t timeout);
void app_mutex_unlock(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_MUTEX_H */
