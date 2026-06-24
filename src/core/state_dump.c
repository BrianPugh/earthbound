#include "core/state_dump.h"

/* The savestate serialization core is compiled for EVERY target (build-order item #5:
 * expose the porting-layer machinery). Storage is reached ONLY through the
 * platform_savestate_* slot hooks (platform.h): desktop = files
 * (port/unix/platform/sdl2_savestate.c); embedded = firmware flash regions
 * (target-specific, wired in a later session). Until an embedded target provides a real
 * backend, src/platform/savestate_backend_stub.c supplies no-op hooks so the build links
 * and a capture fails safe (returns false) instead of tearing. */

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "core/memory.h"
#include "core/math.h"
#include "core/mode_stack.h"
#include "platform/platform.h"
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
#include "intro/file_select.h"
#include "core/tamp/compressor.h"
#include "core/tamp/decompressor.h"

/* Container format (20-byte header, all fields little-endian — asserted below):
 *   magic       u32  "EBSD"
 *   version     u16
 *   frame       u16  informational (the authoritative value rides in SECTION_CORE)
 *   seq         u32  monotonic sequence — the ping-pong layer picks the highest valid
 *   crc32       u32  CRC-32 of the COMPRESSED payload that follows the header
 *   payload_len u32  byte length of the COMPRESSED payload (so the offset-addressed
 *                    slot backend knows how much to read without an EOF signal)
 * Then the payload: a single tamp-compressed stream whose decompressed form is the
 * id(u16)/size(u32)/blob sections followed by a 0xFFFF terminator. The header stays
 * uncompressed so peek/header/CRC paths are cheap raw reads; the CRC is over the
 * stored (compressed) bytes so verify never has to decompress. See the AUDIT in
 * docs/plans/savestate-unified-loop.md. */
#define STATE_DUMP_MAGIC       0x44534245u  /* "EBSD" little-endian */
#define STATE_DUMP_HEADER_SIZE 20           /* magic+version+frame+seq+crc32+payload_len */
#define STATE_DUMP_VERSION 11 /* v6: raw-pointer purge (item #3A) changed PSI/oval/
                               * overworld-deferred layouts. v7: ABI hardening (item
                               * #3B) — PPUState.bg_viewport_fill enum→uint8_t shrank
                               * the PPU section; the format is now 32/64-bit identical.
                               * v8: crash-safe persistence (item #4) — added seq +
                               * payload CRC-32 to the header (validate-on-load).
                               * v9: storage moved onto the platform_savestate_* slot
                               * backend (item #5) — added payload_len to the header.
                               * v10: SECTION_PPU now serializes only the fixed PPUState
                               * prefix — the EB_VIEWPORT_HEIGHT-sized per-scanline HDMA
                               * tables are excluded (derived render scratch), making the
                               * format portable across viewport dimensions.
                               * v11: the payload (sections + terminator) is now a single
                               * tamp-compressed stream; the header's crc32/payload_len
                               * describe the COMPRESSED bytes. Header stays uncompressed. */

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
/* SECTION_PPU serializes only the fixed PREFIX of PPUState — everything before the
 * per-scanline HDMA tables (wh0/wh1/wh2/wh3_table, tm/ts_per_scanline), which are
 * EB_VIEWPORT_HEIGHT-sized derived render scratch deliberately excluded so the format
 * stays fixed-size and portable across viewport dimensions (see the savestate boundary
 * comment in snes/ppu.h). The prefix is viewport-independent, so its size is a plain
 * constant again; a stray pointer/size_t/short-enum in the prefix still trips this. */
_Static_assert(offsetof(PPUState, wh0_table) == 67166, "ABI: ppu");
/* The six per-scanline tables must be EXACTLY the tail of PPUState — nothing serialized
 * may follow them, or it would be silently dropped from the save. This pins that: prefix
 * + the six EB_VIEWPORT_HEIGHT-sized uint8_t tables == the whole struct. Any field added
 * after the tables (or a table inserted/removed) trips this right here, next to the
 * serialization, rather than relying on the LAST_BLOB_FIELD guard in gen_struct_info.c. */
_Static_assert(offsetof(PPUState, wh0_table) + 6 * EB_VIEWPORT_HEIGHT == sizeof(PPUState),
               "PPUState: serialized prefix + per-scanline tables must cover the whole struct");
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
#ifdef EB_ENABLE_AUDIO
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
    ADD(SECTION_PPU,              &ppu,                 offsetof(PPUState, wh0_table));
    ADD(SECTION_POSITION_BUFFER,  &pb,                  sizeof(pb));
    ADD(SECTION_DOOR,             &dr,                  sizeof(dr));
    ADD(SECTION_ENTITY_RUNTIME,   &ert,                 sizeof(ert));
    ADD(SECTION_ENTITY_SYSTEM,    &entities,            sizeof(entities));
    ADD(SECTION_SCRIPTS,          &scripts,             sizeof(scripts));
    ADD(SECTION_SPRITE_PRIORITY,  sprite_priority,      sizeof(SpritePriorityQueue) * 4);
    ADD(SECTION_FADE,             &fade_state,          sizeof(fade_state));
    ADD(SECTION_RNG,              &rng_state,           sizeof(rng_state));
#ifdef EB_ENABLE_AUDIO
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

/* Standard CRC-32 (reflected, polynomial 0xEDB88420). This step function neither
 * seeds nor finalizes — the caller seeds with 0xFFFFFFFF and XORs the result with
 * 0xFFFFFFFF — so the SAME routine serves both the streaming validate pass (raw slot
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

/* ---- Storage on the platform_savestate_* slot backend (build-order item #5) ----
 * All raw byte I/O goes through the port's offset/slot-addressed hooks (file-backed
 * on desktop, flash on embedded). The format/ping-pong/CRC logic lives here in core;
 * the port owns only the bytes + durability. Every multi-byte field is written/read
 * as its little-endian in-memory bytes (the target is asserted little-endian).
 *
 * Since v11 the PAYLOAD (sections + terminator) is a single tamp-compressed stream
 * (item: savestate compression). The 20-byte header stays uncompressed at offset 0;
 * the window/struct/I/O-staging the (de)compressor needs come from the port's
 * platform_savestate_scratch() lend (no persistent RAM). The stored CRC covers the
 * COMPRESSED bytes, so peek/verify never decompress. */

/* tamp LZ window = 2^8 = 256 bytes (measured: only marginally worse ratio than a
 * larger window on this data, and a small window keeps match-search fast). */
#define SS_WINDOW_BITS  8u
#define SS_WINDOW_SIZE  (1u << SS_WINDOW_BITS)
#define SS_STAGE_MAX    16384u   /* cap on compressed-I/O staging (chunky SD ops) */
#define SS_STAGE_MIN    512u     /* refuse to run if the scratch lend is smaller */

static unsigned char *ss_align8(unsigned char *p) {
    uintptr_t a = ((uintptr_t)p + 7u) & ~(uintptr_t)7u;
    return (unsigned char *)a;
}

/* Compressing sequential writer. Replaces the old raw SsWriter: the section walk's
 * byte stream is sunk through a TampCompressor and the compressed output is staged
 * and flushed to the slot in chunks. The window, the compressor struct, and the
 * staging buffer all live in the port's scratch lend (none on the stack). The
 * running CRC accumulates over the COMPRESSED bytes as they are flushed, so the
 * header's crc/len match what verify_slot_crc later reads back. */
typedef struct {
    TampCompressor *c;
    unsigned char  *stage;      /* compressed-output staging (in the scratch lend) */
    size_t          stage_cap;
    size_t          stage_fill;
    int             slot;
    size_t          out_off;    /* slot offset for the next staged flush (>= header) */
    uint32_t        crc;        /* running CRC of compressed bytes (pre final XOR) */
    size_t          comp_len;   /* total compressed bytes written */
    bool            ok;
} CompCtx;

static void comp_flush_stage(CompCtx *x) {
    if (!x->ok || x->stage_fill == 0)
        return;
    if (!platform_savestate_write(x->slot, x->out_off, x->stage, x->stage_fill)) {
        x->ok = false;
        return;
    }
    x->crc       = crc32_step(x->crc, x->stage, x->stage_fill);
    x->out_off  += x->stage_fill;
    x->comp_len += x->stage_fill;
    x->stage_fill = 0;
}

static bool comp_init(CompCtx *x, int slot) {
    size_t region = 0;
    unsigned char *base = (unsigned char *)platform_savestate_scratch(&region);
    if (!base)
        return false;
    unsigned char *p = base;
    unsigned char *window = p;            p += SS_WINDOW_SIZE;
    p = ss_align8(p);
    x->c = (TampCompressor *)p;           p += sizeof(TampCompressor);
    p = ss_align8(p);
    size_t used = (size_t)(p - base);
    if (region < used + SS_STAGE_MIN)
        return false;
    size_t cap = region - used;
    if (cap > SS_STAGE_MAX)
        cap = SS_STAGE_MAX;
    x->stage      = p;
    x->stage_cap  = cap;
    x->stage_fill = 0;
    x->slot       = slot;
    x->out_off    = STATE_DUMP_HEADER_SIZE;
    x->crc        = 0xFFFFFFFFu;
    x->comp_len   = 0;
    x->ok         = true;
    const TampConf conf = { .window = SS_WINDOW_BITS, .literal = 8, .use_custom_dictionary = 0 };
    if (tamp_compressor_init(x->c, &conf, window) != TAMP_OK)
        x->ok = false;
    return x->ok;
}

/* Compress one token out of the input buffer into staging (flushing staging to SD
 * first if it's nearly full — a poll emits at most (16+WINDOW_BITS)/8 = 3 bytes). */
static void comp_poll(CompCtx *x) {
    if (!x->ok)
        return;
    if (x->stage_cap - x->stage_fill < 32)
        comp_flush_stage(x);
    if (!x->ok)
        return;
    size_t written = 0;
    if (tamp_compressor_poll(x->c, x->stage + x->stage_fill,
                             x->stage_cap - x->stage_fill, &written) != TAMP_OK) {
        x->ok = false;
        return;
    }
    x->stage_fill += written;
}

static void comp_write(CompCtx *x, const void *src, size_t n) {
    const unsigned char *p = (const unsigned char *)src;
    while (n && x->ok) {
        size_t consumed = 0;
        tamp_compressor_sink(x->c, p, n, &consumed);
        p += consumed;
        n -= consumed;
        if (tamp_compressor_full(x->c))
            comp_poll(x);
    }
}
static void comp_u16(CompCtx *x, uint16_t v) { comp_write(x, &v, 2); }
static void comp_u32(CompCtx *x, uint32_t v) { comp_write(x, &v, 4); }

/* Drain the compressor (remaining buffered input + bit buffer), flush staging, and
 * finalize the CRC. flush needs all its output contiguous, so empty the staging
 * first (cap >= SS_STAGE_MIN comfortably exceeds flush's worst case). */
static void comp_finish(CompCtx *x) {
    if (!x->ok)
        return;
    comp_flush_stage(x);
    if (!x->ok)
        return;
    size_t written = 0;
    if (tamp_compressor_flush(x->c, x->stage + x->stage_fill,
                              x->stage_cap - x->stage_fill, &written,
                              /*write_token=*/false) != TAMP_OK) {
        x->ok = false;
        return;
    }
    x->stage_fill += written;
    comp_flush_stage(x);
    if (x->ok)
        x->crc ^= 0xFFFFFFFFu;
}

/* Write the 20-byte uncompressed header at offset 0. Called LAST (after the compress
 * pass) because crc/comp_len aren't known until then. */
static bool write_header(int slot, uint16_t frame, uint32_t seq,
                         uint32_t crc, uint32_t comp_len) {
    uint8_t h[STATE_DUMP_HEADER_SIZE];
    uint32_t magic = STATE_DUMP_MAGIC;
    uint16_t ver   = STATE_DUMP_VERSION;
    memcpy(h + 0,  &magic,    4);
    memcpy(h + 4,  &ver,      2);
    memcpy(h + 6,  &frame,    2);
    memcpy(h + 8,  &seq,      4);
    memcpy(h + 12, &crc,      4);
    memcpy(h + 16, &comp_len, 4);
    return platform_savestate_write(slot, 0, h, sizeof h);
}

/* Serialize the live state into `slot`, tagged with sequence `seq`, as a compressed
 * stream. Single pass: the section walk is compressed straight to the slot, then the
 * header (with the resulting compressed CRC + length) is written back at offset 0.
 * Returns true only on a fully committed write; a partial/failed write is left
 * uncommitted (it fails CRC validation later, so the prior slot stays newest-valid). */
static bool write_slot(int slot, uint32_t seq) {
    /* Gather the pack/unpack sections' scattered statics into their scratch buffers
     * BEFORE the compress pass so the bytes are coherent. */
    StateSection table[MAX_SECTIONS];
    int n = build_section_table(table);
    for (int i = 0; i < n; i++)
        if (table[i].pack)
            table[i].pack(table[i].ptr);

    if (!platform_savestate_begin(slot))
        return false;

    CompCtx x;
    if (!comp_init(&x, slot)) {
        platform_savestate_commit(slot);   /* close the truncated (invalid) slot */
        return false;
    }

    /* Compress: id u16 + size u32 + data per section, then the terminator. The
     * decompressed stream is byte-identical to the pre-v11 uncompressed payload. */
    for (int i = 0; i < n; i++) {
        comp_u16(&x, table[i].id);
        comp_u32(&x, table[i].size);
        if (table[i].size)
            comp_write(&x, table[i].ptr, table[i].size); /* straight from the live global */
    }
    comp_u16(&x, SECTION_TERMINATOR);
    comp_finish(&x);

    bool hdr = x.ok && write_header(slot, (uint16_t)core.frame_counter, seq,
                                    x.crc, (uint32_t)x.comp_len);

    /* Always commit (closes/flushes the open writer); succeed only if every write
     * landed. A torn write produces an invalid slot, never a half-applied load. */
    bool committed = platform_savestate_commit(slot);
    return x.ok && hdr && committed;
}

/* Read & validate the 20-byte header from `slot`. On success fills seq/crc/payload_len
 * (any may be NULL). */
static bool read_slot_header(int slot, uint32_t *seq, uint32_t *crc, uint32_t *plen) {
    uint8_t h[STATE_DUMP_HEADER_SIZE];
    if (platform_savestate_read(slot, 0, h, sizeof h) != sizeof h)
        return false;
    uint32_t magic, sq, cc, pl;
    uint16_t version;
    memcpy(&magic,   h + 0,  4);
    memcpy(&version, h + 4,  2);
    /* h + 6: frame (informational, restored via SECTION_CORE) */
    memcpy(&sq,      h + 8,  4);
    memcpy(&cc,      h + 12, 4);
    memcpy(&pl,      h + 16, 4);
    if (magic != STATE_DUMP_MAGIC || version != STATE_DUMP_VERSION)
        return false;
    if (seq)  *seq  = sq;
    if (crc)  *crc  = cc;
    if (plen) *plen = pl;
    return true;
}

/* Stream `payload_len` payload bytes of `slot` through CRC-32 and compare. */
static bool verify_slot_crc(int slot, uint32_t payload_len, uint32_t expected) {
    uint32_t crc = 0xFFFFFFFFu;
    uint8_t buf[4096];
    size_t off = STATE_DUMP_HEADER_SIZE;
    uint32_t remaining = payload_len;
    while (remaining) {
        size_t chunk = remaining < sizeof buf ? remaining : sizeof buf;
        if (platform_savestate_read(slot, off, buf, chunk) != chunk)
            return false;
        crc = crc32_step(crc, buf, chunk);
        off += chunk;
        remaining -= (uint32_t)chunk;
    }
    return (crc ^ 0xFFFFFFFFu) == expected;
}

/* Validate a slot's header + payload CRC. Returns true (and fills *seq) iff the slot
 * is a loadable savestate; false if absent/short/wrong-version/torn. */
static bool peek_slot(int slot, uint32_t *seq) {
    uint32_t crc, plen, sq;
    if (!read_slot_header(slot, &sq, &crc, &plen))
        return false;
    if (!verify_slot_crc(slot, plen, crc))
        return false;
    if (seq) *seq = sq;
    return true;
}

/* Decompressing sequential reader. Replaces the raw SsReader: the apply pass pulls
 * the section stream through a TampDecompressor instead of reading slot bytes
 * directly. The window, the decompressor struct, and the compressed-input staging
 * all live in the port's scratch lend (none on the stack). Compressed bytes are read
 * from the slot starting after the header; tamp self-configures its window/literal
 * bits from the 1-byte stream header (conf = NULL). */
typedef struct {
    TampDecompressor *d;
    unsigned char    *in;        /* compressed-input staging (in the scratch lend) */
    size_t            in_cap;
    size_t            in_fill;    /* valid bytes in staging */
    size_t            in_pos;     /* bytes already consumed from staging */
    int               slot;
    size_t            src_off;    /* next compressed byte offset in the slot */
    uint32_t          src_left;   /* compressed bytes not yet pulled from the slot */
    bool              ok;
} DecompCtx;

static bool decomp_init(DecompCtx *x, int slot, uint32_t comp_len) {
    size_t region = 0;
    unsigned char *base = (unsigned char *)platform_savestate_scratch(&region);
    if (!base)
        return false;
    unsigned char *p = base;
    unsigned char *window = p;            p += SS_WINDOW_SIZE;
    p = ss_align8(p);
    x->d = (TampDecompressor *)p;         p += sizeof(TampDecompressor);
    p = ss_align8(p);
    size_t used = (size_t)(p - base);
    if (region < used + SS_STAGE_MIN)
        return false;
    size_t cap = region - used;
    if (cap > SS_STAGE_MAX)
        cap = SS_STAGE_MAX;
    x->in       = p;
    x->in_cap   = cap;
    x->in_fill  = 0;
    x->in_pos   = 0;
    x->slot     = slot;
    x->src_off  = STATE_DUMP_HEADER_SIZE;
    x->src_left = comp_len;
    x->ok       = true;
    if (tamp_decompressor_init(x->d, NULL /*conf from stream*/, window, SS_WINDOW_BITS) != TAMP_OK)
        x->ok = false;
    return x->ok;
}

/* Deliver exactly `n` decompressed bytes into `dst`; false on a short/torn stream or
 * decode error. (The CRC pass already validated the compressed bytes, so a verified
 * slot never errors here.) */
static bool decomp_read(DecompCtx *x, void *dst, size_t n) {
    if (!x->ok)
        return false;
    unsigned char *out = (unsigned char *)dst;
    size_t done = 0;
    while (done < n) {
        if (x->in_pos >= x->in_fill && x->src_left > 0) {   /* drained — refill from slot */
            size_t chunk = x->src_left < x->in_cap ? x->src_left : x->in_cap;
            size_t got = platform_savestate_read(x->slot, x->src_off, x->in, chunk);
            if (got == 0) { x->ok = false; return false; }
            x->in_fill   = got;
            x->in_pos    = 0;
            x->src_off  += got;
            x->src_left -= (uint32_t)got;
        }
        size_t avail = x->in_fill - x->in_pos;   /* 0 only at end (src_left == 0) */
        size_t written = 0, consumed = 0;
        tamp_res r = tamp_decompressor_decompress(
            x->d, out + done, n - done, &written,
            x->in + x->in_pos, avail, &consumed);
        x->in_pos += consumed;
        done      += written;
        if (done == n)
            break;
        if (r == TAMP_INPUT_EXHAUSTED) {
            /* Needs more input. With slot bytes left we refill next loop; at true
             * end-of-stream the decompressor still drains its bit buffer with zero
             * input (avail==0), so only a zero-progress call there is a real tear. */
            if (x->src_left == 0 && avail == 0 && written == 0 && consumed == 0) {
                x->ok = false;       /* truncated stream */
                return false;
            }
            continue;
        }
        if (r != TAMP_OUTPUT_FULL) {
            x->ok = false;           /* TAMP_OOB or other decode error */
            return false;
        }
        /* TAMP_OUTPUT_FULL with done < n: keep delivering. */
    }
    return true;
}

/* Decompress-and-discard `n` bytes — the rare unknown/mismatched-section skip path
 * (can't seek a compressed stream). */
static bool decomp_skip(DecompCtx *x, uint32_t n) {
    unsigned char tmp[256];
    while (n) {
        uint32_t chunk = n < sizeof tmp ? n : (uint32_t)sizeof tmp;
        if (!decomp_read(x, tmp, chunk))
            return false;
        n -= chunk;
    }
    return true;
}

/* Validate a slot's CRC, then apply its sections to the live globals. Validation
 * happens BEFORE any write to live state, so a torn/corrupt slot is rejected with
 * the running game untouched. */
static bool read_slot(int slot) {
    uint32_t crc, plen;
    if (!read_slot_header(slot, NULL, &crc, &plen))
        return false;
    if (!verify_slot_crc(slot, plen, crc))
        return false;

    StateSection table[MAX_SECTIONS];
    int n = build_section_table(table);

    /* Decompress each tagged section straight into its live global. Unknown ids (or a
     * known id whose size doesn't match this build) are skipped — forward-compatibility,
     * and a guard against a layout mismatch smashing a struct. Same-binary loads never
     * hit the skip; the CRC pass already proved the structure is intact. */
    DecompCtx r;
    if (!decomp_init(&r, slot, plen))
        return false;
    for (;;) {
        uint16_t id;
        if (!decomp_read(&r, &id, 2))
            return false;
        if (id == SECTION_TERMINATOR)
            break;
        uint32_t size;
        if (!decomp_read(&r, &size, 4))
            return false;

        StateSection *sec = NULL;
        for (int i = 0; i < n; i++)
            if (table[i].id == id) { sec = &table[i]; break; }

        if (sec && sec->size == size) {
            if (size && !decomp_read(&r, sec->ptr, size))
                return false;
            /* pack/unpack section: scatter the scratch buffer back into its statics. */
            if (sec->unpack)
                sec->unpack(sec->ptr);
        } else {
            if (!decomp_skip(&r, size)) /* skip unknown / mismatched section */
                return false;
        }
    }

    /* Rebuild the raw pointers in the directly-serialized sections from their
     * serializable companions (offsets / ids) — savestate pointer purge, build
     * item #3. The pack/unpack sections rebuild their own pointers inside unpack(). */
    window_savestate_rebind();
    overworld_savestate_rebind();
    psi_animation_savestate_rebind();
    /* Rebuild the non-serialized map scratch (tileset arrangement buffer, etc.).
     * Without this a cold-boot resume scrolls within the stale VRAM nametable
     * (map "loops") because fill_tilemaps() has no arrangement data to stream. */
    map_loader_savestate_rebind();
    return true;
}

/* ---- Crash-safe ping-pong slots (build-order item #4 + #5) ---- */

bool state_dump_save_slots(void) {
    /* Cheap header-only peek of both slots (20 bytes each), then a SINGLE full CRC
     * verify of the newest-by-header slot — not both. A torn write leaves a valid
     * header carrying the NEW (highest) seq but a failing payload CRC, so the
     * freshly-torn slot, if any, is always the newest-by-header one. Verifying just
     * it tells us whether the newest slot committed (preserve it, write the other)
     * or is torn (overwrite it, preserving the older slot). Under the single-fault
     * model the non-verified slot is then valid, so the second full CRC pass the old
     * code ran over it was pure read overhead. */
    uint32_t s0 = 0, c0 = 0, p0 = 0, s1 = 0, c1 = 0, p1 = 0;
    bool h0 = read_slot_header(0, &s0, &c0, &p0);
    bool h1 = read_slot_header(1, &s1, &c1, &p1);

    int newest = -1;
    if (h0 && h1) newest = (s0 >= s1) ? 0 : 1;
    else if (h0)  newest = 0;
    else if (h1)  newest = 1;

    int target;
    uint32_t next;
    if (newest < 0) {
        target = 0;                 /* no valid header anywhere — start a fresh chain */
        next = 1;
    } else {
        uint32_t nseq  = newest ? s1 : s0;
        uint32_t ncrc  = newest ? c1 : c0;
        uint32_t nplen = newest ? p1 : p0;
        bool other_hdr = newest ? h0 : h1;
        uint32_t oseq  = newest ? s0 : s1;
        if (verify_slot_crc(newest, nplen, ncrc)) {
            target = newest ? 0 : 1;            /* preserve the committed newest slot */
            next = nseq + 1;
        } else {
            target = newest;                    /* newest-by-header is torn — overwrite it */
            next = (other_hdr ? oseq : 0) + 1;  /* preserving the older valid slot */
        }
    }
    bool ok = write_slot(target, next);
    return ok;
}

bool state_dump_load_slots(void) {
    /* Order candidates by a cheap header-only peek; read_slot() does its own
     * verify+apply, so we no longer CRC both slots up front. Newest-by-header is
     * tried first; a torn newest fails read_slot()'s CRC pass and we fall back to
     * the older slot. The old code CRC-verified BOTH slots here AND again inside
     * read_slot — the ~140 KiB snapshot was read ~4x; now it is read at most twice
     * (the chosen slot's verify pass + its apply pass). */
    uint32_t s0 = 0, s1 = 0;
    bool h0 = read_slot_header(0, &s0, NULL, NULL);
    bool h1 = read_slot_header(1, &s1, NULL, NULL);

    int first = -1, second = -1;
    if (h0 && h1) {
        if (s0 >= s1) { first = 0; second = 1; }
        else          { first = 1; second = 0; }
    } else if (h0) {
        first = 0;
    } else if (h1) {
        first = 1;
    }

    bool ok = false;
    if (first >= 0 && read_slot(first))        ok = true;
    else if (second >= 0 && read_slot(second)) ok = true;
    return ok;
}

/* ---- Desktop diagnostics (the `--selftest-savestate` flag) ---- */

/* Byte-compare two slots in full (header + payload), in chunks (no heap). */
static bool slots_byte_equal(int a, int b) {
    uint32_t plen_a, plen_b;
    if (!read_slot_header(a, NULL, NULL, &plen_a)) return false;
    if (!read_slot_header(b, NULL, NULL, &plen_b)) return false;
    if (plen_a != plen_b) return false;

    size_t total = STATE_DUMP_HEADER_SIZE + plen_a, off = 0;
    uint8_t ba[4096], bb[4096];
    while (off < total) {
        size_t chunk = (total - off) < sizeof ba ? (total - off) : sizeof ba;
        if (platform_savestate_read(a, off, ba, chunk) != chunk) return false;
        if (platform_savestate_read(b, off, bb, chunk) != chunk) return false;
        if (memcmp(ba, bb, chunk) != 0) return false;
        off += chunk;
    }
    return true;
}

/* save -> load -> save idempotency check on the CURRENT live state: writes the same
 * state to both slots with a FIXED sequence (the ping-pong layer would bump seq and
 * break the byte-compare), loads one back (idempotent), and confirms the two slot
 * images are byte-identical — proving the loader reads back exactly what the writer
 * wrote. Leaves the slot files populated (a diagnostic run exits afterward). */
bool state_dump_roundtrip_test(void) {
    const uint32_t fixed_seq = 1;
    if (!write_slot(0, fixed_seq)) return false;
    if (!read_slot(0))             return false;
    if (!write_slot(1, fixed_seq)) return false;
    return slots_byte_equal(0, 1);
}

/* Like the round-trip test, but scribbles every direct section's live storage with a
 * sentinel between the reference save and the load, so the load must reconstruct that
 * state purely from the file. If it does, the re-save byte-matches the reference; a
 * section that is written but never restored on load instead retains the sentinel and
 * the comparison fails. (The plain round-trip can't see that hole — there the
 * untouched globals reproduce the reference regardless.) Only the direct sections are
 * perturbed; the pack/unpack sections gather from file-private statics this test can't
 * reach, but read_slot() still scatters them back via unpack(). No game code runs
 * between the scribble and the load, so garbage globals (including pointer fields,
 * rebuilt by the rebind step) cannot misbehave. */
bool state_dump_perturb_test(void) {
    const uint32_t fixed_seq = 1;
    if (!write_slot(0, fixed_seq)) return false;   /* reference image */

    StateSection table[MAX_SECTIONS];
    int n = build_section_table(table);
    for (int i = 0; i < n; i++)
        if (!table[i].pack && table[i].size)       /* direct sections only */
            memset(table[i].ptr, 0xA5, table[i].size);

    if (!read_slot(0))             return false;   /* must restore from the file */
    if (!write_slot(1, fixed_seq)) return false;   /* re-save the restored state */
    return slots_byte_equal(0, 1);
}

/* Simulate a torn write: truncate `slot` to a few garbage bytes so it fails the
 * header/CRC check and reads as invalid. */
static void tear_slot(int slot) {
    if (!platform_savestate_begin(slot))
        return;
    const uint8_t junk[8] = { 0 };
    platform_savestate_write(slot, 0, junk, sizeof junk);
    platform_savestate_commit(slot);
}

/* Crash-safety self-test: two saves must land in different slots with increasing
 * sequence; tearing the newer slot must make a load fall back to the older one;
 * tearing both must make the load fail cleanly (never apply a torn slot). Loading
 * overwrites the live globals with the (identical) snapshot just written, so it is
 * harmless to run on the post-boot state. */
bool state_dump_crashsafe_test(void) {
    if (!state_dump_save_slots()) return false;
    if (!state_dump_save_slots()) return false;

    uint32_t s0 = 0, s1 = 0;
    if (!peek_slot(0, &s0) || !peek_slot(1, &s1) || s0 == s1)
        return false;

    int newer = (s0 > s1) ? 0 : 1;
    int older = (s0 > s1) ? 1 : 0;

    /* Tear the newer slot: it must read as invalid, and a load must fall back to the
     * older (still-valid) slot. */
    tear_slot(newer);
    {
        uint32_t dummy;
        if (peek_slot(newer, &dummy)) return false; /* tear must invalidate it */
    }
    if (!state_dump_load_slots()) return false;     /* must succeed via the older slot */

    /* Tear the older slot too: with no valid slot, the load must fail cleanly. */
    tear_slot(older);
    if (state_dump_load_slots()) return false;

    return true;
}

/* See state_dump.h. Each module owns the list of its own un-sectioned asset
 * caches (the perturb test can't reach them — they're in no section); add a
 * sibling check here when a new module joins the ensure_* pattern. */
bool state_dump_asset_pointer_test(void) {
    return file_select_asset_selfheal_test()
        && ending_asset_selfheal_test();
}
