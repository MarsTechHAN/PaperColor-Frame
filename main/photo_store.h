#ifndef PAPER_E6_PHOTO_STORE_H
#define PAPER_E6_PHOTO_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Default location when SD is available. If SD mount fails, main switches this
// module to the internal SPIFFS mount and photos live directly in /spiffs
// because SPIFFS has a flat namespace.
#define PHOTO_DIR        "/sdcard/photos"
#define PHOTO_DIR_MAX    48
#define PHOTO_NAME_MAX   96
#define PHOTO_PATH_MAX   384

typedef enum {
    PHOTO_STORE_ERR_NONE = 0,
    PHOTO_STORE_ERR_BAD_NAME,
    PHOTO_STORE_ERR_NO_SPACE,
    PHOTO_STORE_ERR_IO,
} photo_store_error_t;

typedef struct {
    char     name[PHOTO_NAME_MAX];
    uint32_t size;
    uint32_t mtime;
} photo_meta_t;

// Create PHOTO_DIR if it does not exist.  Safe to call once SD is mounted.
int  photo_store_init(void);

// Select the storage mount point before photo_store_init(), e.g. "/sdcard" or
// "/spiffs". SD uses "<mount>/photos"; SPIFFS uses the mount root directly.
int  photo_store_set_mount_point(const char *mount_point);
const char *photo_store_dir(void);

// Last error from open/feed/close/save helpers, for HTTP status mapping.
photo_store_error_t photo_store_last_error(void);

// Best-effort free-space query for the active store. Returns false when the
// filesystem backend cannot report capacity cheaply.
bool photo_store_free_bytes(uint64_t *free_bytes);

// Fill *out_list with up to max_n entries from the active photo directory
// (sorted by name).
// Returns count written.  Negative on filesystem error.
int  photo_store_list(photo_meta_t *out_list, int max_n);

// Resolve <name> to /sdcard/photos/<name>.  Rejects names containing '/' or
// starting with '.' to prevent path traversal.  Returns 0 on success.
int  photo_store_path_for(const char *name, char *out, size_t out_sz);

// Atomic save: write to /sdcard/photos/<name>.tmp then rename.  Caller owns
// `data`.  Overwrites an existing file.
int  photo_store_save(const char *name, const uint8_t *data, size_t len);

// Streamed save — for HTTP uploads larger than a chunk.  Begin → feed → end.
typedef struct photo_writer_s photo_writer_t;
photo_writer_t *photo_writer_open(const char *name);
int             photo_writer_feed(photo_writer_t *w, const uint8_t *buf, size_t n);
int             photo_writer_close(photo_writer_t *w, bool keep);

// Delete /sdcard/photos/<name>.  Returns 0 even when the file did not exist.
// Also unlinks the .bin sibling (e.g. p_123.jpg → also removes p_123.bin).
int  photo_store_delete(const char *name);

// Resolve <name>'s pre-dithered companion file path.  Given "p_123.jpg" or
// "p_123.jpeg" this writes "/sdcard/photos/p_123.bin".  For any other name
// the result is "/sdcard/photos/<name>.bin".  Returns 0 on success.
int  photo_store_bin_path_for(const char *name, char *out, size_t out_sz);

// Quick existence check for the .bin sibling.
bool photo_store_has_bin(const char *name);

// Resolve a sibling-file path with the given dotted extension (".json",
// ".bin", etc.), reusing the base name of <name>.  Useful for per-photo
// metadata sidecars.  Returns 0 on success.
int  photo_store_sidecar_path_for(const char *name, const char *ext,
                                  char *out, size_t out_sz);

// Active-photo tracker — what is currently on the panel.  Used by the UI
// "is_active" highlight.  Free-form short string, not persisted.
void photo_store_set_active(const char *name);
const char *photo_store_get_active(void);

#ifdef __cplusplus
}
#endif
#endif
