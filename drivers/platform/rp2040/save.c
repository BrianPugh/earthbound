/*
 * Generic RP2040 flash-backed save implementation.
 *
 * Save data lives in the last 2 sectors (8 KB) of external QSPI flash.
 * Reads go through XIP (memory-mapped flash). Writes disable interrupts
 * and call the low-level flash_range_erase/program directly.
 *
 * No board-specific dependencies — uses only Pico SDK flash APIs.
 * Requires PICO_FLASH_SIZE_BYTES to be defined (typically via CMake).
 */
#include "platform/platform.h"

#include <string.h>
#include "hardware/flash.h"
#include "hardware/sync.h"

#ifdef EB_ENABLE_DUAL_CORE_PPU
#include "pico/multicore.h"
#include "snes/ppu.h"
#endif

/* Save data occupies the last 2 flash sectors (8 KB).
 * PICO_FLASH_SIZE_BYTES is set to the full flash size via target_compile_definitions
 * in CMakeLists.txt (the linker region is 8 KB smaller to prevent overlap). */
#define SAVE_SECTOR_COUNT  2
#define SAVE_FLASH_OFFSET  (PICO_FLASH_SIZE_BYTES - SAVE_SECTOR_COUNT * FLASH_SECTOR_SIZE)

/* RAM buffer for sector read-modify-write (4 KB). Single-core builds put it in
 * the SCRATCH_X bank, which is otherwise idle there, so it stays out of main
 * SRAM's .data + .bss. Dual-core builds give that bank to core 1's stack and
 * line buffers and borrow the renderer's frame scratch instead
 * (sector_buffer()). */
#ifndef EB_ENABLE_DUAL_CORE_PPU
static uint8_t __scratch_x("save_sector_buf") sector_buf[FLASH_SECTOR_SIZE];
#endif

/* The read-modify-write buffer, or NULL if none is big enough. Saves run in
 * game logic on core 0, between frames, while neither core renders, and the
 * renderer rebuilds the borrowed buffers at the start of every frame. */
static uint8_t *sector_buffer(void) {
#ifdef EB_ENABLE_DUAL_CORE_PPU
    size_t bytes;
    uint8_t *buf = ppu_lend_frame_scratch(&bytes);
    return bytes >= FLASH_SECTOR_SIZE ? buf : NULL;
#else
    return sector_buf;
#endif
}

bool platform_save_init(void) {
    return true;
}

size_t platform_save_read(void *dst, size_t offset, size_t size) {
    if (offset + size > SAVE_SECTOR_COUNT * FLASH_SECTOR_SIZE)
        return 0;

    memcpy(dst, (const uint8_t *)(XIP_BASE + SAVE_FLASH_OFFSET + offset), size);
    return size;
}

/* Erase and reprogram one sector from buf.
 * Must be called with interrupts disabled (and core 1 paused if dual-core). */
static void __no_inline_not_in_flash_func(write_sector)(uint32_t sector_offset,
                                                        const uint8_t *buf) {
    flash_range_erase(sector_offset, FLASH_SECTOR_SIZE);
    flash_range_program(sector_offset, buf, FLASH_SECTOR_SIZE);
}

bool platform_save_write(const void *src, size_t offset, size_t size) {
    if (offset + size > SAVE_SECTOR_COUNT * FLASH_SECTOR_SIZE)
        return false;

    uint8_t *rmw = sector_buffer();
    if (!rmw)
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
        memcpy(rmw,
               (const uint8_t *)(XIP_BASE + sector_offset),
               FLASH_SECTOR_SIZE);
        memcpy(rmw + offset_in_sector, data, chunk);

        /* Erase + reprogram with IRQs disabled (and core 1 paused). */
#ifdef EB_ENABLE_DUAL_CORE_PPU
        multicore_lockout_start_blocking();
#endif
        uint32_t saved_irqs = save_and_disable_interrupts();
        write_sector(sector_offset, rmw);
        restore_interrupts(saved_irqs);
#ifdef EB_ENABLE_DUAL_CORE_PPU
        multicore_lockout_end_blocking();
#endif

        data   += chunk;
        offset += chunk;
        size   -= chunk;
    }

    return true;
}

/* Savestate slots: no flash backend yet (the 8 KB save region above only holds
 * battery SRAM). The port builds with EB_EMBEDDED, so the platform_savestate_*
 * hooks come from the shared fail-safe stub, src/platform/savestate_backend_stub.c. */
