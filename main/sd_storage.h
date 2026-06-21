#ifndef PAPER_E6_SD_STORAGE_H
#define PAPER_E6_SD_STORAGE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Mount the SD card at /sdcard using SDSPI on the shared SPI bus.
// Set `init_bus` true if no other driver has called spi_bus_initialize() yet.
// Returns 0 on success, negative on failure.
int sd_storage_mount(bool init_bus);

// Mount the internal LittleFS storage at /littlefs.  Used when SD init/mount
// fails, and also as the primary store on SD-less boards.  Returns 0 on
// success, negative on failure.
int sd_storage_mount_internal(void);

// Unmount and free the card.
void sd_storage_unmount(void);

// ---- Filesystem access guard -------------------------------------------------
// The display worker can hard-power-cycle the shared EPD+SD rail and
// unmount/remount the filesystem during panel recovery. HTTP handlers run on a
// different task and may be mid-fopen/fread/fwrite on the same mount. These
// guard the two against each other:
//
//   * HTTP file handlers call sd_storage_fs_acquire() before touching the FS
//     and sd_storage_fs_release() after. acquire() returns false while a
//     recovery is pending (the handler should answer 503) so no new I/O starts.
//   * The worker calls sd_storage_recover_begin() before unmounting; it blocks
//     new acquisitions and waits up to drain_timeout_ms for in-flight handlers
//     to finish. It returns true once the FS is quiescent and safe to tear
//     down, false on timeout (caller must NOT unmount — skip recovery this
//     round). sd_storage_recover_end() re-opens the gate afterwards.
bool sd_storage_fs_acquire(void);
void sd_storage_fs_release(void);
bool sd_storage_recover_begin(uint32_t drain_timeout_ms);
void sd_storage_recover_end(void);

// Find the first supported image file (case-insensitive .jpg/.jpeg/.bmp) in
// the SD root directory.  Writes the full path into `out_path` (e.g.,
// "/sdcard/photo.jpg") and returns true.  Returns false when none found.
bool sd_storage_find_first_image(char *out_path, int out_path_len);

#ifdef __cplusplus
}
#endif

#endif
