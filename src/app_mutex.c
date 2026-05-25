#include "app_mutex.h"

static SemaphoreHandle_t s_app_mutex = NULL;

void app_mutex_init(void)
{
    if (!s_app_mutex) {
        s_app_mutex = xSemaphoreCreateMutex();
    }
}

SemaphoreHandle_t app_mutex_get(void)
{
    return s_app_mutex;
}

bool app_mutex_lock(TickType_t timeout)
{
    if (!s_app_mutex) {
        return false;
    }
    return xSemaphoreTake(s_app_mutex, timeout) == pdTRUE;
}

void app_mutex_unlock(void)
{
    if (s_app_mutex) {
        xSemaphoreGive(s_app_mutex);
    }
}
