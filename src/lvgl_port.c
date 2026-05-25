#include "lvgl_port.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "app_mutex.h"
#include "ui_engine.h"

#if defined(__has_include)
#if __has_include("lvgl.h")
#include "lvgl.h"
#define LVGL_PORT_HAS_LVGL 1
#else
#define LVGL_PORT_HAS_LVGL 0
#endif
#else
#define LVGL_PORT_HAS_LVGL 0
#endif

static const char *kTag = "lvgl_port";

#if LVGL_PORT_HAS_LVGL
static display_config_t s_disp_cfg = {};
static cst816_config_t s_touch_cfg = {};

#if LVGL_VERSION_MAJOR >= 9
static lv_display_t *s_disp = NULL;
static lv_indev_t *s_indev = NULL;

static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    if (!px_map) {
        lv_display_flush_ready(disp);
        return;
    }

    size_t px = (size_t)(area->x2 - area->x1 + 1) * (size_t)(area->y2 - area->y1 + 1);
    display_driver_flush(area->x1, area->y1, area->x2, area->y2, px_map, px * sizeof(lv_color_t));
    lv_display_flush_ready(disp);
}

static void lvgl_touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    cst816_point_t pt = {};
    cst816_read(&pt);

    if (pt.touched) {
        data->state = LV_INDEV_STATE_PRESSED;
        data->point.x = pt.x;
        data->point.y = pt.y;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}
#else
static lv_disp_t *s_disp = NULL;
static lv_indev_t *s_indev = NULL;

static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p)
{
    if (!color_p) {
        lv_disp_flush_ready(drv);
        return;
    }

    size_t px = (size_t)(area->x2 - area->x1 + 1) * (size_t)(area->y2 - area->y1 + 1);
    display_driver_flush(area->x1, area->y1, area->x2, area->y2, color_p, px * sizeof(lv_color_t));
    lv_disp_flush_ready(drv);
}

static void lvgl_touch_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    (void)drv;
    cst816_point_t pt = {};
    cst816_read(&pt);

    if (pt.touched) {
        data->state = LV_INDEV_STATE_PRESSED;
        data->point.x = pt.x;
        data->point.y = pt.y;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}
#endif

#if !LV_TICK_CUSTOM
static void lvgl_tick_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(1);
}
#endif
#endif

bool lvgl_port_init(const display_config_t *disp_cfg, const cst816_config_t *touch_cfg)
{
#if LVGL_PORT_HAS_LVGL
    if (!disp_cfg || !touch_cfg) {
        return false;
    }

    s_disp_cfg = *disp_cfg;
    s_touch_cfg = *touch_cfg;

    lv_init();

    app_mutex_init();
    if (!xGuiSemaphore) {
        xGuiSemaphore = app_mutex_get();
    }

    if (display_driver_init(&s_disp_cfg) != ESP_OK) {
        ESP_LOGE(kTag, "display init failed");
        return false;
    }

    if (cst816_init(&s_touch_cfg) != ESP_OK) {
        ESP_LOGW(kTag, "touch init failed");
    }

    size_t buf_pixels = (size_t)s_disp_cfg.width * 40;
    lv_color_t *buf1 = heap_caps_malloc(buf_pixels * sizeof(lv_color_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    lv_color_t *buf2 = heap_caps_malloc(buf_pixels * sizeof(lv_color_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf1 || !buf2) {
        if (buf1) {
            heap_caps_free(buf1);
        }
        if (buf2) {
            heap_caps_free(buf2);
        }
        ESP_LOGW(kTag, "PSRAM draw buffers unavailable, using internal heap");
        buf1 = heap_caps_malloc(buf_pixels * sizeof(lv_color_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        buf2 = heap_caps_malloc(buf_pixels * sizeof(lv_color_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }

    if (!buf1 || !buf2) {
        ESP_LOGE(kTag, "LVGL draw buffer allocation failed");
        return false;
    }

#if LVGL_VERSION_MAJOR >= 9
    s_disp = lv_display_create(s_disp_cfg.width, s_disp_cfg.height);
    if (!s_disp) {
        ESP_LOGE(kTag, "LVGL display create failed");
        return false;
    }
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(s_disp, lvgl_flush_cb);
    lv_display_set_buffers(s_disp,
                           buf1,
                           buf2,
                           buf_pixels * sizeof(lv_color_t),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_default(s_disp);

    s_indev = lv_indev_create();
    if (s_indev) {
        lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(s_indev, lvgl_touch_read_cb);
        lv_indev_set_display(s_indev, s_disp);
    }
#else
    static lv_disp_draw_buf_t draw_buf;
    lv_disp_draw_buf_init(&draw_buf, buf1, buf2, buf_pixels);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = s_disp_cfg.width;
    disp_drv.ver_res = s_disp_cfg.height;
    disp_drv.flush_cb = lvgl_flush_cb;
    disp_drv.draw_buf = &draw_buf;
    s_disp = lv_disp_drv_register(&disp_drv);

    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = lvgl_touch_read_cb;
    s_indev = lv_indev_drv_register(&indev_drv);
#endif

#if !LV_TICK_CUSTOM
    const esp_timer_create_args_t tick_args = {
        .callback = lvgl_tick_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "lvgl_tick",
    };
    esp_timer_handle_t tick_timer = NULL;
    if (esp_timer_create(&tick_args, &tick_timer) == ESP_OK) {
        esp_timer_start_periodic(tick_timer, 1000);
    }
#endif

    return true;
#else
    (void)disp_cfg;
    (void)touch_cfg;
    ESP_LOGE(kTag, "LVGL library not available; UI disabled");
    return false;
#endif
}

void lvgl_port_lock(void)
{
#if LVGL_PORT_HAS_LVGL
    if (xGuiSemaphore) {
        xSemaphoreTake(xGuiSemaphore, pdMS_TO_TICKS(100));
    }
#endif
}

void lvgl_port_unlock(void)
{
#if LVGL_PORT_HAS_LVGL
    if (xGuiSemaphore) {
        xSemaphoreGive(xGuiSemaphore);
    }
#endif
}
