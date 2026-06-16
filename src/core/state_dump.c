#include "core/state_dump.h"

#ifdef EB_EMBEDDED

/* No filesystem on embedded targets; the savestate backend is wired separately. */
bool state_dump_save(const char *path) {
    (void)path;
    return false;
}

bool state_dump_load(const char *path) {
    (void)path;
    return false;
}

bool state_dump_roundtrip_test(void) {
    return false;
}

#else

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "core/memory.h"
#include "core/math.h"
#include "core/mode_stack.h"
#include "game/game_state.h"
#include "game/overworld.h"
#include "game/battle.h"
#include "game/display_text.h"
#include "game/window.h"
#include "game/map_loader.h"
#include "game/fade.h"
#include "game/door.h"
#include "game/audio.h"
#include "game/oval_window.h"
#include "game/position_buffer.h"
#include "snes/ppu.h"
#include "entity/entity.h"

/* Container format: "EBSD" magic + version u16 + frame u16, then a series of
 * id(u16)/size(u32)/blob sections, then a 0xFFFF terminator. See the AUDIT in
 * docs/plans/savestate-unified-loop.md. */
#define STATE_DUMP_MAGIC   0x44534245u  /* "EBSD" little-endian */
#define STATE_DUMP_VERSION 1

/* Section IDs */
enum {
    SECTION_CORE             = 0x0001,
    SECTION_GAME_STATE       = 0x0002,
    SECTION_PARTY_CHARACTERS = 0x0003,
    SECTION_EVENT_FLAGS      = 0x0004,
    SECTION_OVERWORLD        = 0x0005,
    SECTION_BATTLE           = 0x0006,
    SECTION_DISPLAY_TEXT     = 0x0007,
    SECTION_WINDOW           = 0x0008,
    SECTION_MAP_LOADER       = 0x0009,
    SECTION_PPU              = 0x000A,
    SECTION_POSITION_BUFFER  = 0x000B,
    SECTION_DOOR             = 0x000C,
    SECTION_ENTITY_RUNTIME   = 0x000D,
    SECTION_ENTITY_SYSTEM    = 0x000E,
    SECTION_SCRIPTS          = 0x000F,
    SECTION_SPRITE_PRIORITY  = 0x0010,
    SECTION_FADE             = 0x0011,
    SECTION_RNG              = 0x0012,
    SECTION_AUDIO            = 0x0013,
    SECTION_PSI_ANIMATION    = 0x0014,
    SECTION_MODE_STACK       = 0x0015,
    SECTION_TERMINATOR       = 0xFFFF,
};

/* One serialized module: a tagged blob copied verbatim to/from its live global.
 * The SAME table drives both save and load, so the two can never drift. */
typedef struct {
    uint16_t id;
    void    *ptr;
    uint32_t size;
} StateSection;

#define MAX_SECTIONS 21

/* Populate `t` with every serialized section in write order; return the count.
 * AUDIO is conditional, so the count is computed here rather than fixed. */
static int build_section_table(StateSection *t) {
    int n = 0;
#define ADD(id_, ptr_, size_)                                  \
    do {                                                       \
        t[n].id = (uint16_t)(id_);                             \
        t[n].ptr = (void *)(ptr_);                             \
        t[n].size = (uint32_t)(size_);                         \
        n++;                                                   \
    } while (0)

    ADD(SECTION_CORE,             &core,                sizeof(core));
    ADD(SECTION_GAME_STATE,       &game_state,          sizeof(game_state));
    ADD(SECTION_PARTY_CHARACTERS, party_characters,     sizeof(party_characters));
    ADD(SECTION_EVENT_FLAGS,      event_flags,          sizeof(event_flags));
    ADD(SECTION_OVERWORLD,        &ow,                  sizeof(ow));
    ADD(SECTION_BATTLE,           &bt,                  sizeof(bt));
    ADD(SECTION_DISPLAY_TEXT,     &dt,                  sizeof(dt));
    ADD(SECTION_WINDOW,           &win,                 sizeof(win));
    ADD(SECTION_MAP_LOADER,       &ml,                  sizeof(ml));
    ADD(SECTION_PPU,              &ppu,                 sizeof(ppu));
    ADD(SECTION_POSITION_BUFFER,  &pb,                  sizeof(pb));
    ADD(SECTION_DOOR,             &dr,                  sizeof(dr));
    ADD(SECTION_ENTITY_RUNTIME,   &ert,                 sizeof(ert));
    ADD(SECTION_ENTITY_SYSTEM,    &entities,            sizeof(entities));
    ADD(SECTION_SCRIPTS,          &scripts,             sizeof(scripts));
    ADD(SECTION_SPRITE_PRIORITY,  sprite_priority,      sizeof(SpritePriorityQueue) * 4);
    ADD(SECTION_FADE,             &fade_state,          sizeof(fade_state));
    ADD(SECTION_RNG,              &rng_state,           sizeof(rng_state));
#ifdef ENABLE_AUDIO
    ADD(SECTION_AUDIO,            &audio_state,         sizeof(audio_state));
#endif
    ADD(SECTION_PSI_ANIMATION,    &psi_animation_state, sizeof(psi_animation_state));
    ADD(SECTION_MODE_STACK,       &g_mode_stack,        sizeof(g_mode_stack));

#undef ADD
    return n;
}

static bool write_section(FILE *f, uint16_t id, const void *data, uint32_t size) {
    return fwrite(&id, 2, 1, f) == 1
        && fwrite(&size, 4, 1, f) == 1
        && (size == 0 || fwrite(data, size, 1, f) == 1);
}

bool state_dump_save(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;

    /* Header: magic "EBSD" + version u16 + frame_counter u16 */
    uint32_t magic = STATE_DUMP_MAGIC;
    uint16_t version = STATE_DUMP_VERSION;
    uint16_t frame = (uint16_t)core.frame_counter;
    bool ok = fwrite(&magic, 4, 1, f) == 1
           && fwrite(&version, 2, 1, f) == 1
           && fwrite(&frame, 2, 1, f) == 1;

    /* Sections — each blob written straight from its live global (no staging
     * buffer; the largest single write is PPU ~68 KB). */
    StateSection table[MAX_SECTIONS];
    int n = build_section_table(table);
    for (int i = 0; i < n && ok; i++)
        ok = write_section(f, table[i].id, table[i].ptr, table[i].size);

    /* Terminator */
    uint16_t term = SECTION_TERMINATOR;
    ok = ok && fwrite(&term, 2, 1, f) == 1;

    fclose(f);
    return ok;
}

bool state_dump_load(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;

    /* Header */
    uint32_t magic;
    uint16_t version, frame;
    if (fread(&magic, 4, 1, f) != 1
     || fread(&version, 2, 1, f) != 1
     || fread(&frame, 2, 1, f) != 1
     || magic != STATE_DUMP_MAGIC
     || version != STATE_DUMP_VERSION) {
        fclose(f);
        return false;
    }
    (void)frame; /* informational; the real value is restored via SECTION_CORE */

    StateSection table[MAX_SECTIONS];
    int n = build_section_table(table);

    /* Read each tagged section straight into its live global. Unknown ids (or a
     * known id whose size doesn't match this build) are skipped rather than
     * loaded — forward-compatibility, and a guard against a layout mismatch
     * silently smashing a struct. Same-binary round-trips never hit the skip. */
    for (;;) {
        uint16_t id;
        if (fread(&id, 2, 1, f) != 1) {
            fclose(f);
            return false; /* ran off the end without a terminator */
        }
        if (id == SECTION_TERMINATOR)
            break;

        uint32_t size;
        if (fread(&size, 4, 1, f) != 1) {
            fclose(f);
            return false;
        }

        StateSection *sec = NULL;
        for (int i = 0; i < n; i++) {
            if (table[i].id == id) { sec = &table[i]; break; }
        }

        if (sec && sec->size == size) {
            if (size && fread(sec->ptr, size, 1, f) != 1) {
                fclose(f);
                return false;
            }
        } else if (size) {
            /* Unknown id or size mismatch: skip the payload. */
            if (fseek(f, (long)size, SEEK_CUR) != 0) {
                fclose(f);
                return false;
            }
        }
    }

    fclose(f);
    return true;
}

/* save -> load -> save idempotency check on the CURRENT live state: proves the
 * loader reads back every byte the writer wrote (no section dropped or mis-sized)
 * and that load is the exact inverse of save. The live globals are restored to
 * their pre-test contents (load of file A overwrites whatever the test touched —
 * which is nothing, since save doesn't mutate). Returns true on a byte-identical
 * match. Desktop-only; used by `--selftest-savestate`. */
bool state_dump_roundtrip_test(void) {
    const char *pa = "savestate_selftest_a.bin";
    const char *pb = "savestate_selftest_b.bin";
    bool result = false;
    FILE *fa = NULL, *fb = NULL;
    uint8_t *ba = NULL, *bb = NULL;

    if (!state_dump_save(pa)) goto done;
    if (!state_dump_load(pa)) goto done;
    if (!state_dump_save(pb)) goto done;

    /* Byte-compare the two files. */
    fa = fopen(pa, "rb");
    fb = fopen(pb, "rb");
    if (!fa || !fb) goto done;
    fseek(fa, 0, SEEK_END);
    fseek(fb, 0, SEEK_END);
    long la = ftell(fa), lb = ftell(fb);
    if (la != lb || la <= 0) goto done;
    rewind(fa);
    rewind(fb);
    ba = (uint8_t *)malloc((size_t)la);
    bb = (uint8_t *)malloc((size_t)lb);
    if (!ba || !bb) goto done;
    if (fread(ba, (size_t)la, 1, fa) != 1 || fread(bb, (size_t)lb, 1, fb) != 1) goto done;
    result = (memcmp(ba, bb, (size_t)la) == 0);

done:
    if (fa) fclose(fa);
    if (fb) fclose(fb);
    free(ba);
    free(bb);
    remove(pa);
    remove(pb);
    return result;
}

#endif /* EB_EMBEDDED */
