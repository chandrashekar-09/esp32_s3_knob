/* Rotary encoder + push button input.
 *
 * GPIO polling decoder lifted from the JC3636K718 manufacturer's demo
 * (Demo_arduino/ST77916_LVGL_DEMO/bidi_switch_knob.c). PCNT quadrature
 * decoding does NOT play nice with this particular encoder module —
 * the B-channel level isn't deterministic at the moment of A's edges,
 * which made the PCNT-based driver emit alternating +1/-1 in a single
 * rotation direction. The manufacturer's approach sidesteps this:
 *
 *   - poll BOTH channels at 3 ms intervals
 *   - on A's debounced LOW→HIGH transition  → emit +1  (RIGHT)
 *   - on B's debounced LOW→HIGH transition  → emit -1  (LEFT)
 *
 * Per-detent the mechanical encoder produces a clean LOW→HIGH cycle
 * on ONLY ONE channel — A for CW, B for CCW. The "stays LOW for
 * DEBOUNCE_TICKS then rises" gate filters out partial transitions of
 * the OTHER channel during the same detent, so each click emits exactly
 * one delta in the correct direction.
 *
 * Button handling fires:
 *   - SHORT-press event on release if hold was below LONG_PRESS_MS
 *   - LONG-press event the moment the hold crosses LONG_PRESS_MS
 *     (does NOT wait for release) so the user gets immediate visual
 *     feedback when they cross the threshold and can let go.
 */

#include "input_encoder.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *kTag = "encoder";

/* Polling interval — 3 ms matches the manufacturer demo's TICKS_INTERVAL.
 * Fast enough that a quick spin doesn't outrun the poller, slow enough
 * to debounce out chatter without burning CPU. */
#define POLL_INTERVAL_MS    3

/* A channel must stay LOW for this many polls before its rising edge is
 * treated as a real detent. Filters out the brief LOW dip the OTHER
 * channel makes during a detent on the FIRST channel. */
#define DEBOUNCE_TICKS      2

/* Mutual-interlock window: after either A or B fires, ignore further
 * events from either channel for this long. Prevents the same physical
 * detent from producing two emits when both channels race past the
 * debounce gate (the "spun once, jumped two" failure). Tuned just
 * above one quadrature half-cycle worth of poll time — long enough to
 * eat the trailing channel's late rise, short enough that a fast spin
 * still resolves each detent. */
#define INTERLOCK_MS        15

/* Rate limit on emitted detents. Raw detents accumulate into a small
 * signed buffer; the poll task drains at most ONE ±1 per this interval.
 * Effect: regardless of how fast the user spins, the queue arc grows
 * at a fixed maximum rate (≈1000/EMIT_MIN_INTERVAL_MS detents/sec).
 * After the user stops spinning, the buffer continues draining smoothly
 * until empty. Combined with the 180 ms arc animation tween in
 * ui_engine.c this gives a continuous "smooth follow" feel even on a
 * frantic spin — never snappy, never laggy. */
#define EMIT_MIN_INTERVAL_MS    60
/* Cap on buffered pending detents so a deliberately absurd spin
 * doesn't take 30 s to settle. ±18 detents = ±6 status levels at
 * 3:1 damping — already more change than any real workflow needs. */
#define PENDING_CAP             18

/* Sensitivity damping is handled in phase_manager — see queue_sub_step.
 * The encoder driver emits one ±1 per raw detent so the UI can animate
 * the queue arc continuously while the discrete status level only
 * commits every N detents (set by STEP in phase_manager.c). */

/* How long the button must be held continuously to register as a
 * long-press. Fires immediately at this threshold (not on release). */
#define LONG_PRESS_MS       1500

/* Software debounce for the button. Edges within this window of the
 * previous edge are ignored. */
#define BTN_DEBOUNCE_US     20000

static encoder_event_cb_t s_cb     = NULL;
static void *s_cb_ctx              = NULL;

/* Encoder pin tracking */
static int s_pin_a                 = -1;
static int s_pin_b                 = -1;
static uint8_t s_a_prev_level      = 1;
static uint8_t s_b_prev_level      = 1;
static uint8_t s_a_debounce_cnt    = 0;
static uint8_t s_b_debounce_cnt    = 0;
static int64_t s_interlock_until_us = 0;

/* Rate-limiter state — pending detents buffered, drained at fixed
 * cadence. See EMIT_MIN_INTERVAL_MS comment for behaviour rationale. */
static int     s_pending_detents   = 0;
static int64_t s_last_emit_us      = 0;


/* Button tracking */
static int s_btn_pin               = -1;
static int s_btn_last_level        = 1;
static int64_t s_btn_down_us       = 0;
static int64_t s_btn_last_edge_us  = 0;
static bool s_long_press_fired     = false;

static void emit_delta(int delta)
{
    if (!s_cb || delta == 0) return;
    encoder_event_t evt = { .delta = delta, .pressed = false, .duration_ms = 0 };
    s_cb(&evt, s_cb_ctx);
}

static void emit_button(uint32_t duration_ms)
{
    if (!s_cb) return;
    encoder_event_t evt = { .delta = 0, .pressed = true, .duration_ms = duration_ms };
    s_cb(&evt, s_cb_ctx);
}

/* Mirrors manufacturer's process_knob_channel(): emit when the channel
 * stays LOW for >= DEBOUNCE_TICKS polls and THEN goes HIGH. Returns the
 * delta to emit (0 if no event this poll). */
static int process_channel(uint8_t cur_level, uint8_t *prev_level,
                           uint8_t *debounce_cnt, int delta_on_rise)
{
    int delta = 0;
    if (cur_level == 0) {
        /* LOW: accumulate debounce count, or reset if we just dropped
         * from HIGH (start counting from 0 on the new LOW press). */
        if (cur_level != *prev_level) {
            *debounce_cnt = 0;
        } else {
            (*debounce_cnt)++;
        }
    } else {
        /* HIGH: if we just rose from LOW AND the debounce count is
         * already at threshold, fire. The pre-increment lets a single
         * LOW poll → rise emit (DEBOUNCE_TICKS=2 means: at least one
         * LOW poll, then this rising poll counts as the 2nd → fires). */
        if (cur_level != *prev_level && ++(*debounce_cnt) >= DEBOUNCE_TICKS) {
            *debounce_cnt = 0;
            delta = delta_on_rise;
        } else {
            *debounce_cnt = 0;
        }
    }
    *prev_level = cur_level;
    return delta;
}

static void poll_encoder(void)
{
    uint8_t a = (uint8_t)gpio_get_level(s_pin_a);
    uint8_t b = (uint8_t)gpio_get_level(s_pin_b);

    /* On this physical encoder, the channel that fires first when turning
     * CW is B (not A) — the manufacturer's stock A→RIGHT/B→LEFT mapping
     * is inverted from how this board is wired. So: B rising → +1
     * (turn right, queue grows); A rising → -1 (turn left, queue shrinks). */
    int da = process_channel(a, &s_a_prev_level, &s_a_debounce_cnt, -1);
    int db = process_channel(b, &s_b_prev_level, &s_b_debounce_cnt, +1);

    /* Mutual interlock: after either channel fires, suppress the OTHER
     * channel for INTERLOCK_MS so the same physical detent can't be
     * counted twice. A standard EC11 emits a rising edge on BOTH A and
     * B per detent — the manufacturer's algorithm relies on debounce
     * timing to filter the "fast" channel out, but on borderline
     * rotation speeds both channels can sneak through and produce a
     * spurious second event that feels like "spun once, jumped two."
     * The interlock window enforces "first wins" per detent. */
    int64_t now = esp_timer_get_time();
    if (da != 0 && now < s_interlock_until_us) da = 0;
    if (db != 0 && now < s_interlock_until_us) db = 0;
    if (da != 0 || db != 0) {
        s_interlock_until_us = now + (int64_t)INTERLOCK_MS * 1000;
    }

    int delta = da + db;
    if (delta != 0) {
        /* Buffer the raw detent. Cap magnitude so an absurd spin
         * can't queue minutes of motion. Sign-flip a buffer that's
         * pointing the wrong way isn't a special case — the cap
         * applies symmetrically so reversing direction is immediate. */
        s_pending_detents += delta;
        if (s_pending_detents >  PENDING_CAP) s_pending_detents =  PENDING_CAP;
        if (s_pending_detents < -PENDING_CAP) s_pending_detents = -PENDING_CAP;
    }

    /* Drain at most one queued detent per EMIT_MIN_INTERVAL_MS.
     * Runs every poll regardless of whether THIS poll saw a raw
     * detent — that's what lets the buffer keep draining smoothly
     * after the user stops physically turning the knob. */
    if (s_pending_detents != 0) {
        int64_t now = esp_timer_get_time();
        if ((now - s_last_emit_us) >= (int64_t)EMIT_MIN_INTERVAL_MS * 1000) {
            s_last_emit_us = now;
            int step = (s_pending_detents > 0) ? +1 : -1;
            s_pending_detents -= step;
            ESP_LOGI(kTag, "rotate emit=%+d  pending=%+d  (raw_da=%+d db=%+d)",
                     step, s_pending_detents, da, db);
            emit_delta(step);
        }
    }
}

static void poll_button(void)
{
    if (s_btn_pin < 0) return;

    int64_t now = esp_timer_get_time();
    int level = gpio_get_level(s_btn_pin);

    /* Long-press during hold: fire ONCE when the hold crosses the
     * threshold, before the user releases. */
    if (level == 0 && s_btn_down_us != 0 && !s_long_press_fired) {
        int64_t hold_us = now - s_btn_down_us;
        if (hold_us >= (int64_t)LONG_PRESS_MS * 1000) {
            s_long_press_fired = true;
            ESP_LOGI(kTag, "button LONG-press @%dms (during hold)", LONG_PRESS_MS);
            emit_button(LONG_PRESS_MS);
        }
    }

    /* Edge detection */
    if (level == s_btn_last_level) return;
    if ((now - s_btn_last_edge_us) < BTN_DEBOUNCE_US) return;
    s_btn_last_edge_us = now;
    s_btn_last_level = level;

    if (level == 0) {
        /* Press start. */
        s_btn_down_us = now;
        s_long_press_fired = false;
    } else {
        /* Release. If long-press already fired during hold, swallow the
         * release event. Otherwise emit short-press with measured
         * duration. */
        if (s_btn_down_us != 0 && !s_long_press_fired) {
            uint32_t dur_ms = (uint32_t)((now - s_btn_down_us) / 1000);
            ESP_LOGI(kTag, "button SHORT-press %ums", (unsigned)dur_ms);
            emit_button(dur_ms);
        }
        s_btn_down_us = 0;
        s_long_press_fired = false;
    }
}

/* esp_timer callback — runs every POLL_INTERVAL_MS on the timer service
 * task. Using esp_timer instead of a FreeRTOS task with vTaskDelay
 * because at CONFIG_FREERTOS_HZ=100 (10 ms tick) pdMS_TO_TICKS(3) rounds
 * to 0, which would spin a task without yielding and trip the WDT.
 * Matches the manufacturer demo's `esp_timer_start_periodic(..., TICKS_INTERVAL*1000U)`. */
static void encoder_timer_cb(void *arg)
{
    (void)arg;
    poll_encoder();
    poll_button();
}

static void init_gpio_input(int pin)
{
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << pin),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
}

esp_err_t encoder_init(const encoder_config_t *config, encoder_event_cb_t cb, void *ctx)
{
    if (!config || config->pin_a < 0 || config->pin_b < 0) {
        return ESP_ERR_INVALID_ARG;
    }

    s_cb = cb;
    s_cb_ctx = ctx;

    s_pin_a = config->pin_a;
    s_pin_b = config->pin_b;
    init_gpio_input(s_pin_a);
    init_gpio_input(s_pin_b);
    s_a_prev_level = (uint8_t)gpio_get_level(s_pin_a);
    s_b_prev_level = (uint8_t)gpio_get_level(s_pin_b);
    s_a_debounce_cnt = 0;
    s_b_debounce_cnt = 0;

    if (config->pin_btn >= 0) {
        s_btn_pin = config->pin_btn;
        init_gpio_input(s_btn_pin);
        s_btn_last_level = gpio_get_level(s_btn_pin);
    }

    static esp_timer_handle_t s_timer = NULL;
    if (!s_timer) {
        const esp_timer_create_args_t args = {
            .callback = encoder_timer_cb,
            .arg = NULL,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "enc_poll",
        };
        esp_err_t terr = esp_timer_create(&args, &s_timer);
        if (terr != ESP_OK) {
            ESP_LOGE(kTag, "esp_timer_create failed: %d", terr);
            return terr;
        }
        terr = esp_timer_start_periodic(s_timer, (uint64_t)POLL_INTERVAL_MS * 1000);
        if (terr != ESP_OK) {
            ESP_LOGE(kTag, "esp_timer_start_periodic failed: %d", terr);
            return terr;
        }
    }

    ESP_LOGI(kTag, "ready: A=%d B=%d BTN=%d  (poll=%dms, debounce=%d, long-press=%dms)",
             config->pin_a, config->pin_b, config->pin_btn,
             POLL_INTERVAL_MS, DEBOUNCE_TICKS, LONG_PRESS_MS);
    return ESP_OK;
}
