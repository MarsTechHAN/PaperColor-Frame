#ifndef PAPER_E6_POWER_MANAGER_H
#define PAPER_E6_POWER_MANAGER_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    // Wi-Fi/DNS/HTTP were started for this boot. The status LEDs are only
    // initialised and driven in this mode.
    bool wifi_enabled;
    // Sleep as soon as the queued display refresh has completed.
    bool sleep_when_display_idle;
    // Sleep 3 seconds after the queued physical-button refresh completes.
    bool button_refresh_wake;
} power_manager_config_t;

// Starts button scanning, optional status-LED state, and deep-sleep policy.
int power_manager_start(const power_manager_config_t *config);

#ifdef __cplusplus
}
#endif
#endif
