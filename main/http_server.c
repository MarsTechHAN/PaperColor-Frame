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
#include <sys/stat.h>

#include "esp_http_server.h"
#include "esp_log.h"

#include "photo_store.h"
#include "config_store.h"
#include "loop_display.h"

static const char *TAG = "http";

// Embedded gzipped web UI (see main/CMakeLists.txt EMBED_FILES).
extern const uint8_t INDEX_GZ_START[] asm("_binary_index_html_gz_start");
extern const uint8_t INDEX_GZ_END[]   asm("_binary_index_html_gz_end");
extern const uint8_t WASM_GZ_START[]  asm("_binary_dither_wasm_gz_start");
extern const uint8_t WASM_GZ_END[]    asm("_binary_dither_wasm_gz_end");

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

// Pull the last path segment as a NUL-terminated name (URL-decoded enough
// for our 'p_<digits>.jpg' filenames).  No percent-decoding here — the UI
// only generates ASCII names.
static int url_tail_name(const char *uri, const char *prefix,
                         char *out, size_t out_sz)
{
    size_t pl = strlen(prefix);
    if (strncmp(uri, prefix, pl) != 0) return -1;
    const char *p = uri + pl;
    // strip any query string
    const char *q = strchr(p, '?');
    size_t n = q ? (size_t)(q - p) : strlen(p);
    if (n == 0 || n >= out_sz) return -1;
    memcpy(out, p, n);
    out[n] = 0;
    return 0;
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

// GET /api/list  → {"active":"...", "photos":[{name,size,mtime},...]}
static esp_err_t h_list(httpd_req_t *req)
{
    photo_meta_t list[64];
    int n = photo_store_list(list, 64);
    if (n < 0) return send_json(req, "{\"photos\":[],\"active\":null}");

    // Build JSON manually — keeps deps lean.
    size_t cap = 256 + n * 96;
    char *buf = malloc(cap);
    if (!buf) return ESP_FAIL;
    size_t off = 0;
    const char *act = photo_store_get_active();
    off += snprintf(buf + off, cap - off,
                    "{\"active\":%s%s%s,\"photos\":[",
                    (act && *act) ? "\"" : "",
                    (act && *act) ? act  : "null",
                    (act && *act) ? "\"" : "");
    for (int i = 0; i < n; i++) {
        off += snprintf(buf + off, cap - off,
                        "%s{\"name\":\"%s\",\"size\":%u,\"mtime\":%u}",
                        i ? "," : "",
                        list[i].name,
                        (unsigned)list[i].size,
                        (unsigned)list[i].mtime);
    }
    off += snprintf(buf + off, cap - off, "]}");
    esp_err_t rc = send_json(req, buf);
    free(buf);
    return rc;
}

// GET /api/photo/<name>  → stream the raw JPG.
static esp_err_t h_photo_get(httpd_req_t *req)
{
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
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        httpd_resp_set_status(req, "404 Not Found");
        return httpd_resp_send(req, "no", HTTPD_RESP_USE_STRLEN);
    }
    const char *mime = "image/jpeg";
    const char *ext = strrchr(name, '.');
    if (ext && strcasecmp(ext, ".bmp") == 0) mime = "image/bmp";
    httpd_resp_set_type(req, mime);
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=86400");

    char chunk[1024];
    size_t r;
    while ((r = fread(chunk, 1, sizeof chunk, fp)) > 0) {
        if (httpd_resp_send_chunk(req, chunk, r) != ESP_OK) {
            fclose(fp);
            return ESP_FAIL;
        }
    }
    fclose(fp);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

// POST /api/upload/<name>  → save to <active-store>/photos/<name>.tmp, then rename.
static esp_err_t h_upload(httpd_req_t *req)
{
    char name[PHOTO_NAME_MAX];
    if (url_tail_name(req->uri, "/api/upload/", name, sizeof name) != 0) {
        return send_text(req, "400 Bad Request", "text/plain", "bad name");
    }
    uint64_t free_bytes = 0;
    if (photo_store_free_bytes(&free_bytes) &&
        req->content_len > 0 &&
        (uint64_t)req->content_len > free_bytes) {
        ESP_LOGW(TAG, "upload rejected: %s needs %d bytes, only %llu free in %s",
                 name, req->content_len, (unsigned long long)free_bytes, photo_store_dir());
        return send_text(req, "507 Insufficient Storage", "text/plain", "not enough storage");
    }
    photo_writer_t *w = photo_writer_open(name);
    if (!w) return send_photo_store_error(req, "open failed");

    char buf[2048];
    int remaining = req->content_len;
    while (remaining > 0) {
        int got = httpd_req_recv(req, buf,
                                 remaining > (int)sizeof buf ? (int)sizeof buf : remaining);
        if (got <= 0) {
            if (got == HTTPD_SOCK_ERR_TIMEOUT) continue;
            photo_writer_close(w, false);
            return ESP_FAIL;
        }
        if (photo_writer_feed(w, (uint8_t *)buf, got) != 0) {
            photo_writer_close(w, false);
            return send_photo_store_error(req, "write failed");
        }
        remaining -= got;
    }
    if (photo_writer_close(w, true) != 0) {
        return send_photo_store_error(req, "rename failed");
    }
    ESP_LOGI(TAG, "upload ok: %s (%d bytes)", name, req->content_len);
    return send_json(req, "{\"ok\":true}");
}

// DELETE /api/photo/<name>
static esp_err_t h_photo_del(httpd_req_t *req)
{
    char name[PHOTO_NAME_MAX];
    if (url_tail_name(req->uri, "/api/photo/", name, sizeof name) != 0) {
        return send_text(req, "400 Bad Request", "text/plain", "bad name");
    }
    if (photo_store_delete(name) != 0) {
        return send_text(req, "500 Internal Server Error", "text/plain", "delete failed");
    }
    return send_json(req, "{\"ok\":true}");
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
    struct stat st;
    if (stat(path, &st) != 0) {
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

// GET /api/config  → {loop_interval_s}
// POST /api/config { "loop_interval_s": N }
static esp_err_t h_config(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        char body[64];
        snprintf(body, sizeof body, "{\"loop_interval_s\":%u}",
                 (unsigned)config_get_loop_interval_s());
        return send_json(req, body);
    }
    // POST: read full body (<= 64 bytes), parse a single integer field.
    char buf[128];
    int n = (req->content_len < (int)sizeof buf - 1)
              ? req->content_len : (int)sizeof buf - 1;
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
    // Cheap parse: find "loop_interval_s" : <num>
    const char *p = strstr(buf, "loop_interval_s");
    if (p) {
        p = strchr(p, ':');
        if (p) {
            uint32_t v = (uint32_t)strtoul(p + 1, NULL, 10);
            config_set_loop_interval_s(v);
        }
    }
    char body[64];
    snprintf(body, sizeof body, "{\"loop_interval_s\":%u}",
             (unsigned)config_get_loop_interval_s());
    return send_json(req, body);
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
    "<title>PaperE6</title>"
    "<a href=\"/\">PaperE6</a>";

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
    cfg.max_uri_handlers = 16;
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
    // Catch-all for captive-portal probes ("/hotspot-detect.html",
    // "/generate_204", "/ncsi.txt", arbitrary URLs typed by users…).
    REG("/*",             HTTP_GET,    h_catch_all);
    REG("/*",             HTTP_POST,   h_catch_all);

#undef REG

    ESP_LOGI(TAG, "HTTP server up on :80");
    return 0;
}
