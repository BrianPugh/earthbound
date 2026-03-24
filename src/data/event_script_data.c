/*
 * Event script data loader.
 *
 * Loads ROM-derived binary assets (extracted by ebtools) at runtime:
 *   - title_screen_scripts.bin: event script bytecode region (bank C4)
 *   - title_screen_script_pointers.bin: 12 × 3-byte ROM address entries
 *   - title_screen_spritemaps.bin: spritemap data + pointer table
 *   - event_script_pointers.bin: 895 × 3-byte ROM address entries
 *   - bank_c4_scripts.bin: naming screen bytecode (bank C4)
 *   - bank_c3_scripts.bin: naming screen bytecode (bank C3)
 *   - naming_screen_entities.bin: entity definition tables
 */
#include "data/event_script_data.h"
#include "data/assets.h"
#include "include/binary.h"
#include "core/log.h"
#include <stdio.h>
#include <string.h>

/* Runtime-loaded data */
const uint8_t *title_script_bank = NULL;
uint16_t title_script_bank_size = 0;
uint16_t title_script_bank_base = 0;
uint16_t title_script_pointers[TITLE_SCREEN_SCRIPT_COUNT];

const uint8_t *title_spritemap_data = NULL;
uint16_t title_spritemap_offsets[ANIMATION_FRAME_COUNT];

/* General event script pointer table */
const uint8_t *event_script_pointer_table = NULL;
uint16_t event_script_pointer_count = 0;

/* Script bank system */
ScriptBankInfo script_banks[MAX_SCRIPT_BANKS];
int script_bank_count = 0;

/* Naming screen entity data */
const uint8_t *naming_entities_data = NULL;
uint16_t naming_entities_data_size = 0;

/* Raw loaded buffers (compiled-in data, do NOT free) */
static const uint8_t *script_bank_buf = NULL;
static const uint8_t *script_ptrs_buf = NULL;
static const uint8_t *spritemap_buf = NULL;
static const uint8_t *event_ptr_table_buf = NULL;
static const uint8_t *bank_c4_scripts_buf = NULL;
static const uint8_t *naming_entities_buf = NULL;

/*
 * Script bank base address within bank $C4.
 * EVENT_BATTLE_FX (787) is at $C42172, so the within-bank base = $2172.
 */
#define TITLE_SCRIPT_BANK_ROM_BASE 0x2172

/* Bank C4 naming scripts start at $C40F18, within-bank = $0F18 */
#define C4_NAMING_SCRIPT_ROM_BASE 0x0E24


/*
 * Spritemap data layout:
 *   [0..404]: 9 spritemaps, 45 bytes each = 405 bytes
 *   [405..422]: 9-entry pointer table (2 bytes each = 18 bytes)
 */
#define SPRITEMAP_DATA_SIZE  405
#define SPRITEMAP_PTR_ENTRIES 9
#define SPRITEMAP_WITHIN_BANK_BASE 0xCE08

/* ---- Script Bank API ---- */

int register_script_bank(const uint8_t *data, uint32_t size,
                         uint16_t rom_base_addr, uint8_t rom_bank) {
    if (script_bank_count >= MAX_SCRIPT_BANKS)
        return -1;
    int idx = script_bank_count++;
    script_banks[idx].data = data;
    script_banks[idx].size = size;
    script_banks[idx].rom_base_addr = rom_base_addr;
    script_banks[idx].rom_bank = rom_bank;
    return idx;
}

uint8_t script_bank_read_byte(int bank_idx, uint16_t offset) {
    if (bank_idx < 0 || bank_idx >= script_bank_count)
        return OP_HALT;
    const ScriptBankInfo *b = &script_banks[bank_idx];
    if (!b->data || offset >= b->size)
        return OP_HALT;
    return b->data[offset];
}

uint16_t script_bank_read_word(int bank_idx, uint16_t offset) {
    return (uint16_t)script_bank_read_byte(bank_idx, offset) |
           ((uint16_t)script_bank_read_byte(bank_idx, (uint16_t)(offset + 1)) << 8);
}

uint32_t script_bank_read_addr(int bank_idx, uint16_t offset) {
    return (uint32_t)script_bank_read_byte(bank_idx, offset) |
           ((uint32_t)script_bank_read_byte(bank_idx, (uint16_t)(offset + 1)) << 8) |
           ((uint32_t)script_bank_read_byte(bank_idx, (uint16_t)(offset + 2)) << 16);
}

uint16_t script_bank_rom_to_offset(int bank_idx, uint16_t rom_addr) {
    if (bank_idx < 0 || bank_idx >= script_bank_count)
        return 0;
    return rom_addr - script_banks[bank_idx].rom_base_addr;
}

/*
 * Resolve a script ID to a bank index and buffer offset.
 *
 * 1. Look up the 3-byte ROM address in EVENT_SCRIPT_POINTERS[script_id]
 * 2. Extract the bank byte and within-bank address
 * 3. Find which loaded script bank covers this address
 * 4. Compute buffer offset = within_bank_addr - bank.rom_base_addr
 */
bool resolve_script_id(uint16_t script_id, int *out_bank_idx, uint16_t *out_offset) {
    if (!event_script_pointer_table || script_id >= event_script_pointer_count) {
        return false;
    }

    /* Read 3-byte PTR3 entry: [lo, hi, bank] */
    uint32_t entry_off = (uint32_t)script_id * 3;
    uint16_t within_bank = read_u16_le(&event_script_pointer_table[entry_off]);
    uint8_t rom_bank = event_script_pointer_table[entry_off + 2];

    /* Find a loaded bank that covers this address */
    for (int i = 0; i < script_bank_count; i++) {
        if (script_banks[i].rom_bank == rom_bank) {
            uint16_t base = script_banks[i].rom_base_addr;
            uint16_t end = base + (uint16_t)script_banks[i].size;
            if (within_bank >= base && within_bank < end) {
                *out_bank_idx = i;
                *out_offset = within_bank - base;
                return true;
            }
        }
    }

    return false;
}

/* ---- Title Screen Data ---- */

void load_title_screen_script_data(void) {
    /* Guard against double-loading */
    if (title_script_bank != NULL) return;

    size_t size;

    /* ---- Load script bytecode ---- */
    size = ASSET_SIZE(ASSET_INTRO_TITLE_SCREEN_SCRIPTS_BIN);
    script_bank_buf = ASSET_DATA(ASSET_INTRO_TITLE_SCREEN_SCRIPTS_BIN);
    if (script_bank_buf) {
        title_script_bank = script_bank_buf;
        title_script_bank_size = (uint16_t)size;
        title_script_bank_base = TITLE_SCRIPT_BANK_ROM_BASE;

        /* Register as script bank */
        register_script_bank(script_bank_buf, (uint32_t)size,
                             TITLE_SCRIPT_BANK_ROM_BASE, 0xC4);
    } else {
        fprintf(stderr, "ERROR: failed to load title_screen_scripts.bin\n");
        title_script_bank = NULL;
        title_script_bank_size = 0;
    }

    /* ---- Load and parse script pointer table ---- */
    size = ASSET_SIZE(ASSET_INTRO_TITLE_SCREEN_SCRIPT_POINTERS_BIN);
    script_ptrs_buf = ASSET_DATA(ASSET_INTRO_TITLE_SCREEN_SCRIPT_POINTERS_BIN);
    if (script_ptrs_buf && size >= TITLE_SCREEN_SCRIPT_COUNT * 3) {
        for (int i = 0; i < TITLE_SCREEN_SCRIPT_COUNT; i++) {
            uint16_t rom_lo = read_u16_le(&script_ptrs_buf[i * 3]);
            title_script_pointers[i] = rom_lo - title_script_bank_base;
        }
    } else {
        fprintf(stderr, "ERROR: failed to load title_screen_script_pointers.bin\n");
        memset(title_script_pointers, 0, sizeof(title_script_pointers));
    }

    /* ---- Load spritemap data ---- */
    size = ASSET_SIZE(ASSET_INTRO_TITLE_SCREEN_SPRITEMAPS_BIN);
    spritemap_buf = ASSET_DATA(ASSET_INTRO_TITLE_SCREEN_SPRITEMAPS_BIN);
    if (spritemap_buf && size >= SPRITEMAP_DATA_SIZE + SPRITEMAP_PTR_ENTRIES * 2) {
        title_spritemap_data = spritemap_buf;
        const uint8_t *ptrs = spritemap_buf + SPRITEMAP_DATA_SIZE;
        for (int i = 0; i < SPRITEMAP_PTR_ENTRIES; i++) {
            uint16_t rom_ptr = read_u16_le(&ptrs[i * 2]);
            title_spritemap_offsets[i] = rom_ptr - SPRITEMAP_WITHIN_BANK_BASE;
        }
    } else {
        fprintf(stderr, "ERROR: failed to load title_screen_spritemaps.bin\n");
        title_spritemap_data = NULL;
        memset(title_spritemap_offsets, 0, sizeof(title_spritemap_offsets));
    }
}

void free_title_screen_script_data(void) {
    script_bank_buf = NULL;
    title_script_bank = NULL;
    title_script_bank_size = 0;

    script_ptrs_buf = NULL;

    spritemap_buf = NULL;
    title_spritemap_data = NULL;
}

/* ---- General Event Script Data ---- */

void load_event_script_data(void) {
    /* Guard against double-loading (init_intro loads early,
     * file_select may also call this) */
    if (event_script_pointer_table != NULL) return;

    size_t size;

    /* Load event script pointer table (895 × 3 bytes) */
    size = ASSET_SIZE(ASSET_EVENTS_EVENT_SCRIPT_POINTERS_BIN);
    event_ptr_table_buf = ASSET_DATA(ASSET_EVENTS_EVENT_SCRIPT_POINTERS_BIN);
    if (event_ptr_table_buf && size >= EVENT_SCRIPT_POINTER_COUNT * 3) {
        event_script_pointer_table = event_ptr_table_buf;
        event_script_pointer_count = EVENT_SCRIPT_POINTER_COUNT;
    } else {
        fprintf(stderr, "ERROR: failed to load event_script_pointers.bin\n");
        event_script_pointer_table = NULL;
        event_script_pointer_count = 0;
    }

    /* Load bank C4 naming scripts (C40F18 onward) */
    size = ASSET_SIZE(ASSET_EVENTS_BANK_C4_SCRIPTS_BIN);
    bank_c4_scripts_buf = ASSET_DATA(ASSET_EVENTS_BANK_C4_SCRIPTS_BIN);
    if (bank_c4_scripts_buf) {
        register_script_bank(bank_c4_scripts_buf, (uint32_t)size,
                             C4_NAMING_SCRIPT_ROM_BASE, 0xC4);
    } else {
        LOG_WARN("WARN: failed to load bank_c4_scripts.bin\n");
    }

    /* Load bank C3 as a single pre-combined asset.
     * On real SNES hardware this is one contiguous ROM bank. The asset
     * is extracted as a single region so asset_load_locale() returns a
     * direct pointer — no RAM copy needed. SHORTCALLs between early
     * and late script regions work because it's one contiguous buffer. */
    {
        size_t c3_size = ASSET_SIZE(ASSET_EVENTS_BANK_C3_SCRIPTS_COMBINED_BIN);
        const uint8_t *c3 = ASSET_DATA(ASSET_EVENTS_BANK_C3_SCRIPTS_COMBINED_BIN);
        if (c3) {
            register_script_bank(c3, (uint32_t)c3_size, 0x0000, 0xC3);
        } else {
            LOG_WARN("WARN: failed to load bank_c3_scripts_combined.bin\n");
        }
    }

    /* Load naming screen entity table (14 DWORD pointers + entity lists) */
    size = ASSET_SIZE(ASSET_EVENTS_NAMING_SCREEN_ENTITIES_BIN);
    naming_entities_buf = ASSET_DATA(ASSET_EVENTS_NAMING_SCREEN_ENTITIES_BIN);
    if (naming_entities_buf) {
        naming_entities_data = naming_entities_buf;
        naming_entities_data_size = (uint16_t)size;
    } else {
        LOG_WARN("WARN: failed to load naming_screen_entities.bin\n");
        naming_entities_data = NULL;
        naming_entities_data_size = 0;
    }
}

void free_event_script_data(void) {
    event_ptr_table_buf = NULL;
    event_script_pointer_table = NULL;
    event_script_pointer_count = 0;

    bank_c4_scripts_buf = NULL;

    naming_entities_buf = NULL;
    naming_entities_data = NULL;
    naming_entities_data_size = 0;

    /* Reset script bank registry (title screen bank is freed separately) */
    script_bank_count = 0;
}
