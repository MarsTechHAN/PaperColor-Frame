// http_server.c — esp_http_server wired up with:
//   * gzipped single-page web UI at "/"
//   * captive-portal probe redirects (iOS / Android / Windows)
//   * /api/list, /api/photo/<name>, /api/upload/<name>, /api/display/<name>,
//     /api/next, /api/config
//
// Conventions:
//   * Names are short ASCII, validated by photo_store_path_for().
//   * Upload payload is the raw JPG bytes (Content-Type: image/jpeg) — the
//     browser does the resize/crop, so the device just streams to disk.
//   * /api/photo/<name> always serves the file with the right MIME so the
//     browser caches between thumbnail tiles and the detail view.

#include "http_server.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <sys/stat.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_app_desc.h"
#include "esp_timer.h"

#include "photo_store.h"
#include "config_store.h"
#include "loop_display.h"
#include "sd_storage.h"

// Wall-clock helpers for handler timing.  We log a one-line summary per
// request when it crosses HTTP_SLOW_LOG_MS so the serial trail surfaces the
// stalls users see without spamming the log on fast requests.
#define HTTP_SLOW_LOG_MS  150
static inline int64_t now_us(void) { return esp_timer_get_time(); }
static inline int     us_to_ms(int64_t us) { return (int)(us / 1000); }

static const char *TAG = "http";

// Embedded gzipped web UI (see main/CMakeLists.txt EMBED_FILES).
extern const uint8_t INDEX_GZ_START[] asm("_binary_index_html_gz_start");
extern const uint8_t INDEX_GZ_END[]   asm("_binary_index_html_gz_end");
extern const uint8_t WASM_GZ_START[]  asm("_binary_dither_wasm_gz_start");
extern const uint8_t WASM_GZ_END[]    asm("_binary_dither_wasm_gz_end");
extern const uint8_t I18N_GZ_START[]  asm("_binary_i18n_js_gz_start");
extern const uint8_t I18N_GZ_END[]    asm("_binary_i18n_js_gz_end");

// -------------------------------------------------------------------- helpers
static esp_err_t send_text(httpd_req_t *req, const char *status,
                           const char *mime, const char *body)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, mime);
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t send_json(httpd_req_t *req, const char *body)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t send_photo_store_error(httpd_req_t *req, const char *fallback)
{
    if (photo_store_last_error() == PHOTO_STORE_ERR_NO_SPACE) {
        return send_text(req, "507 Insufficient Storage", "text/plain", "not enough storage");
    }
    return send_text(req, "500 Internal Server Error", "text/plain", fallback);
}

static int hex_nibble(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Pull the last path segment as a NUL-terminated name, percent-decoding %XX
// escapes. Every JS caller wraps the name in encodeURIComponent(), so a name
// containing a space / '%' / non-ASCII byte arrives percent-encoded; without
// decoding here the on-disk name, its .bin/.json siblings and the /api/list
// entry would all diverge from what /api/display later asks for, and the photo
// would silently never show. Decoding restores the encodeURIComponent contract.
// Subsequent name_is_safe() validation rejects anything dangerous in the result.
static int url_tail_name(const char *uri, const char *prefix,
                         char *out, size_t out_sz)
{
    size_t pl = strlen(prefix);
    if (strncmp(uri, prefix, pl) != 0) return -1;
    const char *p = uri + pl;
    // strip any query string
    const char *q = strchr(p, '?');
    size_t n = q ? (size_t)(q - p) : strlen(p);
    if (n == 0) return -1;

    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        char c = p[i];
        if (c == '%') {
            if (i + 2 >= n) return -1;            // truncated escape
            int hi = hex_nibble((unsigned char)p[i + 1]);
            int lo = hex_nibble((unsigned char)p[i + 2]);
            if (hi < 0 || lo < 0) return -1;       // malformed escape
            c = (char)((hi << 4) | lo);
            i += 2;
        }
        if (o + 1 >= out_sz) return -1;            // would overflow out buffer
        out[o++] = c;
    }
    out[o] = 0;
    return 0;
}

static int json_get_string_field(const char *json, const char *key,
                                 char *out, size_t out_sz)
{
    if (!json || !key || !out || out_sz == 0) return -1;
    char needle[48];
    int nn = snprintf(needle, sizeof needle, "\"%s\"", key);
    if (nn < 0 || nn >= (int)sizeof needle) return -1;
    const char *p = strstr(json, needle);
    if (!p) return 0;
    p += nn;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p++ != ':') return -1;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p++ != '"') return -1;

    size_t n = 0;
    while (*p && *p != '"') {
        unsigned char c = (unsigned char)*p++;
        if (c == '\\') {
            c = (unsigned char)*p++;
            if (c == '"' || c == '\\' || c == '/') {
                // Keep escaped printable character as-is.
            } else if (c == 'n' || c == 'r' || c == 't' ||
                       c == 'b' || c == 'f' || c == 'u') {
                return -1; // Wi-Fi passphrases are restricted to printable ASCII.
            } else {
                return -1;
            }
        }
        if (n + 1 >= out_sz) return -1;
        out[n++] = (char)c;
    }
    if (*p != '"') return -1;
    out[n] = 0;
    return 1;
}


static int json_get_bool_field(const char *json, const char *key, bool *out)
{
    if (!json || !key || !out) return -1;
    char needle[48];
    int nn = snprintf(needle, sizeof needle, "\"%s\"", key);
    if (nn < 0 || nn >= (int)sizeof needle) return -1;
    const char *p = strstr(json, needle);
    if (!p) return 0;
    p += nn;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p++ != ':') return -1;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (strncmp(p, "true", 4) == 0) { *out = true; return 1; }
    if (strncmp(p, "false", 5) == 0) { *out = false; return 1; }
    if (*p == '1') { *out = true; return 1; }
    if (*p == '0') { *out = false; return 1; }
    return -1;
}

static void json_escape_string(char *out, size_t out_sz, const char *in)
{
    if (!out || out_sz == 0) return;
    size_t off = 0;
    if (!in) in = "";
    for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
        unsigned char c = *p;
        if (c == '"' || c == '\\') {
            if (off + 2 >= out_sz) break;
            out[off++] = '\\';
            out[off++] = (char)c;
        } else if (c >= 32) {
            if (off + 1 >= out_sz) break;
            out[off++] = (char)c;
        }
    }
    out[off] = 0;
}

// -------------------------------------------------------------------- routes

// GET / — serve the gzipped UI bundle.
//
// Cache-Control: no-store — the UI changes between firmware revisions and
// captive-portal browsers don't expose a "hard reload" gesture, so any
// caching here strands users on stale UIs. The HTML is tiny (~11 KB gz);
// the cost of always re-fetching is negligible.
static esp_err_t h_index(httpd_req_t *req)
{
    const size_t n = INDEX_GZ_END - INDEX_GZ_START;
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, (const char *)INDEX_GZ_START, n);
}

// GET /dither.wasm — gzip-compressed WASM dither module.
//
// Cache-Control: no-store — the WASM ABI evolves alongside the JS in
// index.html (new exports, struct layouts, etc.).  A stale cached WASM
// without the symbols the new JS expects manifests as a generic "Upload
// failed" with no device-side log, which is hellish to debug remotely.
// The module is tiny (~27 KB gz); refetching per session is cheap.
static esp_err_t h_wasm(httpd_req_t *req)
{
    const size_t n = WASM_GZ_END - WASM_GZ_START;
    httpd_resp_set_type(req, "application/wasm");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, (const char *)WASM_GZ_START, n);
}


// GET /i18n.js — gzip-compressed translation bundle.
static esp_err_t h_i18n(httpd_req_t *req)
{
    const size_t n = I18N_GZ_END - I18N_GZ_START;
    httpd_resp_set_type(req, "application/javascript; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, (const char *)I18N_GZ_START, n);
}

// GET /api/list  → {"active":"...", "photos":[{name,size,mtime},...]}
static esp_err_t h_list(httpd_req_t *req)
{
    int64_t t0 = now_us();
    if (!sd_storage_fs_acquire()) {
        return send_text(req, "503 Service Unavailable", "text/plain", "storage busy");
    }
    photo_meta_t list[64];
    int n = photo_store_list(list, 64);
    sd_storage_fs_release();
    char act[PHOTO_NAME_MAX];
    photo_store_copy_active(act, sizeof act);
    int64_t t_scan = now_us();
    if (n < 0) return send_json(req, "{\"photos\":[],\"active\":null}");

    // Build JSON manually — keeps deps lean.  Budget for the worst case so a
    // long filename can never make snprintf truncate: a truncated write still
    // advances `off` by the *intended* length, which would underflow
    // `cap - off` on the next call and overflow the heap buffer.  Names are
    // JSON-escaped (a '"' or '\\' doubles in length), so reserve 2x the name
    // budget per entry plus the JSON skeleton and two uint32s.
    size_t cap = 128 + 2 * PHOTO_NAME_MAX + (size_t)n * (2 * PHOTO_NAME_MAX + 64);
    char *buf = malloc(cap);
    if (!buf) return ESP_FAIL;
    size_t off = 0;
    char act_esc[PHOTO_NAME_MAX * 2];
    if (act[0]) json_escape_string(act_esc, sizeof act_esc, act);
    int w = snprintf(buf + off, cap - off,
                     "{\"active\":%s%s%s,\"photos\":[",
                     act[0] ? "\"" : "",
                     act[0] ? act_esc : "null",
                     act[0] ? "\"" : "");
    if (w > 0) off += (size_t)w;
    for (int i = 0; i < n && off < cap; i++) {
        char name_esc[PHOTO_NAME_MAX * 2];
        json_escape_string(name_esc, sizeof name_esc, list[i].name);
        w = snprintf(buf + off, cap - off,
                     "%s{\"name\":\"%s\",\"size\":%u,\"mtime\":%u}",
                     i ? "," : "",
                     name_esc,
                     (unsigned)list[i].size,
                     (unsigned)list[i].mtime);
        if (w < 0) break;
        off += (size_t)w;
    }
    if (off >= cap) off = cap - 1;   // defensive: never index past the buffer
    snprintf(buf + off, cap - off, "]}");
    esp_err_t rc = send_json(req, buf);
    free(buf);
    int total_ms = us_to_ms(now_us() - t0);
    if (total_ms >= HTTP_SLOW_LOG_MS) {
        ESP_LOGW(TAG, "slow /api/list: %d photos, scan=%d ms, total=%d ms",
                 n, us_to_ms(t_scan - t0), total_ms);
    }
    return rc;
}

// GET /api/photo/<name>  → stream the raw JPG.
static esp_err_t h_photo_get(httpd_req_t *req)
{
    int64_t t0 = now_us();
    char name[PHOTO_NAME_MAX];
    if (url_tail_name(req->uri, "/api/photo/", name, sizeof name) != 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_send(req, "bad name", HTTPD_RESP_USE_STRLEN);
    }
    char path[PHOTO_PATH_MAX + 32];
    if (photo_store_path_for(name, path, sizeof path) != 0) {
        httpd_resp_set_status(req, "404 Not Found");
        return httpd_resp_send(req, "no", HTTPD_RESP_USE_STRLEN);
    }
    // Hold the FS guard for the whole open→read→close window so a concurrent
    // panel recovery on the worker cannot unmount the filesystem out from under
    // our open FILE* (see sd_storage_fs_acquire).
    if (!sd_storage_fs_acquire()) {
        return send_text(req, "503 Service Unavailable", "text/plain", "storage busy");
    }
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        sd_storage_fs_release();
        httpd_resp_set_status(req, "404 Not Found");
        return httpd_resp_send(req, "no", HTTPD_RESP_USE_STRLEN);
    }
    const char *mime = "image/jpeg";
    const char *ext = strrchr(name, '.');
    if (ext && strcasecmp(ext, ".bmp") == 0) mime = "image/bmp";
    httpd_resp_set_type(req, mime);
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=86400");

    // Wider chunks: fewer httpd_resp_send_chunk syscalls and fewer LittleFS
    // page reads per HTTP frame.  8 KiB matches one TCP segment on typical
    // browsers and stays well under the 16 KiB httpd stack budget.
    char chunk[8192];
    size_t total = 0;
    size_t r;
    while ((r = fread(chunk, 1, sizeof chunk, fp)) > 0) {
        if (httpd_resp_send_chunk(req, chunk, r) != ESP_OK) {
            fclose(fp);
            sd_storage_fs_release();
            return ESP_FAIL;
        }
        total += r;
    }
    fclose(fp);
    sd_storage_fs_release();
    httpd_resp_send_chunk(req, NULL, 0);
    int total_ms = us_to_ms(now_us() - t0);
    if (total_ms >= HTTP_SLOW_LOG_MS) {
        ESP_LOGW(TAG, "slow /api/photo GET %s: %u bytes in %d ms",
                 name, (unsigned)total, total_ms);
    }
    return ESP_OK;
}

// POST /api/upload/<name>  → save to <active-store>/photos/<name>.tmp, then rename.
//
// We log phase timing (recv + write + close+rename) so a slow upload tells us
// whether the network or the filesystem stalled.  An 8 KiB recv buffer keeps
// LittleFS writes aligned with the 4 KiB erase block (two pages per recv,
// no read-modify-write across boundaries) and halves the recv-loop iteration
// count vs. the old 2 KiB.
static esp_err_t h_upload(httpd_req_t *req)
{
    int64_t t0 = now_us();
    char name[PHOTO_NAME_MAX];
    if (url_tail_name(req->uri, "/api/upload/", name, sizeof name) != 0) {
        return send_text(req, "400 Bad Request", "text/plain", "bad name");
    }
    // Reject a bodyless upload. A chunked / Content-Length-0 request would run
    // the recv loop zero times and then rename an empty .tmp over an existing
    // same-named photo — silently truncating it to 0 bytes while still
    // answering {"ok":true}. The browser always sets Content-Length, so this
    // only guards non-browser/proxy clients.
    if (req->content_len <= 0) {
        return send_text(req, "411 Length Required", "text/plain", "empty upload");
    }
    // Hold the FS guard for the whole open→feed→close lifetime so a concurrent
    // panel recovery cannot unmount under the open writer (see h_photo_get).
    if (!sd_storage_fs_acquire()) {
        return send_text(req, "503 Service Unavailable", "text/plain", "storage busy");
    }
    uint64_t free_bytes = 0;
    if (photo_store_free_bytes(&free_bytes) &&
        (uint64_t)req->content_len > free_bytes) {
        ESP_LOGW(TAG, "upload rejected: %s needs %d bytes, only %llu free in %s",
                 name, req->content_len, (unsigned long long)free_bytes, photo_store_dir());
        sd_storage_fs_release();
        return send_text(req, "507 Insufficient Storage", "text/plain", "not enough storage");
    }
    photo_writer_t *w = photo_writer_open(name);
    if (!w) {
        sd_storage_fs_release();
        return send_photo_store_error(req, "open failed");
    }
    int64_t t_open = now_us();

    // Allocate the recv buffer on the heap so the httpd task stack stays slim.
    // PSRAM is fine — the data goes straight to flash after the next loop iter.
    const int CHUNK = 8192;
    char *buf = malloc(CHUNK);
    if (!buf) {
        photo_writer_close(w, false);
        sd_storage_fs_release();
        return send_text(req, "500 Internal Server Error", "text/plain", "out of memory");
    }
    int64_t recv_us = 0, write_us = 0;
    int remaining = req->content_len;
    while (remaining > 0) {
        int64_t ts = now_us();
        int got = httpd_req_recv(req, buf,
                                 remaining > CHUNK ? CHUNK : remaining);
        recv_us += now_us() - ts;
        if (got <= 0) {
            if (got == HTTPD_SOCK_ERR_TIMEOUT) continue;
            photo_writer_close(w, false);
            free(buf);
            sd_storage_fs_release();
            return ESP_FAIL;
        }
        ts = now_us();
        if (photo_writer_feed(w, (uint8_t *)buf, got) != 0) {
            photo_writer_close(w, false);
            free(buf);
            sd_storage_fs_release();
            return send_photo_store_error(req, "write failed");
        }
        write_us += now_us() - ts;
        remaining -= got;
    }
    free(buf);
    int64_t t_loop_end = now_us();
    if (photo_writer_close(w, true) != 0) {
        sd_storage_fs_release();
        return send_photo_store_error(req, "rename failed");
    }
    sd_storage_fs_release();
    int64_t t_done = now_us();
    int total_ms = us_to_ms(t_done - t0);
    // Always log uploads — they're rare and the timing is gold for tuning.
    ESP_LOGI(TAG, "upload ok: %s %d B in %d ms (open=%d recv=%d write=%d close=%d)",
             name, req->content_len, total_ms,
             us_to_ms(t_open - t0),
             us_to_ms(recv_us),
             us_to_ms(write_us),
             us_to_ms(t_done - t_loop_end));
    return send_json(req, "{\"ok\":true}");
}

// DELETE /api/photo/<name>
static esp_err_t h_photo_del(httpd_req_t *req)
{
    int64_t t0 = now_us();
    char name[PHOTO_NAME_MAX];
    if (url_tail_name(req->uri, "/api/photo/", name, sizeof name) != 0) {
        return send_text(req, "400 Bad Request", "text/plain", "bad name");
    }
    if (!sd_storage_fs_acquire()) {
        return send_text(req, "503 Service Unavailable", "text/plain", "storage busy");
    }
    int del_rc = photo_store_delete(name);
    sd_storage_fs_release();
    if (del_rc != 0) {
        return send_text(req, "500 Internal Server Error", "text/plain", "delete failed");
    }
    esp_err_t rc = send_json(req, "{\"ok\":true}");
    int total_ms = us_to_ms(now_us() - t0);
    if (total_ms >= HTTP_SLOW_LOG_MS) {
        ESP_LOGW(TAG, "slow /api/photo DELETE %s: %d ms", name, total_ms);
    }
    return rc;
}

// POST /api/display/<name>  → queue an explicit refresh.
static esp_err_t h_display(httpd_req_t *req)
{
    char name[PHOTO_NAME_MAX];
    if (url_tail_name(req->uri, "/api/display/", name, sizeof name) != 0) {
        return send_text(req, "400 Bad Request", "text/plain", "bad name");
    }
    char path[PHOTO_PATH_MAX + 32];
    if (photo_store_path_for(name, path, sizeof path) != 0) {
        return send_text(req, "400 Bad Request", "text/plain", "bad name");
    }
    if (!sd_storage_fs_acquire()) {
        return send_text(req, "503 Service Unavailable", "text/plain", "storage busy");
    }
    struct stat st;
    int strc = stat(path, &st);
    sd_storage_fs_release();
    if (strc != 0) {
        return send_text(req, "404 Not Found", "text/plain", "no such photo");
    }
    if (loop_display_request_show(name) != 0) {
        return send_text(req, "503 Service Unavailable", "text/plain", "busy");
    }
    return send_json(req, "{\"ok\":true}");
}

// POST /api/next
static esp_err_t h_next(httpd_req_t *req)
{
    if (loop_display_request_next() != 0) {
        return send_text(req, "503 Service Unavailable", "text/plain", "busy");
    }
    return send_json(req, "{\"ok\":true}");
}

// POST /api/calib-fill?ink=<code>  — push a solid full-screen ink to the
// EPD so the user has a real-world reference to match against in the
// calibration wizard.  ink is the 4-bit panel code (0,1,2,3,5,6).
static esp_err_t h_calib_fill(httpd_req_t *req)
{
    char q[32] = {0};
    size_t qlen = httpd_req_get_url_query_len(req);
    if (qlen == 0 || qlen >= sizeof q) {
        return send_text(req, "400 Bad Request", "text/plain", "missing ink");
    }
    httpd_req_get_url_query_str(req, q, sizeof q);
    char inkv[4] = {0};
    if (httpd_query_key_value(q, "ink", inkv, sizeof inkv) != ESP_OK) {
        return send_text(req, "400 Bad Request", "text/plain", "missing ink");
    }
    int ink = atoi(inkv);
    if (ink != 0 && ink != 1 && ink != 2 && ink != 3 && ink != 5 && ink != 6) {
        return send_text(req, "400 Bad Request", "text/plain", "bad ink code");
    }
    if (loop_display_request_fill((uint8_t)ink) != 0) {
        return send_text(req, "503 Service Unavailable", "text/plain", "busy");
    }
    return send_json(req, "{\"ok\":true}");
}

static bool calib_ink_code_ok(int ink)
{
    return ink == 0 || ink == 1 || ink == 2 || ink == 3 || ink == 5 || ink == 6;
}

// POST /api/calib-mix?mix=<ink>:<weight>,...
// Example: mix=1:50,2:50 for a white/yellow 50% halftone patch.
static esp_err_t h_calib_mix(httpd_req_t *req)
{
    char q[160] = {0};
    size_t qlen = httpd_req_get_url_query_len(req);
    if (qlen == 0 || qlen >= sizeof q) {
        return send_text(req, "400 Bad Request", "text/plain", "missing mix");
    }
    httpd_req_get_url_query_str(req, q, sizeof q);
    char mix[128] = {0};
    if (httpd_query_key_value(q, "mix", mix, sizeof mix) != ESP_OK) {
        return send_text(req, "400 Bad Request", "text/plain", "missing mix");
    }

    uint8_t inks[6] = {0};
    uint8_t weights[6] = {0};
    uint8_t n = 0;
    char *save = NULL;
    char *tok = strtok_r(mix, ",", &save);
    while (tok && n < 6) {
        char *colon = strchr(tok, ':');
        if (!colon) return send_text(req, "400 Bad Request", "text/plain", "bad mix");
        *colon = 0;
        int ink = atoi(tok);
        int wt = atoi(colon + 1);
        if (!calib_ink_code_ok(ink) || wt <= 0 || wt > 100) {
            return send_text(req, "400 Bad Request", "text/plain", "bad mix");
        }
        inks[n] = (uint8_t)ink;
        weights[n] = (uint8_t)wt;
        n++;
        tok = strtok_r(NULL, ",", &save);
    }
    if (n == 0 || tok != NULL) {
        return send_text(req, "400 Bad Request", "text/plain", "bad mix");
    }
    if (loop_display_request_mix(inks, weights, n) != 0) {
        return send_text(req, "503 Service Unavailable", "text/plain", "busy");
    }
    return send_json(req, "{\"ok\":true}");
}

// GET /api/calib  → calibration JSON (palette + default adjust)
// POST /api/calib body=<JSON>  → store it
static esp_err_t h_calib(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        char *buf = malloc(CONFIG_CALIB_MAX_LEN + 1);
        if (!buf) return ESP_FAIL;
        int n = config_get_calib_json(buf, CONFIG_CALIB_MAX_LEN + 1);
        if (n < 0) { free(buf); return ESP_FAIL; }
        esp_err_t rc = send_json(req, buf);
        free(buf);
        return rc;
    }
    // POST: read up to CONFIG_CALIB_MAX_LEN bytes, store atomically.
    int n = req->content_len;
    if (n <= 0 || n > CONFIG_CALIB_MAX_LEN) {
        return send_text(req, "400 Bad Request", "text/plain", "size");
    }
    char *buf = malloc(n + 1);
    if (!buf) return ESP_FAIL;
    int got = 0;
    while (got < n) {
        int r = httpd_req_recv(req, buf + got, n - got);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
            free(buf);
            return ESP_FAIL;
        }
        got += r;
    }
    buf[got] = 0;
    int rc = config_set_calib_json(buf, got);
    free(buf);
    if (rc != 0) {
        return send_text(req, "500 Internal Server Error", "text/plain", "store failed");
    }
    return send_json(req, "{\"ok\":true}");
}

static esp_err_t send_config_json_with_version(httpd_req_t *req)
{
    const esp_app_desc_t *app = esp_app_get_description();
    const char *ap_password = config_get_wifi_ap_password();
    const char *sta_password = config_get_wifi_sta_password();
    char ap_ssid[80];
    char sta_ssid[96];
    char fw_version[96];
    char build_time_raw[64];
    char build_time[80];
    json_escape_string(ap_ssid, sizeof ap_ssid, config_get_wifi_ap_ssid());
    json_escape_string(sta_ssid, sizeof sta_ssid, config_get_wifi_sta_ssid());
    json_escape_string(fw_version, sizeof fw_version, app ? app->version : "PaperColor Frame");
    snprintf(build_time_raw, sizeof build_time_raw, "%s %s", app ? app->date : "", app ? app->time : "");
    json_escape_string(build_time, sizeof build_time, build_time_raw);

    char body[768];
    snprintf(body, sizeof body,
             "{\"loop_interval_s\":%u,"
             "\"wifi_idle_sleep_s\":%u,"
             "\"wifi_mode\":\"%s\","
             "\"wifi_ap_ssid\":\"%s\","
             "\"wifi_ap_password_set\":%s,"
             "\"wifi_sta_ssid\":\"%s\","
             "\"wifi_sta_password_set\":%s,"
             "\"mdns_host\":\"papercolor.local\","
             "\"battery_icon_enabled\":%s,"
             "\"fw_version\":\"%s\","
             "\"build_time\":\"%s\"}",
             (unsigned)config_get_loop_interval_s(),
             (unsigned)config_get_wifi_idle_sleep_s(),
             config_get_wifi_mode_string(),
             ap_ssid,
             (ap_password && ap_password[0]) ? "true" : "false",
             sta_ssid,
             (sta_password && sta_password[0]) ? "true" : "false",
             config_get_battery_icon_enabled() ? "true" : "false",
             fw_version,
             build_time);
    return send_json(req, body);
}

// GET /api/config  -> runtime/power-management settings + firmware version.
// POST /api/config { "<integer_field>": N }
static esp_err_t h_config(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        return send_config_json_with_version(req);
    }
    // POST: read the full body (a small JSON patch), parse known fields.
    // Reject anything larger than our parse buffer rather than silently
    // truncating — a truncated body parses as partial garbage and leaves
    // unread bytes on the socket, desyncing a keep-alive connection.
    char buf[512];
    if ((size_t)req->content_len >= sizeof buf) {
        return send_text(req, "400 Bad Request", "text/plain", "bad config body");
    }
    int n = (int)req->content_len;
    int got = 0;
    while (got < n) {
        int r = httpd_req_recv(req, buf + got, n - got);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
            return ESP_FAIL;
        }
        got += r;
    }
    buf[got] = 0;
    // Cheap partial-JSON parse: callers may POST only the value changed.
    const struct {
        const char *key;
        int (*set)(uint32_t);
    } fields[] = {
        {"loop_interval_s", config_set_loop_interval_s},
        {"wifi_idle_sleep_s", config_set_wifi_idle_sleep_s},
    };
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        // Match the *quoted* key token and require a ':' right after, like the
        // string-field parser. A bare strstr would false-match the key as a
        // substring of a longer key or inside a string value.
        char needle[48];
        int nn = snprintf(needle, sizeof needle, "\"%s\"", fields[i].key);
        if (nn < 0 || nn >= (int)sizeof needle) continue;
        const char *p = strstr(buf, needle);
        if (!p) continue;
        p += nn;
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (*p != ':') continue;
        uint32_t v = (uint32_t)strtoul(p + 1, NULL, 10);
        fields[i].set(v);
    }
    bool battery_enabled = false;
    int bool_rc = json_get_bool_field(buf, "battery_icon_enabled", &battery_enabled);
    if (bool_rc < 0) {
        return send_text(req, "400 Bad Request", "text/plain", "bad battery flag");
    }
    if (bool_rc > 0 && config_set_battery_icon_enabled(battery_enabled) != 0) {
        return send_text(req, "500 Internal Server Error", "text/plain", "store failed");
    }

    char wifi_mode[16];
    int mode_rc = json_get_string_field(buf, "wifi_mode", wifi_mode, sizeof wifi_mode);
    if (mode_rc < 0) {
        return send_text(req, "400 Bad Request", "text/plain", "bad Wi-Fi mode");
    }
    if (mode_rc > 0 && config_set_wifi_mode_string(wifi_mode) != 0) {
        return send_text(req, "400 Bad Request", "text/plain", "Wi-Fi mode must be ap, sta, or sta_ap");
    }

    char sta_ssid[CONFIG_WIFI_SSID_MAX_LEN + 1];
    int ssid_rc = json_get_string_field(buf, "wifi_sta_ssid", sta_ssid, sizeof sta_ssid);
    if (ssid_rc < 0) {
        return send_text(req, "400 Bad Request", "text/plain", "bad STA SSID");
    }
    if (ssid_rc > 0 && config_set_wifi_sta_ssid(sta_ssid) != 0) {
        return send_text(req, "400 Bad Request", "text/plain", "SSID must be 0-32 printable ASCII characters");
    }

    char wifi_pass[CONFIG_WIFI_AP_PASSWORD_MAX_LEN + 1];
    int pass_rc = json_get_string_field(buf, "wifi_ap_password",
                                        wifi_pass, sizeof wifi_pass);
    if (pass_rc < 0) {
        return send_text(req, "400 Bad Request", "text/plain", "bad AP password");
    }
    if (pass_rc > 0 && config_set_wifi_ap_password(wifi_pass) != 0) {
        return send_text(req, "400 Bad Request", "text/plain",
                         "password must be empty or 8-63 ASCII characters");
    }

    char sta_pass[CONFIG_WIFI_STA_PASSWORD_MAX_LEN + 1];
    int sta_pass_rc = json_get_string_field(buf, "wifi_sta_password",
                                            sta_pass, sizeof sta_pass);
    if (sta_pass_rc < 0) {
        return send_text(req, "400 Bad Request", "text/plain", "bad STA password");
    }
    if (sta_pass_rc > 0 && config_set_wifi_sta_password(sta_pass) != 0) {
        return send_text(req, "400 Bad Request", "text/plain",
                         "password must be empty or 8-63 ASCII characters");
    }
    return send_config_json_with_version(req);
}

// Catch-all: for OS captive-portal probes (Apple /hotspot-detect.html, Android
// /generate_204, Microsoft /ncsi.txt, plus stray DNS-hijacked requests), serve
// a tiny stub that's just enough to make the OS realise this is a captive
// portal and pop its sign-in sheet, then redirect to "/".  We MUST NOT serve
// the full 6.8 KB UI here — Android storms /generate_204 ~100/s while the
// sheet is open, and 100 × 6.8 KB/s on a flooded socket pool can topple the
// httpd task.
static const char CAPTIVE_STUB[] =
    "<!doctype html><meta http-equiv=\"refresh\" content=\"0;url=/\">"
    "<title>PaperColor Frame</title>"
    "<a href=\"/\">PaperColor Frame</a>";

static esp_err_t h_catch_all(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        return httpd_resp_send(req, CAPTIVE_STUB, HTTPD_RESP_USE_STRLEN);
    }
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    return httpd_resp_send(req, NULL, 0);
}

// -------------------------------------------------------------------- start
int http_server_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn  = httpd_uri_match_wildcard;
    cfg.max_uri_handlers = 18;
    cfg.max_open_sockets = 7;
    // SD readdir + fatfs + sdspi chains burn a lot of stack; 8 KB overflows.
    cfg.stack_size       = 16384;
    cfg.recv_wait_timeout = 10;
    cfg.send_wait_timeout = 10;
    cfg.lru_purge_enable  = true;
    httpd_handle_t srv = NULL;
    if (httpd_start(&srv, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        return -1;
    }

#define REG(URI, M, H) do { \
    httpd_uri_t u = { .uri = URI, .method = M, .handler = H, .user_ctx = NULL }; \
    httpd_register_uri_handler(srv, &u); \
} while (0)

    REG("/",              HTTP_GET,    h_index);
    REG("/dither.wasm",   HTTP_GET,    h_wasm);
    REG("/i18n.js",       HTTP_GET,    h_i18n);
    REG("/api/list",      HTTP_GET,    h_list);
    REG("/api/photo/*",   HTTP_GET,    h_photo_get);
    REG("/api/photo/*",   HTTP_DELETE, h_photo_del);
    REG("/api/upload/*",  HTTP_POST,   h_upload);
    REG("/api/display/*", HTTP_POST,   h_display);
    REG("/api/next",      HTTP_POST,   h_next);
    REG("/api/config",    HTTP_GET,    h_config);
    REG("/api/config",    HTTP_POST,   h_config);
    REG("/api/calib",     HTTP_GET,    h_calib);
    REG("/api/calib",     HTTP_POST,   h_calib);
    REG("/api/calib-fill",HTTP_POST,   h_calib_fill);
    REG("/api/calib-mix", HTTP_POST,   h_calib_mix);
    // Catch-all for captive-portal probes ("/hotspot-detect.html",
    // "/generate_204", "/ncsi.txt", arbitrary URLs typed by users…).
    REG("/*",             HTTP_GET,    h_catch_all);
    REG("/*",             HTTP_POST,   h_catch_all);

#undef REG

    ESP_LOGI(TAG, "HTTP server up on :80");
    return 0;
}
