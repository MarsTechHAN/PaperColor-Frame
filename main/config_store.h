#ifndef PAPER_E6_CONFIG_STORE_H
#define PAPER_E6_CONFIG_STORE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CONFIG_LOOP_INTERVAL_NEVER_S  0
#define CONFIG_LOOP_INTERVAL_MIN_S    300
#define CONFIG_LOOP_INTERVAL_MAX_S    21600
#define CONFIG_LOOP_INTERVAL_DEFAULT  600

#define CONFIG_WIFI_IDLE_SLEEP_MIN_S      30
#define CONFIG_WIFI_IDLE_SLEEP_MAX_S      900
#define CONFIG_WIFI_IDLE_SLEEP_DEFAULT_S  300

#define CONFIG_WIFI_AP_SSID_DEFAULT       "PaperColor"
#define CONFIG_WIFI_SSID_MAX_LEN          32
#define CONFIG_WIFI_AP_PASSWORD_MAX_LEN   63
#define CONFIG_WIFI_STA_PASSWORD_MAX_LEN  63

typedef enum {
    CONFIG_WIFI_MODE_AP = 0,
    CONFIG_WIFI_MODE_STA = 1,
    CONFIG_WIFI_MODE_STA_AP = 2,
} config_wifi_mode_t;

// Max length of the calibration JSON blob (palette + default adjust +
// optional measured mixed-ink patches). Keep below NVS string practical
// limits while leaving room for the calibration target set.
#define CONFIG_CALIB_MAX_LEN          8192

// Open the NVS namespace ("paper_e6") and read all persisted values.
// Idempotent.  Returns 0 on success.
int config_store_init(void);

// Loop-display interval in seconds. 0 means "Never"; other values are snapped
// to the supported discrete intervals on set.
uint32_t config_get_loop_interval_s(void);
int      config_set_loop_interval_s(uint32_t s);

// Sleep after this many seconds with no connected Wi-Fi client. Snapped to the
// supported discrete intervals on set.
uint32_t config_get_wifi_idle_sleep_s(void);
int      config_set_wifi_idle_sleep_s(uint32_t s);

// Wi-Fi operating mode. AP is the factory default; STA and STA+AP use the
// configured home-network credentials below.
config_wifi_mode_t config_get_wifi_mode(void);
const char        *config_get_wifi_mode_string(void);
int                config_set_wifi_mode(config_wifi_mode_t mode);
int                config_set_wifi_mode_string(const char *mode);

// Captive-portal SoftAP identity. Password is empty for an open AP; otherwise
// it must be an ASCII WPA2 passphrase of 8..63 characters.
const char *config_get_wifi_ap_ssid(void);
const char *config_get_wifi_ap_password(void);
int         config_set_wifi_ap_password(const char *password);

// STA/home-network credentials. Empty password means an open network.
const char *config_get_wifi_sta_ssid(void);
const char *config_get_wifi_sta_password(void);
int         config_set_wifi_sta_ssid(const char *ssid);
int         config_set_wifi_sta_password(const char *password);

// Battery overlay on displayed photos. Enabled by default to preserve the
// existing behaviour.
bool config_get_battery_icon_enabled(void);
int  config_set_battery_icon_enabled(bool enabled);

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
