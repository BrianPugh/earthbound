/*
 * Unix/SDL2 savestate storage — file-backed ping-pong slots.
 *
 * Two slots back the crash-safe savestate (build-order item #5): each slot is a
 * file "savestate.bin.<slot>". A write is bracketed by _begin (truncate) and
 * _commit (fflush + fsync + close, then an fsync of the containing directory so a
 * newly created slot file's directory entry is durable too) so a power loss between
 * them leaves the prior slot's file untouched. See state_dump.c for the format/ping-pong logic and
 * src/platform/platform.h for the interface contract.
 */
#include "platform/platform.h"
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <io.h>        /* _commit, _fileno */
#else
#include <unistd.h>    /* fsync, fileno, close */
#include <fcntl.h>     /* open, O_RDONLY */
#endif

/* Slot files are "<prefix>.0" / "<prefix>.1". The playtest harness points the
 * prefix at a named snapshot (snaps/NAME) around a snap/restore. */
static char slot_prefix[200] = "savestate.bin";

void platform_savestate_set_path(const char *prefix) {
    snprintf(slot_prefix, sizeof(slot_prefix), "%s", prefix ? prefix : "savestate.bin");
}

static const char *slot_path(int slot) {
    static char path[sizeof(slot_prefix) + 4];
    snprintf(path, sizeof(path), "%s.%d", slot_prefix, slot);
    return path;
}

#ifndef _WIN32
/* Best-effort fsync of the directory holding `path` (as sdl2_save.c does for the
 * SRAM file): the first write of a slot creates its file, and the new directory
 * entry needs its own flush to survive a power loss. */
static void fsync_containing_dir(const char *path) {
    char dir[sizeof(slot_prefix) + 4];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    const char *dir_path = ".";
    if (slash) {
        *slash = '\0';
        dir_path = dir[0] != '\0' ? dir : "/";
    }
    int fd = open(dir_path, O_RDONLY);
    if (fd >= 0) {
        fsync(fd);
        close(fd);
    }
}
#endif

/* In-progress writers, one per slot (NULL when no write is open). */
static FILE *slot_writer[SAVESTATE_SLOTS];

bool platform_savestate_begin(int slot) {
    if (slot < 0 || slot >= SAVESTATE_SLOTS)
        return false;
    if (slot_writer[slot]) {
        fclose(slot_writer[slot]);
        slot_writer[slot] = NULL;
    }
    FILE *f = fopen(slot_path(slot), "wb"); /* truncate */
    if (!f)
        return false;
    slot_writer[slot] = f;
    return true;
}

bool platform_savestate_write(int slot, size_t offset, const void *src, size_t size) {
    if (slot < 0 || slot >= SAVESTATE_SLOTS || !slot_writer[slot])
        return false;
    if (fseek(slot_writer[slot], (long)offset, SEEK_SET) != 0)
        return false;
    return fwrite(src, 1, size, slot_writer[slot]) == size;
}

bool platform_savestate_commit(int slot) {
    if (slot < 0 || slot >= SAVESTATE_SLOTS || !slot_writer[slot])
        return false;
    FILE *f = slot_writer[slot];
    slot_writer[slot] = NULL;
    bool ok = (fflush(f) == 0);
#ifdef _WIN32
    if (ok && _commit(_fileno(f)) != 0) /* durability: the new slot must hit stable storage */
        ok = false;
#else
    if (ok && fsync(fileno(f)) != 0) /* durability: the new slot must hit stable storage */
        ok = false;
#endif
    if (fclose(f) != 0)
        ok = false;
#ifndef _WIN32
    if (ok)
        fsync_containing_dir(slot_path(slot));
#endif
    return ok;
}

size_t platform_savestate_read(int slot, size_t offset, void *dst, size_t size) {
    if (slot < 0 || slot >= SAVESTATE_SLOTS)
        return 0;
    FILE *f = fopen(slot_path(slot), "rb");
    if (!f)
        return 0;
    size_t r = 0;
    if (fseek(f, (long)offset, SEEK_SET) == 0)
        r = fread(dst, 1, size, f);
    fclose(f);
    return r;
}

/* Savestate (de)compressor scratch — the tamp LZ window + working struct + I/O
 * staging (see state_dump.c). The embedded ports lend an idle framebuffer; on
 * desktop a small static buffer is fine. 16 KiB gives generous staging without
 * mattering on a PC. */
void *platform_savestate_scratch(size_t *out_bytes) {
    static unsigned char scratch[16 * 1024];
    if (out_bytes)
        *out_bytes = sizeof(scratch);
    return scratch;
}
