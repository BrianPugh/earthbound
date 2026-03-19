/*
 * Pico save data implementation — stub (no persistent storage yet).
 */
#include "platform/platform.h"
#include <string.h>

bool platform_save_init(void) {
    return true;
}

size_t platform_save_read(void *dst, size_t offset, size_t size) {
    (void)dst;
    (void)offset;
    (void)size;
    return 0;
}

bool platform_save_write(const void *src, size_t offset, size_t size) {
    (void)src;
    (void)offset;
    (void)size;
    return false;
}
