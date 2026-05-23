#ifndef PAPER_E6_SD_STORAGE_H
#define PAPER_E6_SD_STORAGE_H

#include <stdbool.h>

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

// Find the first supported image file (case-insensitive .jpg/.jpeg/.bmp) in
// the SD root directory.  Writes the full path into `out_path` (e.g.,
// "/sdcard/photo.jpg") and returns true.  Returns false when none found.
bool sd_storage_find_first_image(char *out_path, int out_path_len);

#ifdef __cplusplus
}
#endif

#endif
