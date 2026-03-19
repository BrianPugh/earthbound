/*
 * Unix/SDL2 save data implementation — file-backed persistent storage.
 *
 * Save data is stored in a single file (default "earthbound.sav").
 * The file path can be overridden via --save on the command line.
 */
#include "platform/platform.h"
#include <stdio.h>
#include <string.h>

static const char *save_file_path = "earthbound.sav";

void platform_save_set_path(const char *path) {
    save_file_path = path;
}

bool platform_save_init(void) {
    /* Nothing to initialize for file-backed saves */
    return true;
}

size_t platform_save_read(void *dst, size_t offset, size_t size) {
    FILE *f = fopen(save_file_path, "rb");
    if (!f) return 0;

    if (fseek(f, (long)offset, SEEK_SET) != 0) {
        fclose(f);
        return 0;
    }

    size_t read = fread(dst, 1, size, f);
    fclose(f);
    return read;
}

bool platform_save_write(const void *src, size_t offset, size_t size) {
    /* Open existing or create new */
    FILE *f = fopen(save_file_path, "r+b");
    if (!f) {
        f = fopen(save_file_path, "w+b");
        if (!f) return false;
    }

    if (fseek(f, (long)offset, SEEK_SET) != 0) {
        fclose(f);
        return false;
    }

    size_t written = fwrite(src, 1, size, f);
    fclose(f);
    return written == size;
}
