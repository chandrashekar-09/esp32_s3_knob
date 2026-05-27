/* Inactivity alert — 3-cycle alert sequence + deep sleep.
 *
 * Macro state machine, each phase 10 s:
 *
 *   ALERT 1 (10 s)  → backlight pulse 1 Hz + beep burst 1 Hz, overlay visible
 *   BREAK 1 (10 s)  → silent, backlight normal, overlay hidden
 *   ALERT 2 (10 s)  → blink + beep, overlay visible
 *   BREAK 2 (10 s)  → silent, overlay hidden
 *   ALERT 3 (10 s)  → blink + beep, overlay visible
 *   FINAL          → reset queue to EMPTY, enter deep sleep
 *
 * Deep sleep wakes on encoder rotation / button press / touch — all
 * four pins (ENC_A/B/BTN + TOUCH_INT) are RTC GPIOs on ESP32-S3 and
 * idle HIGH, so ESP_EXT1_WAKEUP_ANY_LOW catches any of them going
 * low (rotation, press, finger contact). The chip restarts from
 * app_main on wake.
 *
 * Hardware: see file header in earlier version for the full rationale
 * on backlight-pulse-instead-of-RGB and I2S-beep substitutions.
 */

#include "inactivity_alert.h"

#include <math.h>
#include <string.h>

#include "app_config.h"
#include "display_driver.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "phase_manager.h"

static const char *kTag = "alert";

/* Blink cadence inside an ALERT phase. */
#define BLINK_PHASE_MS      500
/* Each macro phase (ALERT_n / BREAK_n) lasts this long. */
#define MACRO_PHASE_MS      10000

#define BL_BRIGHT_PCT       100
#define BL_DIM_PCT          20

#define BEEP_SAMPLE_RATE_HZ 16000
#define BEEP_AMPLITUDE      6000

/* Melodic alert tune — 3-note G-major arpeggio ascending. Centered
 * in the same high register as the prior single 2 kHz tone (the
 * middle note is ~1976 Hz) so it stays attention-grabbing, but the
 * upward arpeggio reads as a polite "ding-ding-ding?" question
 * rather than a flat continuous beep. 80 ms per note × 3 = 240 ms
 * total burst (fits comfortably inside the 500 ms blink phase). */
#define BEEP_NOTE_MS        80
#define BEEP_NOTE_COUNT     3
#define BEEP_BURST_MS       (BEEP_NOTE_MS * BEEP_NOTE_COUNT)

static const int kBeepNoteHz[BEEP_NOTE_COUNT] = {
    1568,   /* G6 */
    1976,   /* B6 */
    2349,   /* D7 — G major triad, ascending */
};

#define BEEP_SAMPLES_PER_NOTE  ((BEEP_SAMPLE_RATE_HZ * BEEP_NOTE_MS) / 1000)
#define BEEP_SAMPLES           (BEEP_SAMPLES_PER_NOTE * BEEP_NOTE_COUNT)

typedef enum {
    MACRO_OFF = 0,
    MACRO_ALERT_1,
    MACRO_BREAK_1,
    MACRO_ALERT_2,
    MACRO_BREAK_2,
    MACRO_ALERT_3,
    MACRO_FINAL,        /* reset + deep sleep — never returns */
} alert_macro_t;

static bool             s_active           = false;
static alert_macro_t    s_macro            = MACRO_OFF;
static int64_t          s_macro_start_us   = 0;

static bool             s_blink_on         = false;
static int64_t          s_last_blink_us    = 0;

static bool             s_i2s_ready        = false;
static i2s_chan_handle_t s_i2s_tx          = NULL;
static int16_t          s_tone_buf[BEEP_SAMPLES];

static const char *macro_name(alert_macro_t m)
{
    switch (m) {
    case MACRO_OFF:     return "OFF";
    case MACRO_ALERT_1: return "ALERT_1";
    case MACRO_BREAK_1: return "BREAK_1";
    case MACRO_ALERT_2: return "ALERT_2";
    case MACRO_BREAK_2: return "BREAK_2";
    case MACRO_ALERT_3: return "ALERT_3";
    case MACRO_FINAL:   return "FINAL";
    default:            return "?";
    }
}

static bool macro_is_alert(alert_macro_t m)
{
    return m == MACRO_ALERT_1 || m == MACRO_ALERT_2 || m == MACRO_ALERT_3;
}

static void mute_drive(bool unmute)
{
    gpio_set_level(APP_AUDIO_MUTE_PIN, unmute ? 1 : 0);
}

static void beep_init(void)
{
    gpio_config_t mute_cfg = {
        .pin_bit_mask = 1ULL << APP_AUDIO_MUTE_PIN,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&mute_cfg) != ESP_OK) {
        ESP_LOGW(kTag, "MUTE pin config failed — beep disabled");
        return;
    }
    mute_drive(false);

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    if (i2s_new_channel(&chan_cfg, &s_i2s_tx, NULL) != ESP_OK) {
        ESP_LOGW(kTag, "I2S channel alloc failed — beep disabled");
        return;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(BEEP_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = APP_AUDIO_I2S_BCK,
            .ws   = APP_AUDIO_I2S_WS,
            .dout = APP_AUDIO_I2S_DO,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = { 0, 0, 0 },
        },
    };
    if (i2s_channel_init_std_mode(s_i2s_tx, &std_cfg) != ESP_OK ||
        i2s_channel_enable(s_i2s_tx) != ESP_OK) {
        ESP_LOGW(kTag, "I2S init/enable failed — beep disabled");
        i2s_del_channel(s_i2s_tx);
        s_i2s_tx = NULL;
        return;
    }

    /* Fill the buffer note-by-note: each note is a square wave at its
     * own frequency for BEEP_SAMPLES_PER_NOTE samples, written
     * back-to-back. Note transitions are abrupt (no envelope) — the
     * subtle click at each boundary is part of the alert character. */
    for (int n = 0; n < BEEP_NOTE_COUNT; n++) {
        int half_period = BEEP_SAMPLE_RATE_HZ / (kBeepNoteHz[n] * 2);
        int base = n * BEEP_SAMPLES_PER_NOTE;
        for (int i = 0; i < BEEP_SAMPLES_PER_NOTE; i++) {
            s_tone_buf[base + i] =
                ((i / half_period) & 1) ? -BEEP_AMPLITUDE : BEEP_AMPLITUDE;
        }
    }

    s_i2s_ready = true;
    ESP_LOGI(kTag, "I2S beep ready: %d-note arpeggio %dHz..%dHz, %dms burst",
             BEEP_NOTE_COUNT,
             kBeepNoteHz[0], kBeepNoteHz[BEEP_NOTE_COUNT - 1],
             BEEP_BURST_MS);
}

static void beep_burst(void)
{
    if (!s_i2s_ready) return;
    size_t written = 0;
    i2s_channel_write(s_i2s_tx, s_tone_buf, sizeof(s_tone_buf), &written, 0);
}

/* Called when we enter each macro phase. Sets the immediate outputs
 * (backlight, mute, beep). The per-500ms blink toggling inside an
 * ALERT phase is handled in tick(). */
static void enter_macro(alert_macro_t m)
{
    s_macro = m;
    s_macro_start_us = esp_timer_get_time();
    s_blink_on = false;
    s_last_blink_us = 0;
    ESP_LOGI(kTag, "macro → %s", macro_name(m));

    if (macro_is_alert(m)) {
        /* Start in dim phase; tick() will flip to bright + fire the
         * first beep almost immediately on next call. */
        display_driver_set_backlight(BL_DIM_PCT);
        mute_drive(false);
    } else {
        /* BREAK or OFF: restore baseline, silence. */
        display_driver_set_backlight(BL_BRIGHT_PCT);
        mute_drive(false);
    }
}

static void enter_final_and_sleep(void)
{
    ESP_LOGI(kTag, "FINAL — reset queue, deep sleep (wake on encoder/tap)");

    /* Reset queue back to default before sleep so the next wake
     * starts fresh. */
    phase_manager_reset_queue();

    /* Restore display state so a half-pulsed backlight isn't burned
     * in via deep sleep restart. */
    display_driver_set_backlight(BL_BRIGHT_PCT);
    mute_drive(false);

    /* Wake sources — any of these pins going LOW will wake the chip.
     * All four are RTC-capable GPIOs on ESP32-S3 (pins 0-21 range).
     * Encoder A/B/BTN have internal pullups so they idle HIGH and a
     * rotation/press drops them; touch INT idles HIGH and the
     * cst816 pulls it low on contact. */
    uint64_t mask = (1ULL << APP_PIN_ENC_A) |
                    (1ULL << APP_PIN_ENC_B) |
                    (1ULL << APP_PIN_ENC_BTN) |
                    (1ULL << APP_TOUCH_PIN_INT);
    esp_sleep_enable_ext1_wakeup(mask, ESP_EXT1_WAKEUP_ANY_LOW);

    /* Brief grace period so the last log line + I2S drain finish. */
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_deep_sleep_start();  /* no return — chip restarts on wake */
}

void inactivity_alert_init(void)
{
    static bool s_init_done = false;
    if (s_init_done) return;
    s_init_done = true;
    beep_init();
}

void inactivity_alert_set(bool active)
{
    inactivity_alert_init();

    if (active == s_active) return;
    s_active = active;

    if (active) {
        ESP_LOGI(kTag, "ENTER inactivity — starting 3-alert sequence");
        enter_macro(MACRO_ALERT_1);
    } else {
        ESP_LOGI(kTag, "EXIT inactivity — sequence aborted");
        enter_macro(MACRO_OFF);
    }
}

void inactivity_alert_tick(void)
{
    if (!s_active || s_macro == MACRO_OFF) return;

    int64_t now = esp_timer_get_time();

    /* Macro phase timing — advance to next phase when 10 s elapsed. */
    if ((now - s_macro_start_us) >= (int64_t)MACRO_PHASE_MS * 1000) {
        switch (s_macro) {
        case MACRO_ALERT_1: enter_macro(MACRO_BREAK_1); break;
        case MACRO_BREAK_1: enter_macro(MACRO_ALERT_2); break;
        case MACRO_ALERT_2: enter_macro(MACRO_BREAK_2); break;
        case MACRO_BREAK_2: enter_macro(MACRO_ALERT_3); break;
        case MACRO_ALERT_3:
            /* End of 3rd alert — final reset + deep sleep. */
            s_macro = MACRO_FINAL;
            enter_final_and_sleep();
            return;  /* unreachable */
        default: break;
        }
    }

    /* Inside-phase blink/beep only during ALERT phases. */
    if (!macro_is_alert(s_macro)) return;

    if (s_last_blink_us != 0 &&
        (now - s_last_blink_us) < (int64_t)BLINK_PHASE_MS * 1000) {
        return;
    }
    s_last_blink_us = now;
    s_blink_on = !s_blink_on;

    if (s_blink_on) {
        display_driver_set_backlight(BL_BRIGHT_PCT);
        mute_drive(true);
        beep_burst();
    } else {
        display_driver_set_backlight(BL_DIM_PCT);
        mute_drive(false);
    }
}

bool inactivity_alert_overlay_visible(void)
{
    return s_active && macro_is_alert(s_macro);
}
