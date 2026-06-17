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

bool state_dump_save_slots(const char *basepath) {
    (void)basepath;
    return false;
}

bool state_dump_load_slots(const char *basepath) {
    (void)basepath;
    return false;
}

bool state_dump_roundtrip_test(void) {
    return false;
}

bool state_dump_crashsafe_test(void) {
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
#include "entity/sprite.h"
#include "game/battle_bg.h"
#include "game/text.h"
#include "game/oval_window.h"
#include "game/flyover.h"
#include "game/inventory.h"
#include "game/ending.h"

/* Container format (16-byte header, all fields little-endian — asserted below):
 *   magic   u32  "EBSD"
 *   version u16
 *   frame   u16  informational (the authoritative value rides in SECTION_CORE)
 *   seq     u32  monotonic sequence — the ping-pong layer picks the highest valid one
 *   crc32   u32  CRC-32 of the payload that follows the header
 * Then the payload: id(u16)/size(u32)/blob sections, then a 0xFFFF terminator. See
 * the AUDIT in docs/plans/savestate-unified-loop.md. */
#define STATE_DUMP_MAGIC       0x44534245u  /* "EBSD" little-endian */
#define STATE_DUMP_HEADER_SIZE 16           /* magic+version+frame+seq+crc32 */
#define STATE_DUMP_VERSION 8  /* v6: raw-pointer purge (item #3A) changed PSI/oval/
                               * overworld-deferred layouts. v7: ABI hardening (item
                               * #3B) — PPUState.bg_viewport_fill enum→uint8_t shrank
                               * the PPU section; the format is now 32/64-bit identical.
                               * v8: crash-safe persistence (item #4) — added seq +
                               * payload CRC-32 to the header (validate-on-load). */

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
    /* Loose WRAM-equivalent globals that live outside the big module structs. */
    SECTION_SPRITE_VRAM_TABLE    = 0x0016,
    SECTION_OVERWORLD_SPRITEMAPS = 0x0017,
    SECTION_LOADED_BG_DATA_1     = 0x0018,
    SECTION_LOADED_BG_DATA_2     = 0x0019,
    SECTION_CURRENT_SAVE_SLOT    = 0x001A,
    /* VWF text-render engine cursor — the in-progress (typewriter) glyph state
     * that lives in text.c module globals, not in any captured struct. */
    SECTION_VWF_BUFFER           = 0x001B,
    SECTION_VWF_X                = 0x001C,
    SECTION_VWF_TILE             = 0x001D,
    SECTION_VWF_PIXELS_RENDERED  = 0x001E,
    SECTION_VWF_CHAR_PADDING     = 0x001F,
    SECTION_VWF_INDENT_NEWLINE   = 0x0020,
    SECTION_TEXT_RENDER_STATE    = 0x0021,
    /* Multi-frame animation / deferred-task / menu state that lives in per-file
     * module statics (captured via pack/unpack snapshots, below). */
    SECTION_OVAL_WINDOW          = 0x0022,
    SECTION_FLYOVER              = 0x0023,
    SECTION_OVERWORLD_DEFERRED   = 0x0024,
    SECTION_DOOR_TRANSITION      = 0x0025,
    SECTION_OW_PALETTE_BACKUP    = 0x0026,
    SECTION_TEXT_MENUS           = 0x0027,
    SECTION_ITEM_TRANSFORM       = 0x0028,
    SECTION_BATTLE_BG            = 0x0029,
    SECTION_FRAME_CALLBACK       = 0x002A,
    SECTION_TERMINATOR       = 0xFFFF,
};

/* ---- Cross-platform format contract (build item #3 part B) ----
 * The savestate must load identically on the 32-bit embedded targets (ARM ILP32)
 * and the 64-bit desktop (LP64). That requires (a) a fixed byte order and (b) every
 * directly-serialized struct having the SAME size + field offsets on both ABIs.
 *
 * (a) Endianness — all targets are little-endian; reject anything else at compile time. */
#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__)
#  error "savestate format assumes a little-endian target"
#endif

/* (b) ABI-stable section sizes. These hold on BOTH 32- and 64-bit because every
 * serialized struct is pointer-free OR wraps its pointers with ABI_PTR_ALIGN/PAD
 * (core/types.h) so the slot is 8 bytes/8-aligned everywhere, and every serialized
 * enum field has an explicit fixed underlying type (the only one,
 * PPUState.bg_viewport_fill, is `enum BGViewportMode : uint8_t`) so it is 1 byte even
 * under arm-none-eabi's default -fshort-enums. The numbers are the
 * canonical (identical) sizes; the SAME _Static_asserts compile in the embedded ARM
 * build and FAIL there if any struct's layout diverges (a stray raw pointer, a
 * size_t/long field, or an enum field under -fshort-enums) — i.e. they ARE the
 * permanent cross-ABI test. To verify out-of-tree: compile this set with
 * `arm-none-eabi-gcc -mthumb -ffreestanding -c` and confirm it builds. */
_Static_assert(sizeof(core)                 == 32,    "ABI: core");
_Static_assert(sizeof(game_state)           == 473,   "ABI: game_state");
_Static_assert(sizeof(party_characters)     == 570,   "ABI: party_characters");
_Static_assert(sizeof(event_flags)          == 128,   "ABI: event_flags");
_Static_assert(sizeof(ow)                   == 392,   "ABI: ow");
_Static_assert(sizeof(bt)                   == 3780,  "ABI: bt");
_Static_assert(sizeof(dt)                   == 152,   "ABI: dt");
_Static_assert(sizeof(win)                  == 12952, "ABI: win");
_Static_assert(sizeof(ml)                   == 16970, "ABI: ml");
_Static_assert(sizeof(ppu)                  == 68510, "ABI: ppu");
_Static_assert(sizeof(pb)                   == 2952,  "ABI: pb");
_Static_assert(sizeof(dr)                   == 44,    "ABI: dr");
_Static_assert(sizeof(ert)                  == 24392, "ABI: ert");
_Static_assert(sizeof(entities)             == 3818,  "ABI: entities");
_Static_assert(sizeof(scripts)              == 1750,  "ABI: scripts");
_Static_assert(sizeof(SpritePriorityQueue)  == 258,   "ABI: SpritePriorityQueue");
_Static_assert(sizeof(fade_state)           == 6,     "ABI: fade_state");
_Static_assert(sizeof(rng_state)            == 4,     "ABI: rng_state");
_Static_assert(sizeof(psi_animation_state)  == 88,    "ABI: psi_animation_state");
_Static_assert(sizeof(g_mode_stack)         == 3964,  "ABI: g_mode_stack");
_Static_assert(sizeof(overworld_spritemaps) == 900,   "ABI: overworld_spritemaps");
_Static_assert(sizeof(loaded_bg_data_layer1)== 124,   "ABI: loaded_bg_data_layer1");
_Static_assert(sizeof(text_render_state)    == 6,     "ABI: text_render_state");
#ifdef ENABLE_AUDIO
_Static_assert(sizeof(audio_state)          == 6,     "ABI: audio_state");
#endif

/* One serialized module: a tagged blob copied to/from its live storage. The SAME
 * table drives both save and load, so the two can never drift. Most sections point
 * `ptr` directly at a live global. A section whose live state is scattered across
 * file-private statics instead sets pack/unpack: `ptr` is then a static scratch
 * buffer; save calls pack(ptr) to gather the statics into it before writing, load
 * calls unpack(ptr) to scatter them back after reading. */
typedef struct {
    uint16_t id;
    void    *ptr;
    uint32_t size;
    void (*pack)(void *scratch);         /* NULL for direct sections */
    void (*unpack)(const void *scratch); /* NULL for direct sections */
} StateSection;

#define MAX_SECTIONS 44

/* Populate `t` with every serialized section in write order; return the count.
 * AUDIO is conditional, so the count is computed here rather than fixed. */
/* Static scratch buffers for the pack/unpack (file-static-backed) sections. */
static OvalWindowSaveState     s_oval_ss;
static FlyoverSaveState        s_flyover_ss;
static OverworldDeferredSaveState s_ow_deferred_ss;
static DoorTransitionSaveState s_door_tr_ss;
static OwPaletteBackupSaveState s_ow_pal_ss;
static TextMenuSaveState       s_text_menu_ss;
static ItemTransformSaveState  s_item_xform_ss;
static BattleBgSaveState       s_battle_bg_ss;
static FrameCallbackSaveState  s_frame_cb_ss;

static int build_section_table(StateSection *t) {
    int n = 0;
#define ADD(id_, ptr_, size_)                                  \
    do {                                                       \
        t[n].id = (uint16_t)(id_);                             \
        t[n].ptr = (void *)(ptr_);                             \
        t[n].size = (uint32_t)(size_);                         \
        t[n].pack = NULL;                                      \
        t[n].unpack = NULL;                                    \
        n++;                                                   \
    } while (0)
#define ADDFN(id_, scratch_, size_, pack_, unpack_)            \
    do {                                                       \
        t[n].id = (uint16_t)(id_);                             \
        t[n].ptr = (void *)(scratch_);                         \
        t[n].size = (uint32_t)(size_);                         \
        t[n].pack = (pack_);                                   \
        t[n].unpack = (unpack_);                               \
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

    /* Loose globals outside the module structs that are nevertheless live game
     * state. Without these, a load leaves stale runtime bookkeeping: the sprite
     * VRAM-slot allocation map and overworld spritemap buffer (entities store
     * offsets into it) → sprite artifacts; the battle BG layer config → battle BG
     * artifacts; the active save slot. */
    ADD(SECTION_SPRITE_VRAM_TABLE,    sprite_vram_table,      sizeof(sprite_vram_table));
    ADD(SECTION_OVERWORLD_SPRITEMAPS, overworld_spritemaps,   sizeof(overworld_spritemaps));
    ADD(SECTION_LOADED_BG_DATA_1,     &loaded_bg_data_layer1, sizeof(loaded_bg_data_layer1));
    ADD(SECTION_LOADED_BG_DATA_2,     &loaded_bg_data_layer2, sizeof(loaded_bg_data_layer2));
    ADD(SECTION_CURRENT_SAVE_SLOT,    &current_save_slot,     sizeof(current_save_slot));

    /* VWF text-render engine cursor (text.c globals): without these a savestate
     * taken mid-typewriter resumes the partial glyph run with a stale cursor →
     * corrupt NPC dialogue. The already-rendered glyphs live in VRAM (PPU) and the
     * window content tilemaps (WINDOW); this is the in-progress render position. */
    ADD(SECTION_VWF_BUFFER,           vwf_buffer,             sizeof(vwf_buffer));
    ADD(SECTION_VWF_X,                &vwf_x,                 sizeof(vwf_x));
    ADD(SECTION_VWF_TILE,             &vwf_tile,              sizeof(vwf_tile));
    ADD(SECTION_VWF_PIXELS_RENDERED,  &vwf_pixels_rendered,   sizeof(vwf_pixels_rendered));
    ADD(SECTION_VWF_CHAR_PADDING,     &character_padding,     sizeof(character_padding));
    ADD(SECTION_VWF_INDENT_NEWLINE,   &vwf_indent_new_line,   sizeof(vwf_indent_new_line));
    ADD(SECTION_TEXT_RENDER_STATE,    &text_render_state,     sizeof(text_render_state));

    /* Multi-frame animation / deferred-task / menu state held in per-file statics.
     * These drive operations that span many frames (battle swirl, oval-window and
     * flyover scrolls, escalator/stairs forced-walk, item-ripen timers, equip/PSI
     * menu previews); a savestate taken mid-operation must round-trip them or the
     * resumed operation corrupts. Each is gathered/scattered via a pack/unpack pair
     * owned by the file that holds the statics. (Some still embed raw pointers —
     * fine in-process; the cross-platform pointer purge is build-order item #3.) */
    ADDFN(SECTION_OVAL_WINDOW,        &s_oval_ss,        sizeof(s_oval_ss),
          oval_window_savestate_pack,   oval_window_savestate_unpack);
    ADDFN(SECTION_FLYOVER,            &s_flyover_ss,     sizeof(s_flyover_ss),
          flyover_savestate_pack,       flyover_savestate_unpack);
    ADDFN(SECTION_OVERWORLD_DEFERRED, &s_ow_deferred_ss, sizeof(s_ow_deferred_ss),
          overworld_deferred_savestate_pack, overworld_deferred_savestate_unpack);
    ADDFN(SECTION_DOOR_TRANSITION,    &s_door_tr_ss,     sizeof(s_door_tr_ss),
          door_transition_savestate_pack, door_transition_savestate_unpack);
    ADDFN(SECTION_OW_PALETTE_BACKUP,  &s_ow_pal_ss,      sizeof(s_ow_pal_ss),
          ow_palette_backup_savestate_pack, ow_palette_backup_savestate_unpack);
    ADDFN(SECTION_TEXT_MENUS,         &s_text_menu_ss,   sizeof(s_text_menu_ss),
          text_menus_savestate_pack,    text_menus_savestate_unpack);
    ADDFN(SECTION_ITEM_TRANSFORM,     &s_item_xform_ss,  sizeof(s_item_xform_ss),
          item_transform_savestate_pack, item_transform_savestate_unpack);
    ADDFN(SECTION_BATTLE_BG,          &s_battle_bg_ss,   sizeof(s_battle_bg_ss),
          battle_bg_savestate_pack,     battle_bg_savestate_unpack);
    ADDFN(SECTION_FRAME_CALLBACK,     &s_frame_cb_ss,    sizeof(s_frame_cb_ss),
          frame_callback_savestate_pack, frame_callback_savestate_unpack);

#undef ADD
#undef ADDFN
    return n;
}

static bool write_section(FILE *f, uint16_t id, const void *data, uint32_t size) {
    return fwrite(&id, 2, 1, f) == 1
        && fwrite(&size, 4, 1, f) == 1
        && (size == 0 || fwrite(data, size, 1, f) == 1);
}

/* Standard CRC-32 (reflected, polynomial 0xEDB88420). This step function neither
 * seeds nor finalizes — the caller seeds with 0xFFFFFFFF and XORs the result with
 * 0xFFFFFFFF — so the SAME routine serves both the streaming validate pass (raw file
 * bytes) and the section-walk compute (in-memory bytes). No static table: a couple of
 * passes over ~139 KiB at save/load time is not perf-critical, and avoiding a 1 KiB
 * table keeps the embedded port allocation-free. */
static uint32_t crc32_step(uint32_t crc, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88420u & (uint32_t)(-(int32_t)(crc & 1u)));
    }
    return crc;
}

/* CRC-32 of the exact byte stream the section walk writes (id u16, size u32, data),
 * followed by the 0xFFFF terminator. Relies on the asserted little-endian target so
 * the in-memory bytes of id/size equal the bytes written to the file. Run AFTER each
 * pack() has gathered its statics into the scratch buffer. */
static uint32_t compute_payload_crc(const StateSection *table, int n) {
    uint32_t crc = 0xFFFFFFFFu;
    for (int i = 0; i < n; i++) {
        uint16_t id = table[i].id;
        uint32_t size = table[i].size;
        crc = crc32_step(crc, &id, 2);
        crc = crc32_step(crc, &size, 4);
        if (size)
            crc = crc32_step(crc, table[i].ptr, size);
    }
    uint16_t term = SECTION_TERMINATOR;
    crc = crc32_step(crc, &term, 2);
    return crc ^ 0xFFFFFFFFu;
}

/* Read & validate the fixed header. On success fills *seq/*crc (either may be NULL)
 * and leaves the file positioned at the payload start. */
static bool read_header(FILE *f, uint32_t *seq_out, uint32_t *crc_out) {
    uint32_t magic, seq, crc;
    uint16_t version, frame;
    if (fread(&magic, 4, 1, f) != 1
     || fread(&version, 2, 1, f) != 1
     || fread(&frame, 2, 1, f) != 1
     || fread(&seq, 4, 1, f) != 1
     || fread(&crc, 4, 1, f) != 1
     || magic != STATE_DUMP_MAGIC
     || version != STATE_DUMP_VERSION)
        return false;
    (void)frame; /* informational; the real value is restored via SECTION_CORE */
    if (seq_out) *seq_out = seq;
    if (crc_out) *crc_out = crc;
    return true;
}

/* Stream the payload (file must already be positioned at the payload start) through
 * CRC-32 and compare to `expected`. Leaves the file position at EOF. */
static bool verify_payload_crc(FILE *f, uint32_t expected) {
    uint32_t crc = 0xFFFFFFFFu;
    uint8_t buf[4096];
    size_t r;
    while ((r = fread(buf, 1, sizeof buf, f)) > 0)
        crc = crc32_step(crc, buf, r);
    crc ^= 0xFFFFFFFFu;
    return crc == expected;
}

/* Write a complete savestate to `path` tagged with sequence number `seq`. The CRC is
 * computed in a pre-pass over the section table (no seek-back, no staging of the
 * whole snapshot), so the embedded port can stream the same layout. */
static bool state_dump_save_seq(const char *path, uint32_t seq) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;

    /* Gather the pack/unpack sections' scattered statics into their scratch buffers
     * BEFORE both the CRC pre-pass and the write, so the two see identical bytes. */
    StateSection table[MAX_SECTIONS];
    int n = build_section_table(table);
    for (int i = 0; i < n; i++)
        if (table[i].pack)
            table[i].pack(table[i].ptr);

    uint32_t magic = STATE_DUMP_MAGIC;
    uint16_t version = STATE_DUMP_VERSION;
    uint16_t frame = (uint16_t)core.frame_counter;
    uint32_t crc = compute_payload_crc(table, n);

    bool ok = fwrite(&magic, 4, 1, f) == 1
           && fwrite(&version, 2, 1, f) == 1
           && fwrite(&frame, 2, 1, f) == 1
           && fwrite(&seq, 4, 1, f) == 1
           && fwrite(&crc, 4, 1, f) == 1;

    /* Sections — each direct blob written straight from its live global (no staging
     * buffer; the largest single write is PPU ~68 KB). Do NOT re-pack here: the
     * scratch buffers already hold the bytes the CRC was computed over. */
    for (int i = 0; i < n && ok; i++)
        ok = write_section(f, table[i].id, table[i].ptr, table[i].size);

    /* Terminator */
    uint16_t term = SECTION_TERMINATOR;
    ok = ok && fwrite(&term, 2, 1, f) == 1;

    if (fclose(f) != 0)
        ok = false;
    return ok;
}

bool state_dump_save(const char *path) {
    return state_dump_save_seq(path, 0);
}

bool state_dump_load(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;

    uint32_t crc;
    if (!read_header(f, NULL, &crc)) {
        fclose(f);
        return false;
    }

    /* Validate the WHOLE payload before touching any live state, so a torn/corrupt
     * file never partially overwrites the running game (the in-place apply pass below
     * cannot be undone). */
    if (!verify_payload_crc(f, crc)) {
        fclose(f);
        return false;
    }

    /* Apply pass: rewind to the payload and read each section into its live global. */
    if (fseek(f, STATE_DUMP_HEADER_SIZE, SEEK_SET) != 0) {
        fclose(f);
        return false;
    }

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
            /* pack/unpack section: scatter the scratch buffer back into its statics. */
            if (sec->unpack)
                sec->unpack(sec->ptr);
        } else if (size) {
            /* Unknown id or size mismatch: skip the payload. */
            if (fseek(f, (long)size, SEEK_CUR) != 0) {
                fclose(f);
                return false;
            }
        }
    }

    fclose(f);

    /* Rebuild the raw pointers in the directly-serialized sections from their
     * serializable companions (offsets / ids) — savestate pointer purge, build
     * item #3. The pack/unpack sections rebuild their own pointers inside unpack().
     * Without this, content_tilemap / cursor_move_callback / post_teleport_callback /
     * the PSI streaming pointers hold stale addresses after a cross-process load. */
    window_savestate_rebind();
    overworld_savestate_rebind();
    psi_animation_savestate_rebind();

    return true;
}

/* ---- Crash-safe ping-pong persistence (build-order item #4) ---- */

static void slot_path(char *out, size_t cap, const char *base, int slot) {
    snprintf(out, cap, "%s.%d", base, slot);
}

/* Read a slot's header and validate its payload CRC. Returns true (and fills *seq)
 * iff the slot is a loadable savestate; false if missing/short/wrong-version/torn. */
static bool slot_peek(const char *path, uint32_t *seq) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint32_t crc;
    bool ok = read_header(f, seq, &crc) && verify_payload_crc(f, crc);
    fclose(f);
    return ok;
}

bool state_dump_save_slots(const char *basepath) {
    char p0[512], p1[512];
    slot_path(p0, sizeof p0, basepath, 0);
    slot_path(p1, sizeof p1, basepath, 1);

    uint32_t s0 = 0, s1 = 0;
    bool ok0 = slot_peek(p0, &s0);
    bool ok1 = slot_peek(p1, &s1);

    /* next sequence = max(valid sequences) + 1 (>= 1). */
    uint32_t maxseq = 0;
    bool any = false;
    if (ok0)                          { maxseq = s0; any = true; }
    if (ok1 && (!any || s1 > maxseq)) { maxseq = s1; any = true; }
    uint32_t next = any ? maxseq + 1 : 1;

    /* Write to the slot that is NOT the current newest-valid one, so that slot stays
     * intact until the new write commits (atomic via the sequence number). */
    bool slot0_is_newest = ok0 && (!ok1 || s0 >= s1);
    const char *target = slot0_is_newest ? p1 : p0;
    return state_dump_save_seq(target, next);
}

bool state_dump_load_slots(const char *basepath) {
    char p0[512], p1[512];
    slot_path(p0, sizeof p0, basepath, 0);
    slot_path(p1, sizeof p1, basepath, 1);

    uint32_t s0 = 0, s1 = 0;
    bool ok0 = slot_peek(p0, &s0);
    bool ok1 = slot_peek(p1, &s1);

    /* Try the valid slots newest-first; fall back to the older one if the newest
     * fails to apply (it passed CRC at peek, so this is belt-and-suspenders). */
    const char *first = NULL, *second = NULL;
    if (ok0 && ok1) {
        if (s0 >= s1) { first = p0; second = p1; }
        else          { first = p1; second = p0; }
    } else if (ok0) {
        first = p0;
    } else if (ok1) {
        first = p1;
    } else {
        return false;
    }

    if (first && state_dump_load(first)) return true;
    if (second && state_dump_load(second)) return true;
    return false;
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

/* Flip one payload byte (just past the header) of `path` so its CRC no longer
 * matches — simulates a torn write. */
static void corrupt_payload_byte(const char *path) {
    FILE *f = fopen(path, "r+b");
    if (!f) return;
    if (fseek(f, STATE_DUMP_HEADER_SIZE, SEEK_SET) == 0) {
        int c = fgetc(f);
        if (c != EOF) {
            fseek(f, STATE_DUMP_HEADER_SIZE, SEEK_SET);
            fputc(c ^ 0xFF, f);
        }
    }
    fclose(f);
}

/* Crash-safety self-test for the ping-pong layer: two saves must land in different
 * slots with increasing sequence; corrupting the newer slot must make a load fall
 * back to the older one; corrupting both must make the load fail cleanly (never apply
 * a torn file). Returns true on success. Desktop-only; used by `--selftest-savestate`.
 * Loading overwrites the live globals with the (identical) snapshot just written, so
 * it is harmless to run on the post-boot state. */
bool state_dump_crashsafe_test(void) {
    const char *base = "savestate_cs_test";
    char p0[512], p1[512];
    slot_path(p0, sizeof p0, base, 0);
    slot_path(p1, sizeof p1, base, 1);
    remove(p0);
    remove(p1);
    bool result = false;

    /* Two saves → two distinct slots with increasing sequence. */
    if (!state_dump_save_slots(base)) goto done;
    if (!state_dump_save_slots(base)) goto done;

    uint32_t s0 = 0, s1 = 0;
    if (!slot_peek(p0, &s0) || !slot_peek(p1, &s1) || s0 == s1) goto done;

    const char *newer = (s0 > s1) ? p0 : p1;
    const char *older = (s0 > s1) ? p1 : p0;

    /* Corrupt the newer slot: it must no longer validate, and a load must fall back
     * to the older (still-valid) slot. */
    corrupt_payload_byte(newer);
    {
        uint32_t dummy;
        if (slot_peek(newer, &dummy)) goto done; /* corruption must invalidate it */
    }
    if (!state_dump_load_slots(base)) goto done; /* must succeed via the older slot */

    /* Corrupt the older slot too: with no valid slot, the load must fail cleanly. */
    corrupt_payload_byte(older);
    if (state_dump_load_slots(base)) goto done;

    result = true;
done:
    remove(p0);
    remove(p1);
    return result;
}

#endif /* EB_EMBEDDED */
