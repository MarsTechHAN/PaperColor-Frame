// config_store.c — NVS-backed persistent settings.

#include "config_store.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"

static const char *TAG     = "config_store";
static const char *NS      = "paper_e6";
static const char *K_LOOP  = "loop_int";
static const char *K_CALIB = "calib_json";
static const char *K_WIFI_IDLE_SLEEP = "wifi_sleep";
static const char *K_WIFI_MODE       = "wifi_mode";
static const char *K_WIFI_AP_PASS    = "ap_pass";
static const char *K_WIFI_STA_SSID   = "sta_ssid";
static const char *K_WIFI_STA_PASS   = "sta_pass";
static const char *K_BATTERY_ICON    = "bat_icon";

static uint32_t s_loop_interval_s = CONFIG_LOOP_INTERVAL_DEFAULT;
static uint32_t s_wifi_idle_sleep_s = CONFIG_WIFI_IDLE_SLEEP_DEFAULT_S;
static config_wifi_mode_t s_wifi_mode = CONFIG_WIFI_MODE_AP;
static char     s_wifi_ap_ssid[CONFIG_WIFI_SSID_MAX_LEN + 1] = {0};
static char     s_wifi_ap_password[CONFIG_WIFI_AP_PASSWORD_MAX_LEN + 1] = {0};
static char     s_wifi_sta_ssid[CONFIG_WIFI_SSID_MAX_LEN + 1] = {0};
static char     s_wifi_sta_password[CONFIG_WIFI_STA_PASSWORD_MAX_LEN + 1] = {0};
static bool     s_battery_icon_enabled = true;

static uint32_t snap_to_allowed(uint32_t v, const uint32_t *allowed, size_t n)
{
    uint32_t best = allowed[0];
    uint32_t best_delta = (v > best) ? (v - best) : (best - v);
    for (size_t i = 1; i < n; i++) {
        uint32_t candidate = allowed[i];
        uint32_t delta = (v > candidate) ? (v - candidate) : (candidate - v);
        if (delta < best_delta) {
            best = candidate;
            best_delta = delta;
        }
    }
    return best;
}

static uint32_t snap_loop_interval(uint32_t v)
{
    static const uint32_t allowed[] = {
        300, 600, 900, 1800, 3600, 7200, 21600,
    };
    if (v == CONFIG_LOOP_INTERVAL_NEVER_S) {
        return CONFIG_LOOP_INTERVAL_NEVER_S;
    }
    return snap_to_allowed(v, allowed, sizeof(allowed) / sizeof(allowed[0]));
}

static uint32_t snap_wifi_idle_sleep(uint32_t v)
{
    static const uint32_t allowed[] = {
        30, 60, 300, 600, 900,
    };
    return snap_to_allowed(v, allowed, sizeof(allowed) / sizeof(allowed[0]));
}

static bool ascii_printable_is_valid(const char *s, size_t max_len, bool allow_empty)
{
    if (!s) return false;
    size_t len = strlen(s);
    if (len == 0) return allow_empty;
    if (len > max_len) return false;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 32 || c > 126) return false;
    }
    return true;
}

static bool wifi_password_is_valid(const char *password, size_t max_len)
{
    if (!password) return false;
    size_t len = strlen(password);
    if (len == 0) return true;
    if (len < 8 || len > max_len) return false;
    return ascii_printable_is_valid(password, max_len, false);
}

static bool wifi_ssid_is_valid(const char *ssid)
{
    return ascii_printable_is_valid(ssid, CONFIG_WIFI_SSID_MAX_LEN, true);
}

static bool wifi_mode_is_valid(config_wifi_mode_t mode)
{
    return mode == CONFIG_WIFI_MODE_AP ||
           mode == CONFIG_WIFI_MODE_STA ||
           mode == CONFIG_WIFI_MODE_STA_AP;
}

// Per-device SSID suffix from the SoftAP MAC's last two bytes. Result is the
// same across reboots; recovered without NVS so a wiped device still shows the
// same label printed on the bottom sticker.
static void derive_ap_ssid(char *out, size_t out_sz)
{
    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP) != ESP_OK) {
        esp_fill_random(mac + 4, 2);
    }
    snprintf(out, out_sz, "%s-%02X%02X",
             CONFIG_WIFI_AP_SSID_DEFAULT, mac[4], mac[5]);
}


// Default calibration JSON shipped with the firmware.  Only RGB is stored —
// LAB is derived at dither time from the same C/WASM rgb_to_lab_u8() that
// the algorithm uses, which keeps a single source of truth for colour math.
// The original JSON shipped with hand-typed LAB values that disagreed with
// sRGB→LAB conversion (e.g. yellow had b=-32 ≈ blue, not +69 ≈ yellow); that
// caused warm skin tones to dither to yellow ink.  Storing only RGB removes
// the entire class of stale-LAB bugs.
//
// RGB values come from PaperColor-Cali-Data-20260521.xlsx, Sheet1, "Read from machine".
static const char DEFAULT_CALIB_JSON[] =
"{\"palette\":{\"black\":{\"rgb\":[72,71,82]},\"white\":{\"rgb\":[151,161,161]},\"yellow\":{\"rgb\":[164,154,69"
"]},\"red\":{\"rgb\":[120,70,71]},\"blue\":{\"rgb\":[60,99,136]},\"green\":{\"rgb\":[79,103,77]}},\"adjust\":{\""
"brightness\":0,\"exposure\":0,\"contrast\":106,\"saturation\":136,\"vibrance\":64,\"gamma\":96,\"temperature"
"\":106,\"tint\":99,\"smoothness\":10,\"sharpen\":34,\"vignette\":14},\"auto\":{\"exposureBias\":-33,\"temperat"
"ureBias\":0},\"pipeline\":{\"ditherMode\":\"e6-mix\"},\"mixtures\":{\"version\":2,\"input\":\"rgb\",\"source\":\"P"
"aperColor-Cali-Data-20260521.xlsx / Read from machine\",\"patches\":{\"white_black_25\":{\"rgb\":[130,1"
"38,142]},\"white_black_50\":{\"rgb\":[112,118,123]},\"white_black_75\":{\"rgb\":[93,96,104]},\"white_yell"
"ow_25\":{\"rgb\":[155,159,141]},\"white_yellow_50\":{\"rgb\":[158,157,121]},\"white_yellow_75\":{\"rgb\":[1"
"61,156,97]},\"white_red_25\":{\"rgb\":[143,142,141]},\"white_red_50\":{\"rgb\":[136,122,120]},\"white_red"
"_75\":{\"rgb\":[129,99,97]},\"white_blue_25\":{\"rgb\":[133,148,155]},\"white_blue_50\":{\"rgb\":[113,134,1"
"49]},\"white_blue_75\":{\"rgb\":[90,118,143]},\"white_green_25\":{\"rgb\":[141,151,140]},\"white_green_50"
"\":{\"rgb\":[127,140,121]},\"white_green_75\":{\"rgb\":[128,140,121]},\"black_yellow_50\":{\"rgb\":[126,118"
",81]},\"black_red_50\":{\"rgb\":[96,72,78]},\"black_blue_50\":{\"rgb\":[70,81,104]},\"black_green_50\":{\"r"
"gb\":[80,93,95]},\"yellow_red_50\":{\"rgb\":[146,122,72]},\"yellow_blue_50\":{\"rgb\":[125,132,111]},\"yel"
"low_green_50\":{\"rgb\":[132,137,79]},\"red_blue_50\":{\"rgb\":[95,84,106]},\"red_green_50\":{\"rgb\":[103,"
"96,81]},\"blue_green_50\":{\"rgb\":[73,107,118]},\"skin_wyr\":{\"rgb\":[151,153,136]},\"sky_wb\":{\"rgb\":[1"
"32,147,156]},\"foliage_gyk\":{\"rgb\":[108,119,87]}}}}";

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
        if (nvs_get_u32(h, K_WIFI_MODE, &v) == ESP_OK &&
            wifi_mode_is_valid((config_wifi_mode_t)v)) {
            s_wifi_mode = (config_wifi_mode_t)v;
        }
        if (nvs_get_u32(h, K_BATTERY_ICON, &v) == ESP_OK) {
            s_battery_icon_enabled = v != 0;
        }
        size_t pass_len = sizeof s_wifi_ap_password;
        if (nvs_get_str(h, K_WIFI_AP_PASS, s_wifi_ap_password, &pass_len) != ESP_OK ||
            !wifi_password_is_valid(s_wifi_ap_password, CONFIG_WIFI_AP_PASSWORD_MAX_LEN) ||
            s_wifi_ap_password[0] == 0) {
            s_wifi_ap_password[0] = 0;
        }
        size_t sta_ssid_len = sizeof s_wifi_sta_ssid;
        if (nvs_get_str(h, K_WIFI_STA_SSID, s_wifi_sta_ssid, &sta_ssid_len) != ESP_OK ||
            !wifi_ssid_is_valid(s_wifi_sta_ssid)) {
            s_wifi_sta_ssid[0] = 0;
        }
        size_t sta_pass_len = sizeof s_wifi_sta_password;
        if (nvs_get_str(h, K_WIFI_STA_PASS, s_wifi_sta_password, &sta_pass_len) != ESP_OK ||
            !wifi_password_is_valid(s_wifi_sta_password, CONFIG_WIFI_STA_PASSWORD_MAX_LEN)) {
            s_wifi_sta_password[0] = 0;
        }
        nvs_close(h);
    }
    s_loop_interval_s = snap_loop_interval(s_loop_interval_s);
    s_wifi_idle_sleep_s = snap_wifi_idle_sleep(s_wifi_idle_sleep_s);

    derive_ap_ssid(s_wifi_ap_ssid, sizeof s_wifi_ap_ssid);

    // Factory default is an open AP — empty password. Users who want WPA2 can
    // set one from the Settings page; that path stays through
    // config_set_wifi_ap_password() and persists to NVS as before.

    ESP_LOGI(TAG, "config: loop=%us wifi_sleep=%us wifi_mode=%s ap_ssid=%s sta_ssid=%s ap_pass=%s battery_icon=%d",
             (unsigned)s_loop_interval_s,
             (unsigned)s_wifi_idle_sleep_s,
             config_get_wifi_mode_string(),
             s_wifi_ap_ssid,
             s_wifi_sta_ssid[0] ? s_wifi_sta_ssid : "(none)",
             s_wifi_ap_password[0] ? "set" : "open",
             s_battery_icon_enabled ? 1 : 0);

    // Print AP credentials in cleartext so a user who forgot the password can
    // recover it over UART. Serial access already implies physical access, so
    // this does not change the security posture. Keep it grep-friendly.
    ESP_LOGI(TAG, "==== AP credentials ====");
    ESP_LOGI(TAG, "  SSID:     %s", s_wifi_ap_ssid);
    ESP_LOGI(TAG, "  password: %s", s_wifi_ap_password[0] ? s_wifi_ap_password : "(open)");
    ESP_LOGI(TAG, "========================");
    return 0;
}

uint32_t config_get_loop_interval_s(void)
{
    return s_loop_interval_s;
}

int config_set_loop_interval_s(uint32_t s)
{
    s = snap_loop_interval(s);
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
    s = snap_wifi_idle_sleep(s);
    s_wifi_idle_sleep_s = s;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return -1;
    nvs_set_u32(h, K_WIFI_IDLE_SLEEP, s);
    nvs_commit(h);
    nvs_close(h);
    return 0;
}

config_wifi_mode_t config_get_wifi_mode(void)
{
    return s_wifi_mode;
}

const char *config_get_wifi_mode_string(void)
{
    switch (s_wifi_mode) {
    case CONFIG_WIFI_MODE_STA:    return "sta";
    case CONFIG_WIFI_MODE_STA_AP: return "sta_ap";
    case CONFIG_WIFI_MODE_AP:
    default:                      return "ap";
    }
}

int config_set_wifi_mode(config_wifi_mode_t mode)
{
    if (!wifi_mode_is_valid(mode)) return -1;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return -1;
    esp_err_t e = nvs_set_u32(h, K_WIFI_MODE, (uint32_t)mode);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    if (e != ESP_OK) return -1;
    s_wifi_mode = mode;
    return 0;
}

int config_set_wifi_mode_string(const char *mode)
{
    if (!mode) return -1;
    if (strcmp(mode, "ap") == 0) return config_set_wifi_mode(CONFIG_WIFI_MODE_AP);
    if (strcmp(mode, "sta") == 0) return config_set_wifi_mode(CONFIG_WIFI_MODE_STA);
    if (strcmp(mode, "sta_ap") == 0 || strcmp(mode, "sta+ap") == 0) {
        return config_set_wifi_mode(CONFIG_WIFI_MODE_STA_AP);
    }
    return -1;
}

const char *config_get_wifi_ap_ssid(void)
{
    return s_wifi_ap_ssid[0] ? s_wifi_ap_ssid : CONFIG_WIFI_AP_SSID_DEFAULT;
}

const char *config_get_wifi_ap_password(void)
{
    return s_wifi_ap_password;
}

static int store_string_setting(const char *key, const char *value, char *dst, size_t dst_sz)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return -1;
    esp_err_t e = nvs_set_str(h, key, value);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    if (e != ESP_OK) return -1;
    strncpy(dst, value, dst_sz - 1);
    dst[dst_sz - 1] = 0;
    return 0;
}

int config_set_wifi_ap_password(const char *password)
{
    if (!wifi_password_is_valid(password, CONFIG_WIFI_AP_PASSWORD_MAX_LEN)) return -1;
    return store_string_setting(K_WIFI_AP_PASS, password,
                                s_wifi_ap_password, sizeof s_wifi_ap_password);
}

const char *config_get_wifi_sta_ssid(void)
{
    return s_wifi_sta_ssid;
}

const char *config_get_wifi_sta_password(void)
{
    return s_wifi_sta_password;
}

int config_set_wifi_sta_ssid(const char *ssid)
{
    if (!wifi_ssid_is_valid(ssid)) return -1;
    return store_string_setting(K_WIFI_STA_SSID, ssid,
                                s_wifi_sta_ssid, sizeof s_wifi_sta_ssid);
}

int config_set_wifi_sta_password(const char *password)
{
    if (!wifi_password_is_valid(password, CONFIG_WIFI_STA_PASSWORD_MAX_LEN)) return -1;
    return store_string_setting(K_WIFI_STA_PASS, password,
                                s_wifi_sta_password, sizeof s_wifi_sta_password);
}

bool config_get_battery_icon_enabled(void)
{
    return s_battery_icon_enabled;
}

int config_set_battery_icon_enabled(bool enabled)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return -1;
    esp_err_t e = nvs_set_u32(h, K_BATTERY_ICON, enabled ? 1 : 0);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    if (e != ESP_OK) return -1;
    s_battery_icon_enabled = enabled;
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
