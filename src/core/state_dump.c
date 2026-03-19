#include "core/state_dump.h"

#include <stdio.h>
#include <stdint.h>

#include "core/memory.h"
#include "core/math.h"
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
    SECTION_TERMINATOR       = 0xFFFF,
};

static bool write_section(FILE *f, uint16_t id, const void *data, uint32_t size) {
    return fwrite(&id, 2, 1, f) == 1
        && fwrite(&size, 4, 1, f) == 1
        && (size == 0 || fwrite(data, size, 1, f) == 1);
}

bool state_dump_save(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;

    /* Header: magic "EBSD" + version u16 + frame_counter u16 */
    uint32_t magic = 0x44534245; /* "EBSD" little-endian */
    uint16_t version = 1;
    uint16_t frame = (uint16_t)core.frame_counter;
    fwrite(&magic, 4, 1, f);
    fwrite(&version, 2, 1, f);
    fwrite(&frame, 2, 1, f);

    /* Sections */
    write_section(f, SECTION_CORE,             &core,                sizeof(core));
    write_section(f, SECTION_GAME_STATE,       &game_state,          sizeof(game_state));
    write_section(f, SECTION_PARTY_CHARACTERS, party_characters,     sizeof(party_characters));
    write_section(f, SECTION_EVENT_FLAGS,      event_flags,          sizeof(event_flags));
    write_section(f, SECTION_OVERWORLD,        &ow,                  sizeof(ow));
    write_section(f, SECTION_BATTLE,           &bt,                  sizeof(bt));
    write_section(f, SECTION_DISPLAY_TEXT,     &dt,                  sizeof(dt));
    write_section(f, SECTION_WINDOW,           &win,                 sizeof(win));
    write_section(f, SECTION_MAP_LOADER,       &ml,                  sizeof(ml));
    write_section(f, SECTION_PPU,              &ppu,                 sizeof(ppu));
    write_section(f, SECTION_POSITION_BUFFER,  &pb,                  sizeof(pb));
    write_section(f, SECTION_DOOR,             &dr,                  sizeof(dr));
    write_section(f, SECTION_ENTITY_RUNTIME,   &ert,                 sizeof(ert));
    write_section(f, SECTION_ENTITY_SYSTEM,    &entities,            sizeof(entities));
    write_section(f, SECTION_SCRIPTS,          &scripts,             sizeof(scripts));
    write_section(f, SECTION_SPRITE_PRIORITY,  sprite_priority,      sizeof(SpritePriorityQueue) * 4);
    write_section(f, SECTION_FADE,             &fade_state,          sizeof(fade_state));
    write_section(f, SECTION_RNG,              &rng_state,           sizeof(rng_state));
#ifdef ENABLE_AUDIO
    write_section(f, SECTION_AUDIO,            &audio_state,         sizeof(audio_state));
#endif
    write_section(f, SECTION_PSI_ANIMATION,    &psi_animation_state, sizeof(psi_animation_state));

    /* Terminator */
    uint16_t term = SECTION_TERMINATOR;
    fwrite(&term, 2, 1, f);

    fclose(f);
    return true;
}
