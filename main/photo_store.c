// photo_store.c — flat photo directory under the active storage mount with safe-name
// resolution, atomic writes (via .tmp + rename), and streaming-uploads.

#include "photo_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>

#include "esp_log.h"
#include "esp_spiffs.h"

static const char *TAG = "photo_store";

// ---------------------------------------------------------------------- state
static char s_active[PHOTO_NAME_MAX] = {0};
static char s_photo_dir[PHOTO_DIR_MAX] = PHOTO_DIR;
static photo_store_error_t s_last_error = PHOTO_STORE_ERR_NONE;

static void set_error(photo_store_error_t err)
{
    s_last_error = err;
}

photo_store_error_t photo_store_last_error(void)
{
    return s_last_error;
}

const char *photo_store_dir(void)
{
    return s_photo_dir;
}

int photo_store_set_mount_point(const char *mount_point)
{
    if (!mount_point || mount_point[0] != '/') {
        set_error(PHOTO_STORE_ERR_BAD_NAME);
        return -1;
    }
    int n;
    if (strcmp(mount_point, "/spiffs") == 0) {
        n = snprintf(s_photo_dir, sizeof s_photo_dir, "%s", mount_point);
    } else {
        n = snprintf(s_photo_dir, sizeof s_photo_dir, "%s/photos", mount_point);
    }
    if (n < 0 || (size_t)n >= sizeof s_photo_dir) {
        set_error(PHOTO_STORE_ERR_BAD_NAME);
        return -1;
    }
    ESP_LOGI(TAG, "photo directory set to %s", s_photo_dir);
    set_error(PHOTO_STORE_ERR_NONE);
    return 0;
}

void photo_store_set_active(const char *name)
{
    if (!name) { s_active[0] = 0; return; }
    strncpy(s_active, name, sizeof s_active - 1);
    s_active[sizeof s_active - 1] = 0;
}

const char *photo_store_get_active(void)
{
    return s_active;
}

// ---------------------------------------------------------------------- init
int photo_store_init(void)
{
    struct stat st;
    if (stat(s_photo_dir, &st) != 0) {
        if (strcmp(s_photo_dir, "/spiffs") == 0) {
            set_error(PHOTO_STORE_ERR_NONE);
            return 0;
        }
        if (mkdir(s_photo_dir, 0775) != 0) {
            ESP_LOGE(TAG, "mkdir %s failed: %d", s_photo_dir, errno);
            set_error(errno == ENOSPC ? PHOTO_STORE_ERR_NO_SPACE : PHOTO_STORE_ERR_IO);
            return -1;
        }
        ESP_LOGI(TAG, "created %s", s_photo_dir);
    }
    set_error(PHOTO_STORE_ERR_NONE);
    return 0;
}

// ---------------------------------------------------------- name validation
static bool name_is_safe(const char *name)
{
    if (!name || !*name) return false;
    if (name[0] == '.') return false;     // no dotfiles / no '..'
    size_t n = strlen(name);
    if (n >= PHOTO_NAME_MAX) return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c == '/' || c == '\\') return false;
        if (c < 0x20 || c == 0x7f) return false;
    }
    return true;
}

int photo_store_path_for(const char *name, char *out, size_t out_sz)
{
    if (!name_is_safe(name)) {
        set_error(PHOTO_STORE_ERR_BAD_NAME);
        return -1;
    }
    int n = snprintf(out, out_sz, "%s/%s", s_photo_dir, name);
    if (n < 0 || (size_t)n >= out_sz) {
        set_error(PHOTO_STORE_ERR_BAD_NAME);
        return -1;
    }
    set_error(PHOTO_STORE_ERR_NONE);
    return 0;
}

// ---------------------------------------------------------------------- list
static int cmp_meta(const void *a, const void *b)
{
    return strcmp(((const photo_meta_t*)a)->name,
                  ((const photo_meta_t*)b)->name);
}

int photo_store_list(photo_meta_t *out_list, int max_n)
{
    DIR *d = opendir(s_photo_dir);
    if (!d) {
        ESP_LOGE(TAG, "opendir %s: %d", s_photo_dir, errno);
        set_error(PHOTO_STORE_ERR_IO);
        return -1;
    }
    int count = 0;
    char path[PHOTO_PATH_MAX];
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && count < max_n) {
        if (ent->d_name[0] == '.') continue;
        if (!name_is_safe(ent->d_name)) continue;       // bounds d_name < PHOTO_NAME_MAX
        // Quick extension filter — only .jpg / .jpeg (UI uploads JPG).
        const char *ext = strrchr(ent->d_name, '.');
        if (!ext) continue;
        if (strcasecmp(ext, ".jpg") != 0 && strcasecmp(ext, ".jpeg") != 0
            && strcasecmp(ext, ".bmp") != 0) continue;
        size_t nl = strlen(ent->d_name);
        if (nl == 0 || nl >= PHOTO_NAME_MAX) continue;
        int pn = snprintf(path, sizeof path, "%s/%s", s_photo_dir, ent->d_name);
        if (pn < 0 || pn >= (int)sizeof path) continue;
        struct stat st;
        if (stat(path, &st) != 0) continue;
        memcpy(out_list[count].name, ent->d_name, nl);
        out_list[count].name[nl] = 0;
        out_list[count].size  = (uint32_t)st.st_size;
        out_list[count].mtime = (uint32_t)st.st_mtime;
        count++;
    }
    closedir(d);
    qsort(out_list, count, sizeof(photo_meta_t), cmp_meta);
    set_error(PHOTO_STORE_ERR_NONE);
    return count;
}

bool photo_store_free_bytes(uint64_t *free_bytes)
{
    if (strcmp(s_photo_dir, "/spiffs") != 0) return false;
    size_t total = 0, used = 0;
    if (esp_spiffs_info("storage", &total, &used) != ESP_OK || total < used) {
        return false;
    }
    *free_bytes = (uint64_t)(total - used);
    return true;
}

// ---------------------------------------------------------------------- save
int photo_store_save(const char *name, const uint8_t *data, size_t len)
{
    photo_writer_t *w = photo_writer_open(name);
    if (!w) return -1;
    if (photo_writer_feed(w, data, len) != 0) {
        photo_writer_close(w, false);
        return -1;
    }
    return photo_writer_close(w, true);
}

struct photo_writer_s {
    char  final_path[PHOTO_PATH_MAX];
    char  tmp_path  [PHOTO_PATH_MAX + 8];   // room for ".tmp"
    FILE *fp;
};

photo_writer_t *photo_writer_open(const char *name)
{
    photo_writer_t *w = calloc(1, sizeof *w);
    if (!w) {
        set_error(PHOTO_STORE_ERR_IO);
        return NULL;
    }
    if (photo_store_path_for(name, w->final_path, sizeof w->final_path) != 0) {
        free(w);
        return NULL;
    }
    int n = snprintf(w->tmp_path, sizeof w->tmp_path, "%s.tmp", w->final_path);
    if (n < 0 || n >= (int)sizeof w->tmp_path) {
        set_error(PHOTO_STORE_ERR_BAD_NAME);
        free(w);
        return NULL;
    }
    w->fp = fopen(w->tmp_path, "wb");
    if (!w->fp) {
        ESP_LOGE(TAG, "open %s for write: %d", w->tmp_path, errno);
        set_error(errno == ENOSPC ? PHOTO_STORE_ERR_NO_SPACE : PHOTO_STORE_ERR_IO);
        free(w);
        return NULL;
    }
    set_error(PHOTO_STORE_ERR_NONE);
    return w;
}

int photo_writer_feed(photo_writer_t *w, const uint8_t *buf, size_t n)
{
    if (!w || !w->fp) return -1;
    if (n == 0) return 0;
    size_t off = 0;
    while (off < n) {
        size_t wr = fwrite(buf + off, 1, n - off, w->fp);
        if (wr == 0) {
            ESP_LOGE(TAG, "short write to %s (errno=%d)", w->tmp_path, errno);
            set_error(errno == ENOSPC ? PHOTO_STORE_ERR_NO_SPACE : PHOTO_STORE_ERR_IO);
            return -1;
        }
        off += wr;
    }
    set_error(PHOTO_STORE_ERR_NONE);
    return 0;
}

int photo_writer_close(photo_writer_t *w, bool keep)
{
    if (!w) return -1;
    photo_store_error_t prior_error = s_last_error;
    int rc = 0;
    if (w->fp) {
        if (fclose(w->fp) != 0) {
            set_error(errno == ENOSPC ? PHOTO_STORE_ERR_NO_SPACE : PHOTO_STORE_ERR_IO);
            rc = -1;
        }
        w->fp = NULL;
    }
    if (keep && rc == 0) {
        // Best-effort: remove pre-existing target then rename.  FATFS
        // rename refuses to overwrite, so unlink first.
        unlink(w->final_path);
        if (rename(w->tmp_path, w->final_path) != 0) {
            ESP_LOGE(TAG, "rename %s -> %s failed: %d",
                     w->tmp_path, w->final_path, errno);
            set_error(errno == ENOSPC ? PHOTO_STORE_ERR_NO_SPACE : PHOTO_STORE_ERR_IO);
            rc = -1;
        }
    } else {
        unlink(w->tmp_path);
    }
    if (rc == 0 && (keep || prior_error == PHOTO_STORE_ERR_NONE)) {
        set_error(PHOTO_STORE_ERR_NONE);
    } else if (!keep && prior_error != PHOTO_STORE_ERR_NONE) {
        set_error(prior_error);
    }
    free(w);
    return rc;
}

// -------------------------------------------------------------------- delete
int photo_store_delete(const char *name)
{
    char path[PHOTO_PATH_MAX + 32];
    if (photo_store_path_for(name, path, sizeof path) != 0) return -1;
    if (unlink(path) != 0 && errno != ENOENT) {
        ESP_LOGE(TAG, "unlink %s: %d", path, errno);
        return -1;
    }
    // Also remove the .bin and .json companions if present.  Ignore the
    // result — legacy photos predating those sibling files are a normal case.
    char bin_path[PHOTO_PATH_MAX + 32];
    if (photo_store_bin_path_for(name, bin_path, sizeof bin_path) == 0) {
        unlink(bin_path);
    }
    char json_path[PHOTO_PATH_MAX + 32];
    if (photo_store_sidecar_path_for(name, ".json", json_path, sizeof json_path) == 0) {
        unlink(json_path);
    }
    if (strcmp(s_active, name) == 0) s_active[0] = 0;
    return 0;
}

// --------------------------------------------------- generic sidecar resolver
int photo_store_sidecar_path_for(const char *name, const char *ext,
                                 char *out, size_t out_sz)
{
    if (!name_is_safe(name) || !ext || ext[0] != '.') return -1;
    size_t nl = strlen(name);
    size_t base_len = nl;
    const char *cur = strrchr(name, '.');
    if (cur) {
        if (strcasecmp(cur, ".jpg")  == 0 ||
            strcasecmp(cur, ".jpeg") == 0 ||
            strcasecmp(cur, ".bmp")  == 0 ||
            strcasecmp(cur, ".bin")  == 0 ||
            strcasecmp(cur, ".json") == 0) {
            base_len = (size_t)(cur - name);
        }
    }
    int n = snprintf(out, out_sz, "%s/%.*s%s",
                     s_photo_dir, (int)base_len, name, ext);
    if (n < 0 || (size_t)n >= out_sz) return -1;
    return 0;
}

// ---------------------------------------------------------------- bin sibling
int photo_store_bin_path_for(const char *name, char *out, size_t out_sz)
{
    if (!name_is_safe(name)) return -1;
    // Strip a trailing .jpg / .jpeg / .bmp extension and append .bin.
    size_t nl = strlen(name);
    size_t base_len = nl;
    const char *ext = strrchr(name, '.');
    if (ext) {
        if (strcasecmp(ext, ".jpg")  == 0 ||
            strcasecmp(ext, ".jpeg") == 0 ||
            strcasecmp(ext, ".bmp")  == 0 ||
            strcasecmp(ext, ".bin")  == 0) {
            base_len = (size_t)(ext - name);
        }
    }
    // Compose: "<photo-dir>/" + base + ".bin"
    int n = snprintf(out, out_sz, "%s/%.*s.bin",
                     s_photo_dir, (int)base_len, name);
    if (n < 0 || (size_t)n >= out_sz) return -1;
    return 0;
}

bool photo_store_has_bin(const char *name)
{
    char path[PHOTO_PATH_MAX + 32];
    if (photo_store_bin_path_for(name, path, sizeof path) != 0) return false;
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}
