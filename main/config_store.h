#ifndef PAPER_E6_CONFIG_STORE_H
#define PAPER_E6_CONFIG_STORE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CONFIG_LOOP_INTERVAL_MIN_S    30
#define CONFIG_LOOP_INTERVAL_MAX_S    3600
#define CONFIG_LOOP_INTERVAL_DEFAULT  600

// Max length of the calibration JSON blob (palette + default adjust).
// Real payload is ~1 KB; 2 KB gives plenty of headroom for tunable fields.
#define CONFIG_CALIB_MAX_LEN          2048

// Open the NVS namespace ("paper_e6") and read all persisted values.
// Idempotent.  Returns 0 on success.
int config_store_init(void);

// Loop-display interval in seconds.  Clamped on set.
uint32_t config_get_loop_interval_s(void);
int      config_set_loop_interval_s(uint32_t s);

// Calibration JSON (palette LAB/RGB + default adjust).  The web UI fetches
// this on load and POSTs back when the user edits it.  Returns the number
// of bytes written to `out` (excluding the trailing NUL) on success, or -1.
int  config_get_calib_json(char *out, size_t out_sz);

// Store the calibration JSON.  `len` must be <= CONFIG_CALIB_MAX_LEN.
// Returns 0 on success.
int  config_set_calib_json(const char *json, size_t len);

#ifdef __cplusplus
}
#endif
#endif
