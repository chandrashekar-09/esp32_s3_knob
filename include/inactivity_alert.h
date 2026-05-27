#ifndef INACTIVITY_ALERT_H
#define INACTIVITY_ALERT_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One-time setup: configures the I2S audio peripheral and MUTE GPIO
 * so the beep can play. Idempotent — safe to call multiple times.
 * If the I2S init fails (resource busy / pin conflict) the function
 * logs a warning and the alert silently degrades to backlight-only.
 * inactivity_alert_set() lazy-calls this if it hasn't run yet, but
 * calling it explicitly from app_main keeps init failures observable
 * at boot rather than at first alert. */
void inactivity_alert_init(void);

/* Edge-triggered alert dispatch. The UI task calls this once per
 * transition of the phase_manager inactivity flag:
 *   inactivity_alert_set(true)  — entered "confirm status" state,
 *                                 start the 3-alert sequence
 *   inactivity_alert_set(false) — input received, abort the sequence
 *                                 and restore backlight + mute amp
 *
 * Sequence pattern (10 s each phase):
 *   ALERT 1 → BREAK 1 → ALERT 2 → BREAK 2 → ALERT 3 →
 *   reset queue to EMPTY + deep sleep (wake on encoder or tap)
 *
 * Hardware substitutions made because the JC3636K718 has no dedicated
 * RGB LED: "RGB blink" → backlight pulse on TFT_BLK (pin 21).
 * "beep" → real I2S tone burst on the manufacturer's audio pins
 * (BCK=3, WS=45, DO=42, MUTE=46). */
void inactivity_alert_set(bool active);

/* Called periodically (e.g. from the UI render loop) while the alert
 * is active. Drives the actual blink/beep cadence and macro-phase
 * transitions. Safe to call at any rate (idempotent); recommend
 * 50-200 Hz. */
void inactivity_alert_tick(void);

/* True when the overlay should be visible — only during the three
 * ALERT macro phases, not during the BREAK phases between them.
 * The UI engine polls this each frame and shows/hides the overlay
 * accordingly. */
bool inactivity_alert_overlay_visible(void);

#ifdef __cplusplus
}
#endif

#endif /* INACTIVITY_ALERT_H */
