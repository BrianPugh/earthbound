/*
 * Unix/SDL2 savestate storage — file-backed ping-pong slots.
 *
 * Two slots back the crash-safe savestate (build-order item #5): each slot is a
 * file "savestate.bin.<slot>". A write is bracketed by _begin (truncate) and
 * _commit (fflush + fsync + close) so a power loss between them leaves the prior
 * slot's file untouched. See state_dump.c for the format/ping-pong logic and
 * src/platform/platform.h for the interface contract.
 */
#include "platform/platform.h"
#include <stdio.h>
#include <unistd.h>

static const char *slot_path(int slot) {
    return slot == 0 ? "savestate.bin.0" : "savestate.bin.1";
}

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
    if (ok && fsync(fileno(f)) != 0) /* durability: the new slot must hit stable storage */
        ok = false;
    if (fclose(f) != 0)
        ok = false;
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
