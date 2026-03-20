/*
 * Pico save data implementation — flash-backed persistent storage.
 *
 * Save data lives in the last 2 sectors (8 KB) of the 16 MB external QSPI
 * flash. Reads go through XIP (memory-mapped flash). Writes disable
 * interrupts and call the low-level flash_range_erase/program directly.
 */
#include "platform/platform.h"

#include <string.h>
#include "hardware/flash.h"
#include "hardware/sync.h"

#ifdef ENABLE_DUAL_CORE_PPU
#include "pico/multicore.h"
#endif

/* Save data occupies the last 2 flash sectors (8 KB).
 * PICO_FLASH_SIZE_BYTES is set to the full 16 MB via target_compile_definitions
 * in CMakeLists.txt (the linker region is 8 KB smaller to prevent overlap). */
#define SAVE_SECTOR_COUNT  2
#define SAVE_FLASH_OFFSET  (PICO_FLASH_SIZE_BYTES - SAVE_SECTOR_COUNT * FLASH_SECTOR_SIZE)

/* RAM buffer for sector read-modify-write (4 KB). */
static uint8_t sector_buf[FLASH_SECTOR_SIZE];

bool platform_save_init(void) {
    return true;
}

size_t platform_save_read(void *dst, size_t offset, size_t size) {
    if (offset + size > SAVE_SECTOR_COUNT * FLASH_SECTOR_SIZE)
        return 0;

    memcpy(dst, (const uint8_t *)(XIP_BASE + SAVE_FLASH_OFFSET + offset), size);
    return size;
}

/* Erase and reprogram one sector from sector_buf.
 * Must be called with interrupts disabled (and core 1 paused if dual-core). */
static void __no_inline_not_in_flash_func(write_sector)(uint32_t sector_offset) {
    flash_range_erase(sector_offset, FLASH_SECTOR_SIZE);
    flash_range_program(sector_offset, sector_buf, FLASH_SECTOR_SIZE);
}

bool platform_save_write(const void *src, size_t offset, size_t size) {
    if (offset + size > SAVE_SECTOR_COUNT * FLASH_SECTOR_SIZE)
        return false;

    const uint8_t *data = (const uint8_t *)src;

    while (size > 0) {
        /* Which sector does this byte fall in? */
        uint32_t sector_offset = SAVE_FLASH_OFFSET +
            (offset / FLASH_SECTOR_SIZE) * FLASH_SECTOR_SIZE;
        size_t offset_in_sector = offset % FLASH_SECTOR_SIZE;
        size_t chunk = FLASH_SECTOR_SIZE - offset_in_sector;
        if (chunk > size)
            chunk = size;

        /* Read-modify-write: copy whole sector, patch our bytes. */
        memcpy(sector_buf,
               (const uint8_t *)(XIP_BASE + sector_offset),
               FLASH_SECTOR_SIZE);
        memcpy(sector_buf + offset_in_sector, data, chunk);

        /* Erase + reprogram with IRQs disabled (and core 1 paused). */
#ifdef ENABLE_DUAL_CORE_PPU
        multicore_lockout_start_blocking();
#endif
        uint32_t saved_irqs = save_and_disable_interrupts();
        write_sector(sector_offset);
        restore_interrupts(saved_irqs);
#ifdef ENABLE_DUAL_CORE_PPU
        multicore_lockout_end_blocking();
#endif

        data   += chunk;
        offset += chunk;
        size   -= chunk;
    }

    return true;
}
