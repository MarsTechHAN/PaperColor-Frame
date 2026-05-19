// config_store.c — NVS-backed persistent settings.

#include "config_store.h"

#include <string.h>

#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG     = "config_store";
static const char *NS      = "paper_e6";
static const char *K_LOOP  = "loop_int";
static const char *K_CALIB = "calib_json";

static uint32_t s_loop_interval_s = CONFIG_LOOP_INTERVAL_DEFAULT;

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
  "\"saturation\":106,"
  "\"vibrance\":24,"
  "\"gamma\":96,"
  "\"temperature\":106,"
  "\"tint\":99,"
  "\"smoothness\":10,"
  "\"sharpen\":34,"
  "\"vignette\":14"
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
        nvs_close(h);
    }
    if (s_loop_interval_s < CONFIG_LOOP_INTERVAL_MIN_S
        || s_loop_interval_s > CONFIG_LOOP_INTERVAL_MAX_S) {
        s_loop_interval_s = CONFIG_LOOP_INTERVAL_DEFAULT;
    }
    ESP_LOGI(TAG, "loop_interval_s=%u", (unsigned)s_loop_interval_s);
    return 0;
}

uint32_t config_get_loop_interval_s(void)
{
    return s_loop_interval_s;
}

int config_set_loop_interval_s(uint32_t s)
{
    if (s < CONFIG_LOOP_INTERVAL_MIN_S) s = CONFIG_LOOP_INTERVAL_MIN_S;
    if (s > CONFIG_LOOP_INTERVAL_MAX_S) s = CONFIG_LOOP_INTERVAL_MAX_S;
    s_loop_interval_s = s;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return -1;
    nvs_set_u32(h, K_LOOP, s);
    nvs_commit(h);
    nvs_close(h);
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
