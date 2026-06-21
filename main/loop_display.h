#ifndef PAPER_E6_LOOP_DISPLAY_H
#define PAPER_E6_LOOP_DISPLAY_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Start the single display-worker task.  Idempotent.
int loop_display_start(void);

// Async: ask the worker to refresh the panel with /sdcard/photos/<name>.
// Returns 0 if queued, -1 if the queue is full.
int loop_display_request_show(const char *name);

// Async: ask the worker to advance to the next photo in the directory.
int loop_display_request_next(void);

// Async: paint the welcome card unconditionally, regardless of whether the
// photo library is empty. Used by the "long-press top button" recovery path
// so the user can read the AP credentials when they've forgotten them.
int loop_display_request_welcome(void);

// Async: power-on first screen. Paints the welcome card, dwells ~15 s so the
// user can read the on-screen Wi-Fi credentials, then advances to the first
// stored photo if the library is non-empty (leaves the welcome up otherwise).
// Queued by main() on a fresh power-on / top-button wake.
int loop_display_request_boot_splash(void);

// Async: paint the entire panel with a single ink (ink_code is the panel's
// 4-bit code: 0=black 1=white 2=yellow 3=red 5=blue 6=green).  Used by the
// calibration wizard so the user has a full-screen reference to match
// against.  Returns 0 if queued.
int loop_display_request_fill(uint8_t ink_code);

// Async: paint a calibration halftone patch from up to six ink codes and
// integer weights. Used for measuring mixed-ink optical behaviour. Weights are
// relative and do not need to sum to 100.
int loop_display_request_mix(const uint8_t *ink_codes,
                             const uint8_t *weights,
                             uint8_t n);

// Block the calling thread until the worker is currently idle (the panel is
// holding the last image and no command is in flight).  Useful before a
// shutdown.
void loop_display_wait_idle(void);

// Bounded variant: wait up to timeout_ms for the worker to go idle. Returns
// true if it became idle, false on timeout. Used by the deep-sleep path so a
// permanently wedged controller can't block sleep (and drain the battery)
// forever — the rails are cut on sleep regardless.
bool loop_display_wait_idle_timeout(uint32_t timeout_ms);

// Non-blocking idle snapshot.
bool loop_display_is_idle(void);

#ifdef __cplusplus
}
#endif
#endif
