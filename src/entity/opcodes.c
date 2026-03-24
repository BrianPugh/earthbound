/*
 * Movement opcode handlers for the event script bytecode interpreter.
 *
 * Each handler corresponds to a MOVEMENT_CODE_xx function in the ROM.
 * Handlers read arguments from the script bank, modify entity/script state,
 * and return the updated PC.
 *
 * The bytecode is loaded directly from the donor ROM, so addresses embedded
 * in the bytecode are original ROM addresses. Opcode handlers translate
 * these addresses at runtime:
 *   - SHORTCALL/SHORTJUMP/START_TASK: within-bank addresses → ert.buffer offsets
 *   - CALLROUTINE: 24-bit ROM addresses → dispatch by address
 *   - SET_DRAW_CALLBACK/SET_POS_CALLBACK: within-bank addresses → callback IDs
 *
 * Source: asm/overworld/actionscript/script/[00-43].asm
 */
#include "entity/entity.h"
#include "entity/script.h"
#include "data/event_script_data.h"
#include "game/overworld.h"
#include "game/fade.h"
#include "game/flyover.h"
#include "game/ending.h"
#include "include/binary.h"
#include "snes/ppu.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/*
 * Read helpers — use the generalized script bank system.
 * The current bank index is stored in scripts.pc_bank[script_offset].
 */
static int current_bank_idx;

/* Accumulator for EVENT_WRITE_DWORD_WRAM to POST_TELEPORT_CALLBACK.
 * The macro emits two WRITE_WORD_WRAM opcodes (lo then hi); we store
 * the low word here and resolve the full ROM address on the hi write. */
static uint16_t post_teleport_callback_wram_lo;

static inline uint8_t sb(uint16_t pc) {
    return script_bank_read_byte(current_bank_idx, pc);
}

static inline uint16_t sw(uint16_t pc) {
    return script_bank_read_word(current_bank_idx, pc);
}

static inline uint32_t s24(uint16_t pc) {
    return script_bank_read_addr(current_bank_idx, pc);
}

/*
 * Translate a within-bank ROM address to a ert.buffer offset.
 */
static inline uint16_t rom_to_offset(uint16_t rom_addr) {
    return script_bank_rom_to_offset(current_bank_idx, rom_addr);
}

/* Forward declaration for callroutine dispatch (callroutine.c) */
extern int16_t callroutine_dispatch(uint32_t rom_addr, int16_t entity_offset,
                                    int16_t script_offset, uint16_t pc,
                                    uint16_t *out_pc);

/* Forward declaration for FREE_ACTION_SCRIPT equivalent */
static void free_action_script(int16_t entity_offset, int16_t script_offset);

/*
 * Argument size table for ALL opcodes 0x00-0x44.
 *
 * Used by the default (unimplemented) handler to skip the correct number
 * of argument bytes, keeping the script PC aligned for subsequent opcodes.
 *
 * Special value 0xFF = variable-length (handled separately).
 *
 * Sizes verified against the assembly in asm/overworld/actionscript/script/
 * by counting INY instructions in each handler (and accounting for handlers
 * that replace Y via TXY for jump opcodes).
 */
/* Argument sizes used by the default handler to skip unimplemented opcodes */
static const uint8_t opcode_arg_sizes[0x45] = {
    /* 0x00 END */            0,
    /* 0x01 LOOP */           1,
    /* 0x02 LOOP_END */       0,
    /* 0x03 LONGJUMP */       3,  /* 2-byte addr + 1-byte bank; replaces PC */
    /* 0x04 LONGCALL */       3,  /* 2-byte addr + 1-byte bank; pushes ret */
    /* 0x05 LONGRETURN */     0,
    /* 0x06 PAUSE */          1,
    /* 0x07 START_TASK */     2,
    /* 0x08 SET_TICK_CB */    3,  /* 2-byte addr + 1-byte bank */
    /* 0x09 HALT */           0,
    /* 0x0A SHORTCALL_COND */ 2,
    /* 0x0B SHORTCALL_COND_NOT */ 2,
    /* 0x0C END_TASK */       0,
    /* 0x0D CALLBACK_DISP */  5,  /* 2+1+2 then JMP */
    /* 0x0E SET_VAR */        3,
    /* 0x0F CLEAR_TICK_CB */  0,
    /* 0x10 SWITCH */         0xFF, /* variable: 1 + count*2 */
    /* 0x11 SWITCH_CALL */    0xFF, /* variable: 1 + count*2 */
    /* 0x12 WRITE_BYTE_WRAM */3,
    /* 0x13 CHECK_NEXT */     0,
    /* 0x14 VAR_CB_DISP */    4,  /* 1 + falls into 0x0D_UNK1 (1+1+2) → 4 */
    /* 0x15 WRITE_WORD_WRAM */4,
    /* 0x16 COND_JUMP_POP */  2,
    /* 0x17 COND_JUMP_POP_NOT*/2,
    /* 0x18 CALLBACK_DISP2 */ 4,
    /* 0x19 SHORTJUMP */      2,
    /* 0x1A SHORTCALL */      2,
    /* 0x1B SHORT_RETURN */   0,
    /* 0x1C SET_ANIM_PTR */   3,
    /* 0x1D WRITE_WORD_TV */  2,
    /* 0x1E WRITE_WRAM_TV */  2,
    /* 0x1F SET_VAR_FROM_TV */1,
    /* 0x20 LOAD_VAR_TO_TV */ 1,
    /* 0x21 LOAD_VAR_SLEEP */ 1,
    /* 0x22 SET_DRAW_CB */    2,
    /* 0x23 SET_POS_CB */     2,
    /* 0x24 LOOP_FROM_TV */   0,
    /* 0x25 SET_MOVE_CB */    2,
    /* 0x26 SET_ANIM_VAR */   1,
    /* 0x27 TV_CB_DISPATCH */ 3,  /* falls into 0x0D_UNK2 (1+2) */
    /* 0x28 SET_X */          2,
    /* 0x29 SET_Y */          2,
    /* 0x2A SET_Z */          2,
    /* 0x2B ADD_X */          2,
    /* 0x2C ADD_Y */          2,
    /* 0x2D ADD_Z */          2,
    /* 0x2E ADD_DX */         2,
    /* 0x2F ADD_DY */         2,
    /* 0x30 ADD_DZ */         2,
    /* 0x31 SET_BG_H_OFF */   3,
    /* 0x32 SET_BG_V_OFF */   3,
    /* 0x33 SET_BG_H_VEL */   3,
    /* 0x34 SET_BG_V_VEL */   3,
    /* 0x35 ADD_BG_H_VEL */   3,
    /* 0x36 ADD_BG_V_VEL */   3,
    /* 0x37 ADD_BG_H_OFF */   3,
    /* 0x38 ADD_BG_V_OFF */   3,
    /* 0x39 SET_VEL_ZERO */   0,
    /* 0x3A CLEAR_BG_VEL */   1,
    /* 0x3B SET_ANIMATION */  1,
    /* 0x3C NEXT_ANIM */      0,
    /* 0x3D PREV_ANIM */      0,
    /* 0x3E SKIP_ANIM */      1,
    /* 0x3F SET_X_VEL */      2,
    /* 0x40 SET_Y_VEL */      2,
    /* 0x41 SET_Z_VEL */      2,
    /* 0x42 CALLROUTINE */    3,
    /* 0x43 SET_PRIORITY */   1,
    /* 0x44 SLEEP_FROM_TV */  0,
};

/*
 * Resolve a 16-bit WRAM address to a C pointer + field width.
 *
 * Entity scripts use opcodes 0x0D, 0x12, 0x15, 0x18, and 0x1E to perform
 * read/write/modify operations on arbitrary WRAM addresses.
 * This function maps those addresses to the corresponding C port fields.
 *
 * WRAM layout: entity tables are at fixed addresses (from ram.asm / earthbound.map),
 * each with MAX_ENTITIES * 2 = 60 bytes (30 entities × 2-byte words).
 * Script tables are MAX_SCRIPTS * 2 = 140 bytes.
 *
 * The bounds check uses the WRAM byte span (MAX_ENTITIES * 2), but the array
 * index divides by 2 since entity/script arrays are now [MAX_ENTITIES]/[MAX_SCRIPTS].
 *
 * The width field records the C-side element size (1 or 2 bytes). Opcodes use
 * wram_read/wram_write to access the field with correct zero-extension (for
 * reads of 8-bit fields) or truncation (for writes to 8-bit fields). This
 * keeps 8-bit fields packed in memory while matching the SNES's 16-bit
 * word-indexed WRAM access pattern.
 */
typedef struct {
    void *ptr;
    uint8_t width;  /* sizeof the C field element: 1 or 2 */
} WramField;

#define WRAM_FIELD_NULL ((WramField){NULL, 0})

/* Read a WRAM field as int16_t. 8-bit fields are zero-extended. */
static inline int16_t wram_read(WramField f) {
    if (f.width == 1) {
        /* For signed int8_t fields, reading as uint8_t then widening to
         * int16_t matches the SNES behavior: a 16-bit LDA from a byte
         * field reads the next byte (adjacent slot) as the high byte,
         * but scripts only care about the low byte. Zero-extending is
         * the safest conservative choice. */
        return (int16_t)*(uint8_t *)f.ptr;
    }
    return *(int16_t *)f.ptr;
}

/* Write an int16_t to a WRAM field. 8-bit fields are truncated. */
static inline void wram_write(WramField f, int16_t val) {
    if (f.width == 1)
        *(uint8_t *)f.ptr = (uint8_t)val;
    else
        *(int16_t *)f.ptr = val;
}

/*
 * Table macros — sizeof(field[0]) auto-detects the C-side element width,
 * so 8-bit fields stay packed at 1 byte while 16-bit fields use 2.
 */
#define WRAM_ENT_TABLE(base, field) \
    if (addr >= (base) && addr < (base) + MAX_ENTITIES * 2) \
        return (WramField){&entities.field[(addr - (base)) / 2], \
                           sizeof(entities.field[0])}

#define WRAM_SCR_TABLE(base, field) \
    if (addr >= (base) && addr < (base) + MAX_SCRIPTS * 2) \
        return (WramField){&scripts.field[(addr - (base)) / 2], \
                           sizeof(scripts.field[0])}

#define WRAM_BG_TABLE(base, arr) \
    if (addr >= (base) && addr < (base) + MAX_BG_LAYERS * 2) \
        return (WramField){&arr[(addr - (base)) / 2], sizeof(arr[0])}

#define WRAM_PATH_TABLE(base, arr) \
    if (addr >= (base) && addr < (base) + MAX_ENTITIES * 2) \
        return (WramField){&arr[(addr - (base)) / 2], sizeof(arr[0])}

/* Shorthand for 16-bit scalar globals */
#define WRAM_GLOBAL(address, var) \
    if (addr == (address)) return (WramField){&(var), 2}

static WramField resolve_wram(uint16_t addr) {
    /* Entity script/linkage tables (ram.asm order) */
    WRAM_ENT_TABLE(0x0A62, script_table);
    WRAM_ENT_TABLE(0x0A9E, next_entity);
    WRAM_ENT_TABLE(0x0ADA, script_index);
    WRAM_ENT_TABLE(0x0B16, screen_x);
    WRAM_ENT_TABLE(0x0B52, screen_y);
    WRAM_ENT_TABLE(0x0B8E, abs_x);
    WRAM_ENT_TABLE(0x0BCA, abs_y);
    WRAM_ENT_TABLE(0x0C06, abs_z);
    WRAM_ENT_TABLE(0x0C42, frac_x);
    WRAM_ENT_TABLE(0x0C7E, frac_y);
    WRAM_ENT_TABLE(0x0CBA, frac_z);
    WRAM_ENT_TABLE(0x0CF6, delta_x);
    WRAM_ENT_TABLE(0x0D32, delta_y);
    WRAM_ENT_TABLE(0x0D6E, delta_z);
    WRAM_ENT_TABLE(0x0DAA, delta_frac_x);
    WRAM_ENT_TABLE(0x0DE6, delta_frac_y);
    WRAM_ENT_TABLE(0x0E22, delta_frac_z);

    /* Entity script variables (8 tables) */
    for (int i = 0; i < 8; i++) {
        uint16_t base = 0x0E5E + i * 0x3C; /* each table is 60 bytes apart in WRAM */
        if (addr >= base && addr < base + MAX_ENTITIES * 2)
            return (WramField){&entities.var[i][(addr - base) / 2], 2};
    }

    /* Entity draw/callback tables */
    WRAM_ENT_TABLE(0x103E, draw_priority);
    WRAM_ENT_TABLE(0x107A, tick_callback_lo);
    WRAM_ENT_TABLE(0x10B6, tick_callback_hi);
    WRAM_ENT_TABLE(0x10F2, animation_frame);
    WRAM_ENT_TABLE(0x112E, spritemap_ptr_lo);
    WRAM_ENT_TABLE(0x116A, spritemap_ptr_hi);
    WRAM_ENT_TABLE(0x11A6, screen_pos_callback);
    WRAM_ENT_TABLE(0x11E2, draw_callback);
    WRAM_ENT_TABLE(0x121E, move_callback);

    /* Script tables */
    WRAM_SCR_TABLE(0x125A, next_script);
    WRAM_SCR_TABLE(0x12E6, stack_offset);
    WRAM_SCR_TABLE(0x1372, sleep_frames);
    WRAM_SCR_TABLE(0x13FE, pc);
    WRAM_SCR_TABLE(0x148A, pc_bank);
    WRAM_SCR_TABLE(0x1516, tempvar);

    /* BG scroll tables (4 layers × word-indexed) */
    WRAM_BG_TABLE(0x1A02, ert.entity_bg_h_offset_lo);
    WRAM_BG_TABLE(0x1A0A, ert.entity_bg_v_offset_lo);
    WRAM_BG_TABLE(0x1A12, ert.entity_bg_h_offset_hi);
    WRAM_BG_TABLE(0x1A1A, ert.entity_bg_v_offset_hi);
    WRAM_BG_TABLE(0x1A22, ert.entity_bg_h_velocity_lo);
    WRAM_BG_TABLE(0x1A2A, ert.entity_bg_v_velocity_lo);
    WRAM_BG_TABLE(0x1A32, ert.entity_bg_h_velocity_hi);
    WRAM_BG_TABLE(0x1A3A, ert.entity_bg_v_velocity_hi);

    /* Entity hitbox (first table sits between BG tables and moving_directions) */
    WRAM_ENT_TABLE(0x1A4A, hitbox_lr_heights);

    /* Entity movement/identity tables */
    WRAM_ENT_TABLE(0x1A86, moving_directions);
    WRAM_PATH_TABLE(0x284C, ert.entity_callback_flags_backup);
    WRAM_ENT_TABLE(0x289E, collided_objects);
    WRAM_ENT_TABLE(0x28DA, obstacle_flags);
    WRAM_ENT_TABLE(0x2AF6, directions);
    WRAM_ENT_TABLE(0x2B32, movement_speeds);
    WRAM_ENT_TABLE(0x2B6E, sizes);
    WRAM_ENT_TABLE(0x2BAA, surface_flags);
    WRAM_ENT_TABLE(0x2BE6, upper_lower_body_divides);
    WRAM_ENT_TABLE(0x2C22, walking_styles);
    WRAM_ENT_TABLE(0x2C5E, pathfinding_states);
    WRAM_ENT_TABLE(0x2C9A, npc_ids);
    WRAM_ENT_TABLE(0x2CD6, sprite_ids);
    WRAM_ENT_TABLE(0x2D12, enemy_ids);
    WRAM_ENT_TABLE(0x2D4E, enemy_spawn_tiles);

    /* Pathfinding tables (same layout as entity tables but separate arrays) */
    WRAM_PATH_TABLE(0x2E02, ert.entity_path_points);
    WRAM_PATH_TABLE(0x2E3E, ert.entity_path_point_counts);

    WRAM_ENT_TABLE(0x2E7A, overlay_flags);
    WRAM_ENT_TABLE(0x332A, hitbox_enabled);
    WRAM_ENT_TABLE(0x3366, hitbox_ud_widths);
    WRAM_ENT_TABLE(0x33A2, hitbox_ud_heights);
    WRAM_ENT_TABLE(0x33DE, hitbox_lr_widths);
    WRAM_ENT_TABLE(0x341A, current_displayed_sprites);
    WRAM_ENT_TABLE(0x3456, animation_fingerprints);

    /* Entity management globals (all 16-bit) */
    WRAM_GLOBAL(0x0A38, ert.new_entity_var[0]);
    WRAM_GLOBAL(0x0A3A, ert.new_entity_var[1]);
    WRAM_GLOBAL(0x0A3C, ert.new_entity_var[2]);
    WRAM_GLOBAL(0x0A3E, ert.new_entity_var[3]);
    WRAM_GLOBAL(0x0A40, ert.new_entity_var[4]);
    WRAM_GLOBAL(0x0A42, ert.new_entity_var[5]);
    WRAM_GLOBAL(0x0A44, ert.new_entity_var[6]);
    WRAM_GLOBAL(0x0A46, ert.new_entity_var[7]);
    WRAM_GLOBAL(0x0A48, ert.new_entity_pos_z);
    WRAM_GLOBAL(0x0A4A, ert.new_entity_priority);
    WRAM_GLOBAL(0x0A4C, entities.alloc_min_slot);
    WRAM_GLOBAL(0x0A4E, entities.alloc_max_slot);
    WRAM_GLOBAL(0x0A50, entities.first_entity);
    WRAM_GLOBAL(0x0A52, entities.last_entity);
    WRAM_GLOBAL(0x0A60, ert.disable_actionscript);

    /* Entity fade globals */
    WRAM_GLOBAL(0x195F, ow.entity_fade_entity);

    /* Overworld state globals */
    WRAM_GLOBAL(0x438A, ow.current_teleport_destination_x);
    WRAM_GLOBAL(0x438C, ow.current_teleport_destination_y);
    WRAM_GLOBAL(0x4A66, ow.show_npc_flag);
    WRAM_GLOBAL(0x4DBA, ow.enemy_has_been_touched);
    WRAM_GLOBAL(0x5D60, ow.battle_swirl_countdown);
    WRAM_GLOBAL(0x5D9A, ow.pending_interactions);
    WRAM_GLOBAL(0x9F3F, ow.psi_teleport_destination);

    /* Ending / cast scene globals */
    WRAM_GLOBAL(0xB4D1, cast_tile_offset);

    return WRAM_FIELD_NULL;
}

#undef WRAM_ENT_TABLE
#undef WRAM_SCR_TABLE
#undef WRAM_BG_TABLE
#undef WRAM_PATH_TABLE
#undef WRAM_GLOBAL

/*
 * Opcode dispatch — maps opcode number to handler.
 * Returns updated PC.
 */
uint16_t opcode_dispatch(uint8_t opcode, int16_t script_offset,
                         int16_t entity_offset, uint16_t pc) {
    /* Set current bank for read helpers */
    current_bank_idx = (int)scripts.pc_bank[script_offset];

    LOG_TRACE("TRACE: opcode=0x%02X pc=%u bank=%d ent=%d scr=%d\n",
              opcode, pc, current_bank_idx, entity_offset, script_offset);

    switch (opcode) {

    /* ---- 0x00: EVENT_END — deactivate entity ---- */
    case OP_END: {
        deactivate_entity(entity_offset);
        scripts.sleep_frames[script_offset] = -1;
        ert.actionscript_current_script = ENTITY_NONE;
        return pc;
    }

    /* ---- 0x01: EVENT_LOOP — push loop count + return address to stack ---- */
    case OP_LOOP: {
        uint8_t count = sb(pc);
        pc++;
        uint16_t stack_off = scripts.stack_offset[script_offset];
        if (stack_off + 3 > SCRIPT_STACK_SIZE)
            FATAL("OP_LOOP stack overflow (off=%u, scr=%d)\n", stack_off, script_offset);
        uint8_t *stack = scripts.stack[script_offset];
        stack[stack_off] = (uint8_t)(pc & 0xFF);
        stack[stack_off + 1] = (uint8_t)(pc >> 8);
        stack[stack_off + 2] = count;
        scripts.stack_offset[script_offset] = stack_off + 3;
        return pc;
    }

    /* ---- 0x02: EVENT_LOOP_END — decrement loop counter, jump back if > 0 ---- */
    case OP_LOOP_END: {
        uint16_t stack_off = scripts.stack_offset[script_offset];
        if (stack_off < 3)
            FATAL("OP_LOOP_END stack underflow (off=%u, scr=%d)\n", stack_off, script_offset);
        uint8_t *stack = scripts.stack[script_offset];
        uint8_t count = stack[stack_off - 1];
        count--;
        stack[stack_off - 1] = count;
        if (count != 0) {
            pc = read_u16_le(&stack[stack_off - 3]);
        } else {
            scripts.stack_offset[script_offset] = stack_off - 3;
        }
        return pc;
    }

    /* ---- 0x06: EVENT_PAUSE — set sleep frames ---- */
    case OP_PAUSE: {
        uint8_t frames = sb(pc);
        pc++;
        scripts.sleep_frames[script_offset] = frames;
        return pc;
    }

    /* ---- 0x07: EVENT_START_TASK — allocate new script, link to entity ---- */
    case OP_START_TASK: {
        uint16_t target_rom = sw(pc);
        pc += 2;

        uint16_t target_pc = rom_to_offset(target_rom);

        int16_t new_script;
        if (ert.last_allocated_script < 0)
            return pc;

        new_script = ert.last_allocated_script;
        ert.last_allocated_script = scripts.next_script[new_script];

        /* Assembly: STY ACTIONSCRIPT_CURRENT_SCRIPT (Y = new_script).
         * The iteration loop will process the newly spawned task next. */
        scripts.next_script[new_script] = scripts.next_script[script_offset];
        ert.actionscript_current_script = new_script;
        scripts.next_script[script_offset] = new_script;

        scripts.stack_offset[new_script] = 0;
        scripts.sleep_frames[new_script] = 0;
        scripts.pc[new_script] = target_pc;
        scripts.pc_bank[new_script] = scripts.pc_bank[script_offset];
        return pc;
    }

    /* ---- 0x08: SET_TICK_CALLBACK — set tick callback address ---- */
    case OP_SET_TICK_CALLBACK: {
        uint16_t lo = sw(pc);
        pc += 2;
        uint8_t hi = sb(pc);
        pc++;
        entities.tick_callback_lo[entity_offset] = lo;
        /* Only write the low byte (bank), preserving flags in high byte */
        entities.tick_callback_hi[entity_offset] =
            (entities.tick_callback_hi[entity_offset] & 0xFF00) | hi;
        return pc;
    }

    /* ---- 0x09: EVENT_HALT — infinite sleep ---- */
    case OP_HALT: {
        pc--;
        scripts.sleep_frames[script_offset] = -1;
        return pc;
    }

    /* ---- 0x0A: SHORTCALL_COND — if tempvar == 0, jump to target ---- */
    case OP_SHORTCALL_COND: {
        uint16_t target_rom = sw(pc);
        pc += 2;
        if (scripts.tempvar[script_offset] == 0) {
            pc = rom_to_offset(target_rom);
        }
        return pc;
    }

    /* ---- 0x0B: SHORTCALL_COND_NOT — if tempvar != 0, jump to target ---- */
    case OP_SHORTCALL_COND_NOT: {
        uint16_t target_rom = sw(pc);
        pc += 2;
        if (scripts.tempvar[script_offset] != 0) {
            pc = rom_to_offset(target_rom);
        }
        return pc;
    }

    /* ---- 0x0C: EVENT_END_TASK — terminate current script task ---- */
    case OP_END_TASK: {
        free_action_script(entity_offset, script_offset);
        scripts.sleep_frames[script_offset] = -1;
        if (entities.script_index[entity_offset] < 0) {
            deactivate_entity(entity_offset);
            scripts.sleep_frames[script_offset] = -1;
            ert.actionscript_current_script = ENTITY_NONE;
        }
        return pc;
    }

    /* ---- 0x0F: CLEAR_TICK_CALLBACK — set tick callback to NOP ---- */
    case OP_CLEAR_TICK_CALLBACK: {
        /* Assembly: sets callback to MOVEMENT_NOP (a no-op).
         * The 16-bit STA to tick_callback_hi clears bits 14-15 (frozen/disabled).
         * In C port we set to 0, effectively clearing the callback. */
        entities.tick_callback_lo[entity_offset] = 0;
        entities.tick_callback_hi[entity_offset] = 0;
        return pc;
    }

    /* ---- 0x27: TV_CB_DISPATCH — apply callback op to script tempvar ---- */
    /* Assembly: ENTITY_SCRIPT_TEMPVARS + script_offset → $8C, then
     * reads [callback_idx(1), param(2)] and dispatches AND/OR/ADD/XOR. */
    case 0x27: {
        uint8_t cb_idx = sb(pc);
        pc++;
        int16_t param = (int16_t)sw(pc);
        pc += 2;
        int16_t *target = &scripts.tempvar[script_offset];
        switch (cb_idx) {
            case 0: *target &= param; break;  /* AND */
            case 1: *target |= param; break;  /* OR */
            case 2: *target += param; break;  /* ADD */
            case 3: *target ^= param; break;  /* XOR */
            default: break;
        }
        return pc;
    }

    /* ---- 0x14: VAR_CB_DISP — apply callback op to entity variable ---- */
    /* Assembly: VAR_TABLE[var_idx] + entity_offset → $8C, then reads
     * [callback_idx(1), param(2)] and dispatches AND/OR/ADD/XOR. */
    case 0x14: {
        uint8_t var_idx = sb(pc);
        pc++;
        uint8_t cb_idx = sb(pc);
        pc++;
        int16_t param = (int16_t)sw(pc);
        pc += 2;
        if (var_idx < 8) {
            int16_t *target = &entities.var[var_idx][entity_offset];
            switch (cb_idx) {
                case 0: *target &= param; break;
                case 1: *target |= param; break;
                case 2: *target += param; break;
                case 3: *target ^= param; break;
                default: break;
            }
        }
        return pc;
    }

    /* ---- 0x0D: CALLBACK_DISP — 16-bit callback op on WRAM address ---- */
    /* Assembly: reads [base_addr(2), callback_idx(1), param(2)] from script.
     * Dispatches AND/OR/ADD/XOR via ENTITY_VAR_OP_TABLE. The WRAM address
     * is resolved to a C port field via resolve_wram(). */
    case 0x0D: {
        uint16_t addr = sw(pc);
        pc += 2;
        uint8_t cb_idx = sb(pc);
        pc++;
        int16_t param = (int16_t)sw(pc);
        pc += 2;
        WramField f = resolve_wram(addr);
        if (f.ptr) {
            int16_t val = wram_read(f);
            switch (cb_idx) {
                case 0: val &= param; break;  /* AND */
                case 1: val |= param; break;  /* OR */
                case 2: val += param; break;  /* ADD */
                case 3: val ^= param; break;  /* XOR */
                default: break;
            }
            wram_write(f, val);
        } else {
            LOG_WARN("WARN: CALLBACK_DISP unhandled addr=$%04X cb=%d param=$%04X "
                     "(ent=%d scr=%d)\n",
                     addr, cb_idx, (uint16_t)param, entity_offset, script_offset);
        }
        return pc;
    }

    /* ---- 0x18: CALLBACK_DISP2 — 8-bit callback op on WRAM address ---- */
    /* Assembly: reads [base_addr(2), callback_idx(1), param(1)] from script.
     * Uses 8-bit accumulator mode (SEP before callback dispatch).
     * The WRAM address targets a single byte. If the address is even, it
     * targets the low byte of the 16-bit word; if odd, the high byte. */
    case 0x18: {
        uint16_t addr = sw(pc);
        pc += 2;
        uint8_t cb_idx = sb(pc);
        pc++;
        uint8_t param = sb(pc);
        pc++;
        /* Resolve to the containing 16-bit WRAM word (even-aligned address) */
        WramField f = resolve_wram(addr & ~1);
        if (f.ptr) {
            int16_t word = wram_read(f);
            uint8_t byte_val;
            if (addr & 1) {
                byte_val = (uint8_t)((uint16_t)word >> 8);
            } else {
                byte_val = (uint8_t)((uint16_t)word & 0xFF);
            }
            switch (cb_idx) {
                case 0: byte_val &= param; break;
                case 1: byte_val |= param; break;
                case 2: byte_val += param; break;
                case 3: byte_val ^= param; break;
                default: break;
            }
            if (addr & 1) {
                wram_write(f, (int16_t)(((uint16_t)word & 0x00FF) | ((uint16_t)byte_val << 8)));
            } else {
                wram_write(f, (int16_t)(((uint16_t)word & 0xFF00) | byte_val));
            }
        } else {
            LOG_WARN("WARN: CALLBACK_DISP2 unhandled addr=$%04X cb=%d param=$%02X "
                     "(ent=%d scr=%d)\n",
                     addr, cb_idx, param, entity_offset, script_offset);
        }
        return pc;
    }

    /* ---- 0x03: LONGJUMP — far jump (offset + bank) ---- */
    case 0x03: {
        uint16_t target_addr = sw(pc);
        pc += 2;
        uint8_t bank = sb(pc);
        pc++;
        for (int i = 0; i < script_bank_count; i++) {
            if (script_banks[i].rom_bank == bank) {
                scripts.pc_bank[script_offset] = (uint8_t)i;
                return script_bank_rom_to_offset(i, target_addr);
            }
        }
        LOG_WARN("WARN: LONGJUMP to unknown bank $%02X\n", bank);
        return pc;
    }

    /* ---- 0x04: LONGCALL — far call (offset + bank), push return ---- */
    case 0x04: {
        uint16_t target_addr = sw(pc);
        pc += 2;
        uint8_t bank = sb(pc);
        pc++;
        /* Push return address: offset (2 bytes) + bank (1 byte) */
        uint16_t stack_off = scripts.stack_offset[script_offset];
        if (stack_off + 3 > SCRIPT_STACK_SIZE)
            FATAL("LONGCALL stack overflow (off=%u, scr=%d)\n", stack_off, script_offset);
        uint8_t *stack = scripts.stack[script_offset];
        stack[stack_off] = (uint8_t)(pc & 0xFF);
        stack[stack_off + 1] = (uint8_t)(pc >> 8);
        stack[stack_off + 2] = (uint8_t)scripts.pc_bank[script_offset];
        scripts.stack_offset[script_offset] = stack_off + 3;
        for (int i = 0; i < script_bank_count; i++) {
            if (script_banks[i].rom_bank == bank) {
                scripts.pc_bank[script_offset] = (uint8_t)i;
                return script_bank_rom_to_offset(i, target_addr);
            }
        }
        LOG_WARN("WARN: LONGCALL to unknown bank $%02X\n", bank);
        return pc;
    }

    /* ---- 0x05: LONGRETURN — pop far return address from stack ---- */
    case 0x05: {
        uint16_t stack_off = scripts.stack_offset[script_offset];
        if (stack_off == 0) {
            /* Empty stack — terminate script (same as END_TASK) */
            free_action_script(entity_offset, script_offset);
            scripts.sleep_frames[script_offset] = -1;
            if (entities.script_index[entity_offset] < 0) {
                deactivate_entity(entity_offset);
                ert.actionscript_current_script = ENTITY_NONE;
            }
            return pc;
        }
        /* Pop bank (1 byte) + offset (2 bytes) */
        stack_off--;
        uint8_t ret_bank = scripts.stack[script_offset][stack_off];
        stack_off -= 2;
        scripts.stack_offset[script_offset] = stack_off;
        scripts.pc_bank[script_offset] = ret_bank;
        pc = read_u16_le(&scripts.stack[script_offset][stack_off]);
        return pc;
    }

    /* ---- 0x10: SWITCH — jump table indexed by tempvar ---- */
    case 0x10: {
        int16_t tv = scripts.tempvar[script_offset];
        uint8_t count = sb(pc);
        pc++;
        if ((uint16_t)tv < count) {
            /* In-range: read address at index tempvar */
            uint16_t target_rom = sw(pc + (uint16_t)tv * 2);
            return rom_to_offset(target_rom);
        }
        /* Out-of-range: skip past all entries */
        return pc + count * 2;
    }

    /* ---- 0x11: SWITCH_CALL — call through jump table indexed by tempvar ---- */
    case 0x11: {
        int16_t tv = scripts.tempvar[script_offset];
        uint8_t count = sb(pc);
        pc++;
        if ((uint16_t)tv < count) {
            /* In-range: push return address (end of table), call target */
            uint16_t return_pc = pc + count * 2;
            uint16_t stack_off = scripts.stack_offset[script_offset];
            if (stack_off + 2 > SCRIPT_STACK_SIZE)
                FATAL("SWITCH_CALL stack overflow (off=%u, scr=%d)\n", stack_off, script_offset);
            uint8_t *stk = scripts.stack[script_offset];
            stk[stack_off] = (uint8_t)(return_pc & 0xFF);
            stk[stack_off + 1] = (uint8_t)(return_pc >> 8);
            scripts.stack_offset[script_offset] = stack_off + 2;
            uint16_t target_rom = sw(pc + (uint16_t)tv * 2);
            return rom_to_offset(target_rom);
        }
        /* Out-of-range: skip past all entries */
        return pc + count * 2;
    }

    /* ---- 0x12: WRITE_BYTE_WRAM — write 8-bit value to WRAM address ---- */
    case 0x12: {
        uint16_t addr = sw(pc);
        pc += 2;
        uint8_t val = sb(pc);
        pc++;
        /* Map known WRAM addresses to C variables */
        LOG_TRACE("  WRITE_BYTE_WRAM addr=$%04X val=$%02X\n", addr, val);
        /* Map known WRAM addresses to C port variables.
         * The original game's NMI handler copies these mirrors to hardware.
         * Addresses from earthbound.map (7E00xx low byte). */
        switch (addr) {
        /* PPU mirror registers (ram.asm offsets from $7E0000) */
        case 0x000D: ppu.inidisp = val; break;   /* INIDISP_MIRROR */
        case 0x000E: ppu.obsel = val; break;      /* OBSEL_MIRROR */
        case 0x000F: ppu.bgmode = val; break;     /* BGMODE_MIRROR */
        case 0x0010: ppu.mosaic = val; break;     /* MOSAIC_MIRROR */
        case 0x0011: ppu.bg_sc[0] = val; break;   /* BG1SC_MIRROR */
        case 0x0012: ppu.bg_sc[1] = val; break;   /* BG2SC_MIRROR */
        case 0x0013: ppu.bg_sc[2] = val; break;   /* BG3SC_MIRROR */
        case 0x0014: ppu.bg_sc[3] = val; break;   /* BG4SC_MIRROR */
        case 0x0015: ppu.bg_nba[0] = val; break;  /* BG12NBA_MIRROR */
        case 0x0016: ppu.bg_nba[1] = val; break;  /* BG34NBA_MIRROR */
        case 0x0017: ppu.wh2 = val; break;        /* WH2_MIRROR */
        case 0x001A: ppu.tm = val; break;          /* TM_MIRROR */
        case 0x001B: ppu.ts = val; break;          /* TD_MIRROR (sub-screen) */
        default: {
            /* Try entity/script table mapping */
            WramField f = resolve_wram(addr & ~1);
            if (f.ptr) {
                int16_t word = wram_read(f);
                if (addr & 1) {
                    wram_write(f, (int16_t)(((uint16_t)word & 0x00FF) | ((uint16_t)val << 8)));
                } else {
                    wram_write(f, (int16_t)(((uint16_t)word & 0xFF00) | val));
                }
            } else {
                LOG_WARN("WRITE_BYTE_WRAM: unhandled addr=$%04X val=$%02X\n", addr, val);
            }
            break;
        }
        }
        return pc;
    }

    /* ---- 0x13: CHECK_NEXT — if next script exists, free it ---- */
    case 0x13: {
        /* Assembly (13.asm): checks ENTITY_SCRIPT_NEXT_SCRIPTS[script_offset].
         * If >= 0 (a next script is linked after the current one), frees that
         * NEXT script via MOVEMENT_CODE_0C_UNK1 (0C.asm:5-15).
         * If no more scripts remain on entity, deactivates entity.
         * Otherwise the current script continues normally. */
        int8_t next = scripts.next_script[script_offset];
        if (next >= 0) {
            free_action_script(entity_offset, next);
            scripts.sleep_frames[next] = -1;
            if (entities.script_index[entity_offset] < 0) {
                deactivate_entity(entity_offset);
                scripts.sleep_frames[script_offset] = -1;
                ert.actionscript_current_script = ENTITY_NONE;
            }
        }
        return pc;
    }

    /* ---- 0x16: COND_JUMP_POP — if tempvar == 0, jump and pop 3 from stack ---- */
    case 0x16: {
        uint16_t target_rom = sw(pc);
        pc += 2;
        if (scripts.tempvar[script_offset] == 0) {
            if (scripts.stack_offset[script_offset] < 3)
                FATAL("COND_JUMP_POP stack underflow (off=%u, scr=%d)\n",
                      scripts.stack_offset[script_offset], script_offset);
            scripts.stack_offset[script_offset] -= 3;
            return rom_to_offset(target_rom);
        }
        return pc;
    }

    /* ---- 0x17: COND_JUMP_POP_NOT — if tempvar != 0, jump and pop 3 ---- */
    case 0x17: {
        uint16_t target_rom = sw(pc);
        pc += 2;
        if (scripts.tempvar[script_offset] != 0) {
            if (scripts.stack_offset[script_offset] < 3)
                FATAL("COND_JUMP_POP_NOT stack underflow (off=%u, scr=%d)\n",
                      scripts.stack_offset[script_offset], script_offset);
            scripts.stack_offset[script_offset] -= 3;
            return rom_to_offset(target_rom);
        }
        return pc;
    }

    /* ---- 0x0E: SET_VAR — write word to entity variable table ---- */
    case OP_SET_VAR: {
        uint8_t var_idx = sb(pc);
        pc++;
        int16_t value = (int16_t)sw(pc);
        pc += 2;
        if (var_idx < 8) {
            entities.var[var_idx][entity_offset] = value;
        }
        return pc;
    }

    /* ---- 0x15: WRITE_WORD_WRAM — write 16-bit value to WRAM address ---- */
    case OP_WRITE_WORD_WRAM: {
        uint16_t addr = sw(pc);
        pc += 2;
        int16_t value = (int16_t)sw(pc);
        pc += 2;
        if (addr == WRAM_WAIT_FOR_NAMING_SCREEN) {
            ert.wait_for_naming_screen_actionscript = (uint16_t)value;
        } else if (addr == WRAM_TITLE_SCREEN_QUICK_MODE) {
            ert.title_screen_quick_mode = (uint16_t)value;
        } else if (addr == WRAM_POST_TELEPORT_CALLBACK_LO) {
            post_teleport_callback_wram_lo = (uint16_t)value;
        } else if (addr == WRAM_POST_TELEPORT_CALLBACK_HI) {
            uint32_t rom_addr = post_teleport_callback_wram_lo
                              | ((uint32_t)(uint16_t)value << 16);
            if (rom_addr == ROM_ADDR_UNDRAW_FLYOVER_TEXT) {
                ow.post_teleport_callback = undraw_flyover_text;
            } else if (rom_addr == 0) {
                ow.post_teleport_callback = NULL;
            } else {
                LOG_WARN("WARN: POST_TELEPORT_CALLBACK unknown ROM addr $%06X\n",
                         rom_addr);
            }
        } else {
            WramField f = resolve_wram(addr);
            if (f.ptr) {
                wram_write(f, value);
            } else {
                LOG_WARN("WARN: WRITE_WORD_WRAM to unhandled addr $%04X = %d\n",
                         addr, value);
            }
        }
        return pc;
    }

    /* ---- 0x19: SHORTJUMP — unconditional jump within bank ---- */
    case OP_SHORTJUMP: {
        uint16_t target_rom = sw(pc);
        return rom_to_offset(target_rom);
    }

    /* ---- 0x1A: SHORTCALL — push return address, jump to target ---- */
    case OP_SHORTCALL: {
        uint16_t target_rom = sw(pc);
        pc += 2;
        uint16_t stack_off = scripts.stack_offset[script_offset];
        if (stack_off + 2 > SCRIPT_STACK_SIZE)
            FATAL("SHORTCALL stack overflow (off=%u, scr=%d)\n", stack_off, script_offset);
        uint8_t *stack = scripts.stack[script_offset];
        stack[stack_off] = (uint8_t)(pc & 0xFF);
        stack[stack_off + 1] = (uint8_t)(pc >> 8);
        scripts.stack_offset[script_offset] = stack_off + 2;
        return rom_to_offset(target_rom);
    }

    /* ---- 0x1B: SHORT_RETURN — pop return address from stack ---- */
    case OP_SHORT_RETURN: {
        uint16_t stack_off = scripts.stack_offset[script_offset];
        if (stack_off == 0) {
            free_action_script(entity_offset, script_offset);
            scripts.sleep_frames[script_offset] = -1;
            if (entities.script_index[entity_offset] < 0) {
                deactivate_entity(entity_offset);
                ert.actionscript_current_script = ENTITY_NONE;
            }
            return pc;
        }
        stack_off -= 2;
        scripts.stack_offset[script_offset] = stack_off;
        uint8_t *stack = scripts.stack[script_offset];
        pc = read_u16_le(&stack[stack_off]);
        return pc;
    }

    /* ---- 0x1C: SET_ANIM_PTR — set animation table pointer ---- */
    case OP_SET_ANIM_PTR: {
        entities.spritemap_ptr_lo[entity_offset] = sw(pc);
        pc += 2;
        uint8_t bank = sb(pc);
        pc++;
        /* Assembly uses SEP #ACCUM8 then 8-bit STA, preserving the full
         * high byte (bits 8-15). Mask is 0xFF00, not just bit 15. */
        entities.spritemap_ptr_hi[entity_offset] =
            (entities.spritemap_ptr_hi[entity_offset] & 0xFF00) | bank;
        return pc;
    }

    /* ---- 0x1D: WRITE_WORD_TEMPVAR — tempvar = literal word ---- */
    case OP_WRITE_WORD_TEMPVAR: {
        int16_t value = (int16_t)sw(pc);
        pc += 2;
        scripts.tempvar[script_offset] = value;
        return pc;
    }

    /* ---- 0x1E: WRITE_WRAM_TEMPVAR — tempvar = mem[addr] ---- */
    case OP_WRITE_WRAM_TEMPVAR: {
        uint16_t addr = sw(pc);
        pc += 2;
        if (addr == WRAM_FADE_STEP) {
            /* FADE_PARAMETERS::step ($7E0028) — non-zero while a palette fade is in
             * progress, 0 when idle.  WAIT_UNTIL_ENTITY_STOPPED (C3ABE0) polls this
             * to hold event scripts until the fade finishes. */
            scripts.tempvar[script_offset] = (int16_t)fade_state.step;
        } else if (addr == WRAM_TITLE_SCREEN_QUICK_MODE) {
            scripts.tempvar[script_offset] = (int16_t)ert.title_screen_quick_mode;
        } else if (addr == WRAM_WAIT_FOR_NAMING_SCREEN) {
            scripts.tempvar[script_offset] =
                (int16_t)ert.wait_for_naming_screen_actionscript;
        } else {
            WramField f = resolve_wram(addr);
            if (f.ptr) {
                scripts.tempvar[script_offset] = wram_read(f);
            } else {
                LOG_WARN("WARN: WRITE_WRAM_TEMPVAR unhandled addr $%04X\n", addr);
                scripts.tempvar[script_offset] = 0;
            }
        }
        return pc;
    }

    /* ---- 0x1F: SET_VAR_FROM_TEMPVAR — var[idx] = tempvar ---- */
    case 0x1F: {
        uint8_t var_idx = sb(pc);
        pc++;
        if (var_idx < 8) {
            entities.var[var_idx][entity_offset] = scripts.tempvar[script_offset];
        }
        return pc;
    }

    /* ---- 0x20: LOAD_VAR_TO_TEMPVAR — tempvar = var[idx] ---- */
    case 0x20: {
        uint8_t var_idx = sb(pc);
        pc++;
        if (var_idx < 8) {
            scripts.tempvar[script_offset] = entities.var[var_idx][entity_offset];
        }
        return pc;
    }

    /* ---- 0x21: LOAD_VAR_SLEEP — sleep_frames = var[idx] ---- */
    case 0x21: {
        uint8_t var_idx = sb(pc);
        pc++;
        if (var_idx < 8) {
            scripts.sleep_frames[script_offset] = entities.var[var_idx][entity_offset];
        }
        return pc;
    }

    /* ---- 0x22: SET_DRAW_CALLBACK ---- */
    case OP_SET_DRAW_CALLBACK: {
        uint16_t rom_addr = sw(pc);
        pc += 2;
        if (rom_addr == ROM_ADDR_DRAW_TITLE_LETTER) {
            entities.draw_callback[entity_offset] = CB_DRAW_TITLE_LETTER;
        } else {
            entities.draw_callback[entity_offset] = CB_DRAW_ENTITY_SPRITE;
        }
        return pc;
    }

    /* ---- 0x23: SET_POS_CALLBACK ---- */
    case OP_SET_POS_CALLBACK: {
        uint16_t rom_addr = sw(pc);
        pc += 2;
        if (rom_addr == ROM_ADDR_POS_COPY_ABS) {
            entities.screen_pos_callback[entity_offset] = CB_POS_COPY_ABS;
        } else if (rom_addr == ROM_ADDR_POS_NOP) {
            entities.screen_pos_callback[entity_offset] = CB_POS_NOP;
        } else if (rom_addr == ROM_ADDR_POS_SCREEN_BG3) {
            entities.screen_pos_callback[entity_offset] = CB_POS_SCREEN_BG3;
        } else if (rom_addr == ROM_ADDR_POS_BG1_WITH_Z) {
            entities.screen_pos_callback[entity_offset] = CB_POS_SCREEN_BG1_Z;
        } else if (rom_addr == ROM_ADDR_POS_BG3_WITH_Z) {
            entities.screen_pos_callback[entity_offset] = CB_POS_SCREEN_BG3_Z;
        } else if (rom_addr == ROM_ADDR_MOVE_FORCE_MOVE) {
            entities.screen_pos_callback[entity_offset] = CB_POS_FORCE_MOVE;
        } else {
            entities.screen_pos_callback[entity_offset] = CB_POS_SCREEN_BG1;
        }
        return pc;
    }

    /* ---- 0x25: SET_MOVE_CALLBACK ---- */
    case OP_SET_MOVE_CALLBACK: {
        uint16_t rom_addr = sw(pc);
        pc += 2;
        if (rom_addr == ROM_ADDR_MOVE_FORCE_MOVE) {
            entities.move_callback[entity_offset] = CB_MOVE_FORCE_MOVE;
        } else if (rom_addr == ROM_ADDR_MOVE_PARTY_SPRITE) {
            entities.move_callback[entity_offset] = CB_MOVE_PARTY_SPRITE;
        } else if (rom_addr == ROM_ADDR_MOVE_WITH_COLLISION) {
            entities.move_callback[entity_offset] = CB_MOVE_WITH_COLLISION;
        } else if (rom_addr == ROM_ADDR_MOVE_SIMPLE_COLLISION) {
            entities.move_callback[entity_offset] = CB_MOVE_SIMPLE_COLLISION;
        } else if (rom_addr == ROM_ADDR_MOVE_DELTA_3D || rom_addr == ROM_ADDR_MOVE_Z_UPDATE) {
            entities.move_callback[entity_offset] = CB_MOVE_DELTA_3D;
        } else if (rom_addr == ROM_ADDR_MOVE_NOP) {
            entities.move_callback[entity_offset] = CB_MOVE_NOP;
        } else {
            entities.move_callback[entity_offset] = CB_MOVE_APPLY_DELTA;
        }
        return pc;
    }

    /* ---- 0x24: LOOP_FROM_TV — loop with count from tempvar ---- */
    case 0x24: {
        uint8_t count = (uint8_t)scripts.tempvar[script_offset];
        uint16_t stack_off = scripts.stack_offset[script_offset];
        if (stack_off + 3 > SCRIPT_STACK_SIZE)
            FATAL("LOOP_FROM_TV stack overflow (off=%u, scr=%d)\n", stack_off, script_offset);
        uint8_t *stk = scripts.stack[script_offset];
        stk[stack_off] = (uint8_t)(pc & 0xFF);
        stk[stack_off + 1] = (uint8_t)(pc >> 8);
        stk[stack_off + 2] = count;
        scripts.stack_offset[script_offset] = stack_off + 3;
        return pc;
    }

    /* ---- 0x26: SET_ANIM_VAR — animation_frame = var[idx] ---- */
    case 0x26: {
        uint8_t var_idx = sb(pc);
        pc++;
        if (var_idx < 8) {
            entities.animation_frame[entity_offset] = entities.var[var_idx][entity_offset];
        }
        return pc;
    }

    /* ---- 0x28: SET_X — set absolute X position ---- */
    case OP_SET_X: {
        int16_t x = (int16_t)sw(pc);
        pc += 2;
        entities.abs_x[entity_offset] = x;
        entities.frac_x[entity_offset] = 0x8000;
        return pc;
    }

    /* ---- 0x29: SET_Y — set absolute Y position ---- */
    case OP_SET_Y: {
        int16_t y = (int16_t)sw(pc);
        pc += 2;
        entities.abs_y[entity_offset] = y;
        entities.frac_y[entity_offset] = 0x8000;
        return pc;
    }

    /* ---- 0x2A: SET_Z — set absolute Z position ---- */
    case OP_SET_Z: {
        int16_t z = (int16_t)sw(pc);
        pc += 2;
        entities.abs_z[entity_offset] = z;
        entities.frac_z[entity_offset] = 0x8000;
        return pc;
    }

    /* ---- 0x2B: ADD_X — add signed 16-bit offset to abs_x ---- */
    case 0x2B: {
        int16_t val = (int16_t)sw(pc);
        pc += 2;
        entities.abs_x[entity_offset] += val;
        return pc;
    }

    /* ---- 0x2C: ADD_Y — add signed 16-bit offset to abs_y ---- */
    case 0x2C: {
        int16_t val = (int16_t)sw(pc);
        pc += 2;
        entities.abs_y[entity_offset] += val;
        return pc;
    }

    /* ---- 0x2D: ADD_Z — add signed 16-bit offset to abs_z ---- */
    case 0x2D: {
        int16_t val = (int16_t)sw(pc);
        pc += 2;
        entities.abs_z[entity_offset] += val;
        return pc;
    }

    /* ---- 0x2E: ADD_DX — add fixed-point velocity to delta_x ---- */
    case 0x2E: {
        uint16_t raw = sw(pc);
        pc += 2;
        uint16_t add_frac = ((uint16_t)(raw & 0xFF)) << 8;
        int16_t add_int = (int16_t)(int8_t)((raw >> 8) & 0xFF);
        uint16_t old_frac = entities.delta_frac_x[entity_offset];
        uint32_t frac_sum = (uint32_t)old_frac + (uint32_t)add_frac;
        entities.delta_frac_x[entity_offset] = (uint16_t)frac_sum;
        entities.delta_x[entity_offset] += add_int + (int16_t)(frac_sum >> 16);
        return pc;
    }

    /* ---- 0x2F: ADD_DY — add fixed-point velocity to delta_y ---- */
    case 0x2F: {
        uint16_t raw = sw(pc);
        pc += 2;
        uint16_t add_frac = ((uint16_t)(raw & 0xFF)) << 8;
        int16_t add_int = (int16_t)(int8_t)((raw >> 8) & 0xFF);
        uint16_t old_frac = entities.delta_frac_y[entity_offset];
        uint32_t frac_sum = (uint32_t)old_frac + (uint32_t)add_frac;
        entities.delta_frac_y[entity_offset] = (uint16_t)frac_sum;
        entities.delta_y[entity_offset] += add_int + (int16_t)(frac_sum >> 16);
        return pc;
    }

    /* ---- 0x30: ADD_DZ — add fixed-point velocity to delta_z ---- */
    case 0x30: {
        uint16_t raw = sw(pc);
        pc += 2;
        uint16_t add_frac = ((uint16_t)(raw & 0xFF)) << 8;
        int16_t add_int = (int16_t)(int8_t)((raw >> 8) & 0xFF);
        uint16_t old_frac = entities.delta_frac_z[entity_offset];
        uint32_t frac_sum = (uint32_t)old_frac + (uint32_t)add_frac;
        entities.delta_frac_z[entity_offset] = (uint16_t)frac_sum;
        entities.delta_z[entity_offset] += add_int + (int16_t)(frac_sum >> 16);
        return pc;
    }

    /* ---- 0x39: SET_VEL_ZERO — zero all velocity components ---- */
    case OP_SET_VEL_ZERO: {
        entities.delta_frac_x[entity_offset] = 0;
        entities.delta_x[entity_offset] = 0;
        entities.delta_frac_y[entity_offset] = 0;
        entities.delta_y[entity_offset] = 0;
        entities.delta_frac_z[entity_offset] = 0;
        entities.delta_z[entity_offset] = 0;
        return pc;
    }

    /* ---- 0x31: BG_SET_H_OFFSET — set BG horizontal scroll offset ---- */
    case 0x31: {
        uint8_t bg_idx = sb(pc);
        pc++;
        int16_t val = (int16_t)sw(pc);
        pc += 2;
        uint8_t idx = bg_idx;
        if (idx < MAX_BG_LAYERS) {
            ert.entity_bg_h_offset_lo[idx] = val;
            ert.entity_bg_h_offset_hi[idx] = 0;
        }
        return pc;
    }

    /* ---- 0x32: BG_SET_V_OFFSET — set BG vertical scroll offset ---- */
    case 0x32: {
        uint8_t bg_idx = sb(pc);
        pc++;
        int16_t val = (int16_t)sw(pc);
        pc += 2;
        uint8_t idx = bg_idx;
        if (idx < MAX_BG_LAYERS) {
            ert.entity_bg_v_offset_lo[idx] = val;
            ert.entity_bg_v_offset_hi[idx] = 0;
        }
        return pc;
    }

    /* ---- 0x33: BG_SET_H_VEL — set BG horizontal scroll velocity ---- */
    case 0x33: {
        uint8_t bg_idx = sb(pc);
        pc++;
        uint16_t raw = sw(pc);
        pc += 2;
        uint8_t idx = bg_idx;
        if (idx < MAX_BG_LAYERS) {
            /* Low byte → fraction (shifted to high byte), high byte → integer (sign-extended) */
            ert.entity_bg_h_velocity_hi[idx] = (int16_t)(uint16_t)(((uint16_t)(raw & 0xFF)) << 8);
            int16_t int_part = (int16_t)(int8_t)((raw >> 8) & 0xFF);
            ert.entity_bg_h_velocity_lo[idx] = int_part;
        }
        return pc;
    }

    /* ---- 0x34: BG_SET_V_VEL — set BG vertical scroll velocity ---- */
    case 0x34: {
        uint8_t bg_idx = sb(pc);
        pc++;
        uint16_t raw = sw(pc);
        pc += 2;
        uint8_t idx = bg_idx;
        if (idx < MAX_BG_LAYERS) {
            ert.entity_bg_v_velocity_hi[idx] = (int16_t)(uint16_t)(((uint16_t)(raw & 0xFF)) << 8);
            int16_t int_part = (int16_t)(int8_t)((raw >> 8) & 0xFF);
            ert.entity_bg_v_velocity_lo[idx] = int_part;
        }
        return pc;
    }

    /* ---- 0x35: BG_ADD_H_VEL — add to BG horizontal scroll velocity ---- */
    case 0x35: {
        uint8_t bg_idx = sb(pc);
        pc++;
        uint16_t raw = sw(pc);
        pc += 2;
        uint8_t idx = bg_idx;
        if (idx < MAX_BG_LAYERS) {
            uint16_t add_frac = ((uint16_t)(raw & 0xFF)) << 8;
            int16_t add_int = (int16_t)(int8_t)((raw >> 8) & 0xFF);
            uint32_t frac_sum = (uint32_t)(uint16_t)ert.entity_bg_h_velocity_hi[idx] + (uint32_t)add_frac;
            ert.entity_bg_h_velocity_hi[idx] = (int16_t)(uint16_t)frac_sum;
            ert.entity_bg_h_velocity_lo[idx] += add_int + (int16_t)(frac_sum >> 16);
        }
        return pc;
    }

    /* ---- 0x36: BG_ADD_V_VEL — add to BG vertical scroll velocity ---- */
    case 0x36: {
        uint8_t bg_idx = sb(pc);
        pc++;
        uint16_t raw = sw(pc);
        pc += 2;
        uint8_t idx = bg_idx;
        if (idx < MAX_BG_LAYERS) {
            uint16_t add_frac = ((uint16_t)(raw & 0xFF)) << 8;
            int16_t add_int = (int16_t)(int8_t)((raw >> 8) & 0xFF);
            uint32_t frac_sum = (uint32_t)(uint16_t)ert.entity_bg_v_velocity_hi[idx] + (uint32_t)add_frac;
            ert.entity_bg_v_velocity_hi[idx] = (int16_t)(uint16_t)frac_sum;
            ert.entity_bg_v_velocity_lo[idx] += add_int + (int16_t)(frac_sum >> 16);
        }
        return pc;
    }

    /* ---- 0x37: BG_ADD_H_OFF — add to BG horizontal scroll offset ---- */
    case 0x37: {
        uint8_t bg_idx = sb(pc);
        pc++;
        int16_t val = (int16_t)sw(pc);
        pc += 2;
        uint8_t idx = bg_idx;
        if (idx < MAX_BG_LAYERS) {
            ert.entity_bg_h_offset_lo[idx] += val;
        }
        return pc;
    }

    /* ---- 0x38: BG_ADD_V_OFF — add to BG vertical scroll offset ---- */
    case 0x38: {
        uint8_t bg_idx = sb(pc);
        pc++;
        int16_t val = (int16_t)sw(pc);
        pc += 2;
        uint8_t idx = bg_idx;
        if (idx < MAX_BG_LAYERS) {
            ert.entity_bg_v_offset_lo[idx] += val;
        }
        return pc;
    }

    /* ---- 0x3A: CLEAR_BG_VEL — zero BG layer scroll velocity ---- */
    case 0x3A: {
        uint8_t bg_idx = sb(pc);
        pc++;
        uint8_t idx = bg_idx;
        if (idx < MAX_BG_LAYERS) {
            ert.entity_bg_h_velocity_hi[idx] = 0;
            ert.entity_bg_h_velocity_lo[idx] = 0;
            ert.entity_bg_v_velocity_hi[idx] = 0;
            ert.entity_bg_v_velocity_lo[idx] = 0;
        }
        return pc;
    }

    /* ---- 0x3B: SET_ANIMATION — set animation frame ---- */
    case OP_SET_ANIMATION: {
        uint8_t frame = sb(pc);
        pc++;
        if (frame == 0xFF) {
            entities.animation_frame[entity_offset] = -1;
        } else {
            entities.animation_frame[entity_offset] = frame;
        }
        LOG_TRACE("SET_ANIMATION: ent=%d frame=%d (raw 0x%02X)\n",
                  entity_offset, entities.animation_frame[entity_offset], frame);
        return pc;
    }

    /* ---- 0x3C: NEXT_ANIM_FRAME — increment animation frame ---- */
    case OP_NEXT_ANIM_FRAME: {
        entities.animation_frame[entity_offset]++;
        return pc;
    }

    /* ---- 0x3D: PREV_ANIM_FRAME — decrement animation frame ---- */
    case OP_PREV_ANIM_FRAME: {
        entities.animation_frame[entity_offset]--;
        return pc;
    }

    /* ---- 0x3E: SKIP_ANIM_FRAMES — add N to animation frame ---- */
    case OP_SKIP_ANIM_FRAMES: {
        int8_t delta = (int8_t)sb(pc);
        pc++;
        entities.animation_frame[entity_offset] += delta;
        return pc;
    }

    /* ---- 0x3F: SET_X_VELOCITY ---- */
    case OP_SET_X_VELOCITY: {
        uint16_t raw = sw(pc);
        pc += 2;
        uint8_t frac_lo = (uint8_t)(raw & 0xFF);
        int8_t int_hi = (int8_t)((raw >> 8) & 0xFF);
        entities.delta_frac_x[entity_offset] = (uint16_t)frac_lo << 8;
        entities.delta_x[entity_offset] = (int16_t)int_hi;
        return pc;
    }

    /* ---- 0x40: SET_Y_VELOCITY ---- */
    case OP_SET_Y_VELOCITY: {
        uint16_t raw = sw(pc);
        pc += 2;
        uint8_t frac_lo = (uint8_t)(raw & 0xFF);
        int8_t int_hi = (int8_t)((raw >> 8) & 0xFF);
        entities.delta_frac_y[entity_offset] = (uint16_t)frac_lo << 8;
        entities.delta_y[entity_offset] = (int16_t)int_hi;
        return pc;
    }

    /* ---- 0x41: SET_Z_VELOCITY ---- */
    case OP_SET_Z_VELOCITY: {
        uint16_t raw = sw(pc);
        pc += 2;
        uint8_t frac_lo = (uint8_t)(raw & 0xFF);
        int8_t int_hi = (int8_t)((raw >> 8) & 0xFF);
        entities.delta_frac_z[entity_offset] = (uint16_t)frac_lo << 8;
        entities.delta_z[entity_offset] = (int16_t)int_hi;
        return pc;
    }

    /* ---- 0x42: CALLROUTINE — call C function via ROM address dispatch ---- */
    case OP_CALLROUTINE: {
        uint32_t rom_addr = s24(pc);
        pc += 3;

        LOG_TRACE("  CALLROUTINE addr=$%06X pc_after=%u\n", rom_addr, pc);

        uint16_t new_pc;
        int16_t result = callroutine_dispatch(rom_addr, entity_offset,
                                              script_offset, pc, &new_pc);
        scripts.tempvar[script_offset] = result;
        return new_pc;
    }

    /* ---- 0x43: SET_PRIORITY ---- */
    case OP_SET_PRIORITY: {
        uint8_t prio = sb(pc);
        pc++;
        entities.draw_priority[entity_offset] = prio;
        return pc;
    }

    /* ---- 0x44: SLEEP_FROM_TEMPVAR — if tempvar != 0, sleep that many frames ---- */
    case OP_SLEEP_FROM_TEMPVAR: {
        int16_t tv = scripts.tempvar[script_offset];
        if (tv != 0) {
            scripts.sleep_frames[script_offset] = tv;
        }
        return pc;
    }

    default:
        if (opcode < 0x45) {
            uint8_t arg_size = opcode_arg_sizes[opcode];
            if (arg_size == 0xFF) {
                /* Variable-length opcode — cannot safely skip */
                FATAL("unimplemented variable-length opcode 0x%02X "
                      "(ent=%d scr=%d)\n",
                      opcode, entity_offset, script_offset);
            }
            LOG_WARN("WARN: unimplemented opcode 0x%02X (%u arg bytes) "
                  "(ent=%d scr=%d) — skipping\n",
                  opcode, arg_size, entity_offset, script_offset);
            return pc + arg_size;  /* skip argument bytes */
        }
        FATAL("invalid opcode 0x%02X at pc=%u bank=%d "
              "(ent=%d scr=%d)\n",
              opcode, pc - 1, current_bank_idx, entity_offset, script_offset);
    }
}

/*
 * FREE_ACTION_SCRIPT — remove a script from its entity's script chain
 * and return it to the free list.
 */
static void free_action_script(int16_t entity_offset, int16_t script_offset) {
    int16_t script_idx = entities.script_index[entity_offset];

    if (script_idx == script_offset) {
        entities.script_index[entity_offset] = scripts.next_script[script_offset];
    } else {
        int16_t prev = script_idx;
        while (prev >= 0 && scripts.next_script[prev] != script_offset) {
            prev = scripts.next_script[prev];
        }
        if (prev >= 0) {
            scripts.next_script[prev] = scripts.next_script[script_offset];
        }
    }

    /* UNLINK_ACTION_SCRIPT checks: if we're freeing the script that
     * ert.actionscript_current_script points to, advance it so the
     * iteration loop doesn't follow a freed slot. */
    if (ert.actionscript_current_script == script_offset) {
        ert.actionscript_current_script = scripts.next_script[script_offset];
    }

    scripts.next_script[script_offset] = ert.last_allocated_script;
    ert.last_allocated_script = script_offset;
}
