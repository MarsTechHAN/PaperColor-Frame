// config_store.c — NVS-backed persistent settings.

#include "config_store.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG     = "config_store";
static const char *NS      = "paper_e6";
static const char *K_LOOP  = "loop_int";
static const char *K_CALIB = "calib_json";
static const char *K_WIFI_IDLE_SLEEP = "wifi_sleep";
static const char *K_BUTTON_SLEEP    = "btn_sleep";
static const char *K_STATUS_LED      = "led_bright";
static const char *K_WIFI_AP_PASS    = "ap_pass";

static uint32_t s_loop_interval_s = CONFIG_LOOP_INTERVAL_DEFAULT;
static uint32_t s_wifi_idle_sleep_s = CONFIG_WIFI_IDLE_SLEEP_DEFAULT_S;
static uint32_t s_button_sleep_s = CONFIG_BUTTON_SLEEP_DEFAULT_S;
static uint8_t  s_status_led_brightness = CONFIG_STATUS_LED_BRIGHTNESS_DEFAULT;
static char     s_wifi_ap_password[CONFIG_WIFI_AP_PASSWORD_MAX_LEN + 1] = {0};

static uint32_t clamp_u32(uint32_t v, uint32_t lo, uint32_t hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static bool wifi_ap_password_is_valid(const char *password)
{
    if (!password) return false;
    size_t len = strlen(password);
    if (len == 0) return true;            // Empty means open AP.
    if (len < 8 || len > CONFIG_WIFI_AP_PASSWORD_MAX_LEN) return false;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)password[i];
        if (c < 32 || c > 126) return false;
    }
    return true;
}

// Default calibration JSON shipped with the firmware.  Only RGB is stored —
// LAB is derived at dither time from the same C/WASM rgb_to_lab_u8() that
// the algorithm uses, which keeps a single source of truth for colour math.
// The original JSON shipped with hand-typed LAB values that disagreed with
// sRGB→LAB conversion (e.g. yellow had b=-32 ≈ blue, not +69 ≈ yellow); that
// caused warm skin tones to dither to yellow ink.  Storing only RGB removes
// the entire class of stale-LAB bugs.
//
// RGB values are averaged from the user's three colorimeter readings.
static const char DEFAULT_CALIB_JSON[] =
"{"
"\"palette\":{"
  "\"black\":{\"rgb\":[70,71,80]},"
  "\"white\":{\"rgb\":[151,161,160]},"
  "\"yellow\":{\"rgb\":[163,154,69]},"
  "\"red\":{\"rgb\":[118,69,70]},"
  "\"blue\":{\"rgb\":[60,97,134]},"
  "\"green\":{\"rgb\":[77,100,76]}"
"},"
// Adjust defaults are now NEUTRAL.  The earlier values from
// epd6color_adjust.json (contrast=108, saturation=81, gamma=54) were dialled
// in to compensate for a dither that targeted the *idealised* sRGB palette;
// they over-darken mid-tones once the dither already maps into the panel's
// real L*≈30..65 gamut, which is what made skin tones collapse into blue.
// Default adjust:
//   * contrast/saturation are slightly lifted to compensate for reflective
//     paper softness after gamut compression.
//   * gamma=98 and temperature=108 keep mid-tones clear and a little warm.
//   * smoothness=12 preserves transition dither instead of merging colors.
"\"adjust\":{"
  "\"brightness\":0,"
  "\"exposure\":0,"
  "\"contrast\":106,"
  "\"saturation\":136,"
  "\"vibrance\":64,"
  "\"gamma\":96,"
  "\"temperature\":106,"
  "\"tint\":99,"
  "\"smoothness\":10,"
  "\"sharpen\":34,"
  "\"vignette\":14"
"},"
// Browser-side Auto carries an exposure-target compensation to protect
// Spectra 6 highlights.  Temperature bias is in the same slider units as the
// manual Temperature control.
"\"auto\":{"
  "\"exposureBias\":-33,"
  "\"temperatureBias\":0"
"},"
"\"pipeline\":{"
  "\"ditherMode\":\"e6-mix\""
"}"
"}";

int config_store_init(void)
{
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        e = nvs_flash_init();
    }
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init: %s", esp_err_to_name(e));
        return -1;
    }
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        uint32_t v;
        if (nvs_get_u32(h, K_LOOP, &v) == ESP_OK) {
            s_loop_interval_s = v;
        }
        if (nvs_get_u32(h, K_WIFI_IDLE_SLEEP, &v) == ESP_OK) {
            s_wifi_idle_sleep_s = v;
        }
        if (nvs_get_u32(h, K_BUTTON_SLEEP, &v) == ESP_OK) {
            s_button_sleep_s = v;
        }
        if (nvs_get_u32(h, K_STATUS_LED, &v) == ESP_OK) {
            s_status_led_brightness = (uint8_t)v;
        }
        size_t pass_len = sizeof s_wifi_ap_password;
        if (nvs_get_str(h, K_WIFI_AP_PASS, s_wifi_ap_password, &pass_len) != ESP_OK ||
            !wifi_ap_password_is_valid(s_wifi_ap_password)) {
            s_wifi_ap_password[0] = 0;
        }
        nvs_close(h);
    }
    s_loop_interval_s = clamp_u32(s_loop_interval_s,
                                  CONFIG_LOOP_INTERVAL_MIN_S,
                                  CONFIG_LOOP_INTERVAL_MAX_S);
    s_wifi_idle_sleep_s = clamp_u32(s_wifi_idle_sleep_s,
                                    CONFIG_WIFI_IDLE_SLEEP_MIN_S,
                                    CONFIG_WIFI_IDLE_SLEEP_MAX_S);
    s_button_sleep_s = clamp_u32(s_button_sleep_s,
                                 CONFIG_BUTTON_SLEEP_MIN_S,
                                 CONFIG_BUTTON_SLEEP_MAX_S);
    s_status_led_brightness = (uint8_t)clamp_u32(s_status_led_brightness,
                                                 CONFIG_STATUS_LED_BRIGHTNESS_MIN,
                                                 CONFIG_STATUS_LED_BRIGHTNESS_MAX);
    ESP_LOGI(TAG, "config: loop=%us wifi_sleep=%us button_sleep=%us led=%u ap_pass=%s",
             (unsigned)s_loop_interval_s,
             (unsigned)s_wifi_idle_sleep_s,
             (unsigned)s_button_sleep_s,
             (unsigned)s_status_led_brightness,
             s_wifi_ap_password[0] ? "set" : "open");
    return 0;
}

uint32_t config_get_loop_interval_s(void)
{
    return s_loop_interval_s;
}

int config_set_loop_interval_s(uint32_t s)
{
    s = clamp_u32(s, CONFIG_LOOP_INTERVAL_MIN_S, CONFIG_LOOP_INTERVAL_MAX_S);
    s_loop_interval_s = s;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return -1;
    nvs_set_u32(h, K_LOOP, s);
    nvs_commit(h);
    nvs_close(h);
    return 0;
}

uint32_t config_get_wifi_idle_sleep_s(void)
{
    return s_wifi_idle_sleep_s;
}

int config_set_wifi_idle_sleep_s(uint32_t s)
{
    s = clamp_u32(s, CONFIG_WIFI_IDLE_SLEEP_MIN_S, CONFIG_WIFI_IDLE_SLEEP_MAX_S);
    s_wifi_idle_sleep_s = s;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return -1;
    nvs_set_u32(h, K_WIFI_IDLE_SLEEP, s);
    nvs_commit(h);
    nvs_close(h);
    return 0;
}

uint32_t config_get_button_sleep_s(void)
{
    return s_button_sleep_s;
}

int config_set_button_sleep_s(uint32_t s)
{
    s = clamp_u32(s, CONFIG_BUTTON_SLEEP_MIN_S, CONFIG_BUTTON_SLEEP_MAX_S);
    s_button_sleep_s = s;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return -1;
    nvs_set_u32(h, K_BUTTON_SLEEP, s);
    nvs_commit(h);
    nvs_close(h);
    return 0;
}

uint8_t config_get_status_led_brightness(void)
{
    return s_status_led_brightness;
}

int config_set_status_led_brightness(uint32_t brightness)
{
    brightness = clamp_u32(brightness,
                           CONFIG_STATUS_LED_BRIGHTNESS_MIN,
                           CONFIG_STATUS_LED_BRIGHTNESS_MAX);
    s_status_led_brightness = (uint8_t)brightness;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return -1;
    nvs_set_u32(h, K_STATUS_LED, brightness);
    nvs_commit(h);
    nvs_close(h);
    return 0;
}

const char *config_get_wifi_ap_ssid(void)
{
    return CONFIG_WIFI_AP_SSID_DEFAULT;
}

const char *config_get_wifi_ap_password(void)
{
    return s_wifi_ap_password;
}

int config_set_wifi_ap_password(const char *password)
{
    if (!wifi_ap_password_is_valid(password)) return -1;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return -1;
    esp_err_t e = nvs_set_str(h, K_WIFI_AP_PASS, password);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    if (e != ESP_OK) return -1;
    strncpy(s_wifi_ap_password, password, sizeof s_wifi_ap_password - 1);
    s_wifi_ap_password[sizeof s_wifi_ap_password - 1] = 0;
    return 0;
}

int config_get_calib_json(char *out, size_t out_sz)
{
    if (!out || out_sz < 2) return -1;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        size_t len = out_sz;
        esp_err_t e = nvs_get_str(h, K_CALIB, out, &len);
        nvs_close(h);
        if (e == ESP_OK) {
            // nvs_get_str writes the trailing NUL inside `len`; return the
            // bytes excluding it for snprintf parity.
            return (int)(len > 0 ? len - 1 : 0);
        }
    }
    // Fall back to the firmware default.
    size_t dlen = sizeof DEFAULT_CALIB_JSON - 1;
    if (dlen >= out_sz) dlen = out_sz - 1;
    memcpy(out, DEFAULT_CALIB_JSON, dlen);
    out[dlen] = 0;
    return (int)dlen;
}

int config_set_calib_json(const char *json, size_t len)
{
    if (!json || len == 0 || len > CONFIG_CALIB_MAX_LEN) return -1;
    // Defensive NUL-termination: nvs_set_str expects a C string.
    char *tmp = malloc(len + 1);
    if (!tmp) return -1;
    memcpy(tmp, json, len);
    tmp[len] = 0;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        free(tmp);
        return -1;
    }
    esp_err_t e = nvs_set_str(h, K_CALIB, tmp);
    if (e == ESP_OK) nvs_commit(h);
    nvs_close(h);
    free(tmp);
    return e == ESP_OK ? 0 : -1;
}
