/*
 * Default no-op savestate storage backend.
 *
 * The savestate serialization CORE (src/core/state_dump.c) is compiled for every
 * target, but the actual storage hooks (platform_savestate_*, platform.h) are
 * target-specific: desktop writes two files (port/unix/platform/sdl2_savestate.c);
 * an embedded target writes two flash regions wired to its firmware power-off
 * handler. That firmware backend is target-specific code handled in a later session.
 *
 * Until an embedded target provides a real backend, these stubs satisfy the link and
 * make a savestate capture/restore fail SAFE — _begin/_write/_commit return false and
 * _read returns 0, so state_dump_save_slots()/_load_slots() report failure (the
 * firmware's rail-hold handshake sees HOST_CAPTURE_FAILED) instead of tearing a write.
 *
 * A target that supplies a real backend defines EB_HAVE_SAVESTATE_BACKEND (build flag)
 * to drop these stubs. Desktop is excluded by the EB_EMBEDDED guard (it links the real
 * SDL backend), so this TU is empty on desktop and never collides with it.
 */
#include "platform/platform.h"

#if defined(EB_EMBEDDED) && !defined(EB_HAVE_SAVESTATE_BACKEND)

bool platform_savestate_begin(int slot) {
    (void)slot;
    return false;
}

bool platform_savestate_write(int slot, size_t offset, const void *src, size_t size) {
    (void)slot;
    (void)offset;
    (void)src;
    (void)size;
    return false;
}

bool platform_savestate_commit(int slot) {
    (void)slot;
    return false;
}

size_t platform_savestate_read(int slot, size_t offset, void *dst, size_t size) {
    (void)slot;
    (void)offset;
    (void)dst;
    (void)size;
    return 0;
}

#endif /* EB_EMBEDDED && !EB_HAVE_SAVESTATE_BACKEND */
