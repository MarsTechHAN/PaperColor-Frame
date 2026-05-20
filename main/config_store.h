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

#define CONFIG_WIFI_IDLE_SLEEP_MIN_S      30
#define CONFIG_WIFI_IDLE_SLEEP_MAX_S      86400
#define CONFIG_WIFI_IDLE_SLEEP_DEFAULT_S  300

#define CONFIG_BUTTON_SLEEP_MIN_S         1
#define CONFIG_BUTTON_SLEEP_MAX_S         60
#define CONFIG_BUTTON_SLEEP_DEFAULT_S     3

#define CONFIG_STATUS_LED_BRIGHTNESS_MIN  0
#define CONFIG_STATUS_LED_BRIGHTNESS_MAX  80
#define CONFIG_STATUS_LED_BRIGHTNESS_DEFAULT  24

#define CONFIG_WIFI_AP_SSID_DEFAULT       "PaperColor"
#define CONFIG_WIFI_AP_PASSWORD_MAX_LEN   63

// Max length of the calibration JSON blob (palette + default adjust +
// optional measured mixed-ink patches). Keep below NVS string practical
// limits while leaving room for the calibration target set.
#define CONFIG_CALIB_MAX_LEN          4096

// Open the NVS namespace ("paper_e6") and read all persisted values.
// Idempotent.  Returns 0 on success.
int config_store_init(void);

// Loop-display interval in seconds.  Clamped on set.
uint32_t config_get_loop_interval_s(void);
int      config_set_loop_interval_s(uint32_t s);

// Sleep after this many seconds with no connected Wi-Fi client.
uint32_t config_get_wifi_idle_sleep_s(void);
int      config_set_wifi_idle_sleep_s(uint32_t s);

// Sleep after a button/timer refresh once the panel has been idle this long.
uint32_t config_get_button_sleep_s(void);
int      config_set_button_sleep_s(uint32_t s);

// Status NeoPixel brightness limit, 0 disables the LEDs.
uint8_t  config_get_status_led_brightness(void);
int      config_set_status_led_brightness(uint32_t brightness);

// Captive-portal SoftAP identity. Password is empty for an open AP; otherwise
// it must be an ASCII WPA2 passphrase of 8..63 characters.
const char *config_get_wifi_ap_ssid(void);
const char *config_get_wifi_ap_password(void);
int         config_set_wifi_ap_password(const char *password);

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
