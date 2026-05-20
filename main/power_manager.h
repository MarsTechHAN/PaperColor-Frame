#ifndef PAPER_E6_POWER_MANAGER_H
#define PAPER_E6_POWER_MANAGER_H

#ifdef __cplusplus
extern "C" {
#endif

// Starts button scanning, status-LED state, and automatic deep-sleep policy.
int power_manager_start(void);

#ifdef __cplusplus
}
#endif
#endif
