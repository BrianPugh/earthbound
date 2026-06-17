#include "core/state_dump.h"

#ifdef EB_EMBEDDED

/* Embedded targets reach storage through the firmware's platform_savestate_* hooks;
 * the shared serialization core (the #else branch) is not yet wired into the embedded
 * build, so the slot entry points stub out for now. (Flipping the core on is the
 * remaining item-#5 step — it needs an arm-none-eabi build to verify.) */
bool state_dump_save_slots(void) {
    return false;
}

bool state_dump_load_slots(void) {
    return false;
}

bool state_dump_roundtrip_test(void) {
    return false;
}

bool state_dump_crashsafe_test(void) {
    return false;
}

#else

#include <stdint.h>
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

/* Container format (20-byte header, all fields little-endian — asserted below):
 *   magic       u32  "EBSD"
 *   version     u16
 *   frame       u16  informational (the authoritative value rides in SECTION_CORE)
 *   seq         u32  monotonic sequence — the ping-pong layer picks the highest valid
 *   crc32       u32  CRC-32 of the payload that follows the header
 *   payload_len u32  byte length of the payload (so the offset-addressed slot backend
 *                    knows how much to read without an EOF signal)
 * Then the payload: id(u16)/size(u32)/blob sections, then a 0xFFFF terminator. See
 * the AUDIT in docs/plans/savestate-unified-loop.md. */
#define STATE_DUMP_MAGIC       0x44534245u  /* "EBSD" little-endian */
#define STATE_DUMP_HEADER_SIZE 20           /* magic+version+frame+seq+crc32+payload_len */
#define STATE_DUMP_VERSION 9  /* v6: raw-pointer purge (item #3A) changed PSI/oval/
                               * overworld-deferred layouts. v7: ABI hardening (item
                               * #3B) — PPUState.bg_viewport_fill enum→uint8_t shrank
                               * the PPU section; the format is now 32/64-bit identical.
                               * v8: crash-safe persistence (item #4) — added seq +
                               * payload CRC-32 to the header (validate-on-load).
                               * v9: storage moved onto the platform_savestate_* slot
                               * backend (item #5) — added payload_len to the header. */

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

/* CRC-32 of the exact byte stream the section walk writes (id u16, size u32, data),
 * followed by the 0xFFFF terminator. Relies on the asserted little-endian target so
 * the in-memory bytes of id/size equal the bytes written to the slot. Run AFTER each
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

/* ---- Storage on the platform_savestate_* slot backend (build-order item #5) ----
 * All raw byte I/O goes through the port's offset/slot-addressed hooks (file-backed
 * on desktop, flash on embedded). The format/ping-pong/CRC logic lives here in core;
 * the port owns only the bytes + durability. Every multi-byte field is written/read
 * as its little-endian in-memory bytes (the target is asserted little-endian). */

/* Sequential slot writer: tracks a running offset and latches the first failure. */
typedef struct { int slot; size_t off; bool ok; } SsWriter;
static void ssw(SsWriter *w, const void *p, size_t n) {
    if (w->ok && !platform_savestate_write(w->slot, w->off, p, n))
        w->ok = false;
    w->off += n;
}
static void ssw_u16(SsWriter *w, uint16_t v) { ssw(w, &v, 2); }
static void ssw_u32(SsWriter *w, uint32_t v) { ssw(w, &v, 4); }

/* Serialize the live state into `slot`, tagged with sequence `seq`. The CRC and
 * payload length are computed in a pre-pass over the section table (no seek-back, no
 * staging of the whole snapshot). Returns true only on a fully committed write; a
 * partial/failed write is left uncommitted (it fails CRC validation later, so the
 * prior slot stays the newest valid one). */
static bool write_slot(int slot, uint32_t seq) {
    /* Gather the pack/unpack sections' scattered statics into their scratch buffers
     * BEFORE both the CRC pre-pass and the write, so the two see identical bytes. */
    StateSection table[MAX_SECTIONS];
    int n = build_section_table(table);
    for (int i = 0; i < n; i++)
        if (table[i].pack)
            table[i].pack(table[i].ptr);

    /* payload = sections (id u16 + size u32 + data) + terminator u16 */
    uint32_t payload_len = 2; /* terminator */
    for (int i = 0; i < n; i++)
        payload_len += 2 + 4 + table[i].size;
    uint32_t crc = compute_payload_crc(table, n);

    if (!platform_savestate_begin(slot))
        return false;

    SsWriter w = { slot, 0, true };
    ssw_u32(&w, STATE_DUMP_MAGIC);
    ssw_u16(&w, STATE_DUMP_VERSION);
    ssw_u16(&w, (uint16_t)core.frame_counter);
    ssw_u32(&w, seq);
    ssw_u32(&w, crc);
    ssw_u32(&w, payload_len);
    for (int i = 0; i < n; i++) {
        ssw_u16(&w, table[i].id);
        ssw_u32(&w, table[i].size);
        if (table[i].size)
            ssw(&w, table[i].ptr, table[i].size); /* straight from the live global */
    }
    ssw_u16(&w, SECTION_TERMINATOR);

    /* Always commit (closes/flushes the open writer); succeed only if every write
     * landed. A torn write produces an invalid slot, never a half-applied load. */
    bool committed = platform_savestate_commit(slot);
    return w.ok && committed;
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

/* Sequential slot reader: tracks a running offset and latches the first failure. */
typedef struct { int slot; size_t off; bool ok; } SsReader;
static bool ssr(SsReader *r, void *p, size_t n) {
    if (!r->ok)
        return false;
    if (platform_savestate_read(r->slot, r->off, p, n) != n) {
        r->ok = false;
        return false;
    }
    r->off += n;
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

    /* Read each tagged section straight into its live global. Unknown ids (or a known
     * id whose size doesn't match this build) are skipped — forward-compatibility, and
     * a guard against a layout mismatch smashing a struct. Same-binary loads never
     * hit the skip; the CRC pass already proved the structure is intact. */
    SsReader r = { slot, STATE_DUMP_HEADER_SIZE, true };
    for (;;) {
        uint16_t id;
        if (!ssr(&r, &id, 2))
            return false;
        if (id == SECTION_TERMINATOR)
            break;
        uint32_t size;
        if (!ssr(&r, &size, 4))
            return false;

        StateSection *sec = NULL;
        for (int i = 0; i < n; i++)
            if (table[i].id == id) { sec = &table[i]; break; }

        if (sec && sec->size == size) {
            if (size && !ssr(&r, sec->ptr, size))
                return false;
            /* pack/unpack section: scatter the scratch buffer back into its statics. */
            if (sec->unpack)
                sec->unpack(sec->ptr);
        } else {
            r.off += size; /* skip unknown / mismatched section */
        }
    }

    /* Rebuild the raw pointers in the directly-serialized sections from their
     * serializable companions (offsets / ids) — savestate pointer purge, build
     * item #3. The pack/unpack sections rebuild their own pointers inside unpack(). */
    window_savestate_rebind();
    overworld_savestate_rebind();
    psi_animation_savestate_rebind();
    return true;
}

/* ---- Crash-safe ping-pong slots (build-order item #4 + #5) ---- */

bool state_dump_save_slots(void) {
    uint32_t s0 = 0, s1 = 0;
    bool ok0 = peek_slot(0, &s0);
    bool ok1 = peek_slot(1, &s1);

    /* next sequence = max(valid sequences) + 1 (>= 1). */
    uint32_t maxseq = 0;
    bool any = false;
    if (ok0)                          { maxseq = s0; any = true; }
    if (ok1 && (!any || s1 > maxseq)) { maxseq = s1; any = true; }
    uint32_t next = any ? maxseq + 1 : 1;

    /* Write to the slot that is NOT the current newest-valid one, so that slot stays
     * intact until the new write commits (atomic via the sequence number). */
    bool slot0_is_newest = ok0 && (!ok1 || s0 >= s1);
    int target = slot0_is_newest ? 1 : 0;
    return write_slot(target, next);
}

bool state_dump_load_slots(void) {
    uint32_t s0 = 0, s1 = 0;
    bool ok0 = peek_slot(0, &s0);
    bool ok1 = peek_slot(1, &s1);

    /* Try the valid slots newest-first; fall back to the older one if the newest
     * fails to apply (it passed CRC at peek, so this is belt-and-suspenders). */
    int first = -1, second = -1;
    if (ok0 && ok1) {
        if (s0 >= s1) { first = 0; second = 1; }
        else          { first = 1; second = 0; }
    } else if (ok0) {
        first = 0;
    } else if (ok1) {
        first = 1;
    } else {
        return false;
    }

    if (first >= 0 && read_slot(first)) return true;
    if (second >= 0 && read_slot(second)) return true;
    return false;
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

#endif /* EB_EMBEDDED */
