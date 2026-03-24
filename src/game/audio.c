#include "game/audio.h"

#ifdef ENABLE_AUDIO

#include "game/overworld.h"
#include "platform/platform.h"
#include "data/assets.h"
#include "include/binary.h"

#include <stdio.h>
#include <string.h>

#include "apu.h"
#include "spc.h"
#include "dsp.h"

/* Dataset table: 3 bytes per track (primary_sample_pack, secondary_sample_pack, sequence_pack).
   Loaded from donor ROM asset "music/dataset_table.bin". */
#define DATASET_ENTRY_SIZE 3
static const uint8_t *dataset_table;
static size_t dataset_table_count;

/* Sound stone recording track range (seamless transition, no stop before play) */
#define MUSIC_SOUNDSTONE_RECORDING_GIANT_STEP  160
#define MUSIC_SOUNDSTONE_RECORDING_FIRE_SPRING 167

/* SPC700 engine entry point (matching LOAD_SPC700_DATA's default start address) */
#define SPC700_ENGINE_ENTRY 0x0500

/* APU state */
static Apu *apu;
AudioState audio_state;

/* SFX queue (8-entry circular buffer, matching assembly) */
static uint8_t sfx_queue[8];
static uint8_t sfx_queue_end;
static uint8_t sfx_queue_start;
static uint8_t sfx_upper_bit_flipper;

/* Port 1 bit-flip state (separate from SFX port 3 flipper).
 * WRITE_APU_PORT1 (C0AC0C) ORs the value with this, then XORs it with 0x80.
 * The SPC700 engine detects new commands on port 1 by watching bit 7 toggle. */
static uint8_t audio_effect_bit_flipper;

/* Parse and upload a pack's block transfer data directly into APU RAM.
   Pack format: repeated [size:2 LE][addr:2 LE][data:size bytes], terminated by size==0.
   When size==0, the next 2 bytes are the jump address (ignored - we set PC directly). */
static void audio_upload_pack(const uint8_t *pack_data, size_t pack_size) {
    size_t offset = 0;
    while (offset + 2 <= pack_size) {
        uint16_t block_size = read_u16_le(&pack_data[offset]);
        offset += 2;
        if (block_size == 0) {
            /* Terminal block - remaining 2 bytes would be jump address (we ignore it) */
            break;
        }
        if (offset + 2 > pack_size) break;
        uint16_t addr = read_u16_le(&pack_data[offset]);
        offset += 2;
        if (offset + block_size > pack_size) break;
        /* Clamp block_size to avoid writing past the end of 64KB APU RAM */
        if ((uint32_t)addr + block_size > 65536) {
            block_size = 65536 - addr;
        }
        memcpy(&apu->ram[addr], &pack_data[offset], block_size);
        offset += block_size;
    }
}

/* Load an audio pack by index and upload to APU RAM */
static bool audio_load_pack(uint8_t pack_index) {
    size_t size = ASSET_SIZE(ASSET_AUDIOPACKS(pack_index));
    const uint8_t *data = ASSET_DATA(ASSET_AUDIOPACKS(pack_index));
    if (!data) {
        fprintf(stderr, "audio: failed to load audiopacks/%d.ebm\n", pack_index);
        return false;
    }
    audio_upload_pack(data, size);
    return true;
}

void audio_init(void) {
    /* Load dataset table from donor ROM asset */
    size_t dataset_size = ASSET_SIZE(ASSET_MUSIC_DATASET_TABLE_BIN);
    dataset_table = ASSET_DATA(ASSET_MUSIC_DATASET_TABLE_BIN);
    if (!dataset_table) {
        fprintf(stderr, "audio: failed to load music/dataset_table.bin\n");
        return;
    }
    dataset_table_count = dataset_size / DATASET_ENTRY_SIZE;

    apu = apu_init(NULL);
    if (!apu) {
        fprintf(stderr, "audio: apu_init failed (allocation failure)\n");
        return;
    }
    apu->palTiming = false;  /* NTSC */
    apu_reset(apu);

    audio_state.current_music_track = 0xFFFF;
    audio_state.current_primary_sample_pack = 0xFF;
    audio_state.current_secondary_sample_pack = 0xFF;
    audio_state.current_sequence_pack = 0xFF;

    sfx_queue_end = 0;
    sfx_queue_start = 0;
    sfx_upper_bit_flipper = 0;
    audio_effect_bit_flipper = 0;
    memset(sfx_queue, 0, sizeof(sfx_queue));

    /* Load initial pack: dataset_table[0].sequence_pack (pack 1).
       This contains the SPC700 engine binary + base sample data.
       Port of INITIALIZE_MUSIC_SUBSYSTEM. */
    uint8_t init_pack = dataset_table[0 * DATASET_ENTRY_SIZE + 2];  /* sequence_pack of entry 0 */
    audio_state.unknown_7eb543 = init_pack;
    audio_state.current_secondary_sample_pack = init_pack;
    audio_load_pack(init_pack);

    /* Start SPC700 execution at $0500 (engine entry point).
       The IPL boot ROM has been bypassed by direct RAM writes.
       Clear resetWanted to prevent the reset handler from reading the
       reset vector at $FFFE/$FFFF (which is 0 in RAM, not $0500). */
    apu->romReadable = false;
    apu->spc->resetWanted = false;
    apu->spc->pc = SPC700_ENGINE_ENTRY;
    apu->spc->stopped = false;

    /* Run the SPC700 for a few frames to let it initialize */
    for (int i = 0; i < 10; i++) {
        apu_runCycles(apu, 17066);
    }

    /* Set stereo mode: write 1 byte (value $01) to SPC address $0431.
       Port of SET_AUDIO_CHANNELS(1) - stereo mode. */
    apu->ram[0x0431] = 0x01;
}

void audio_shutdown(void) {
    if (apu) {
        apu_free(apu);
        apu = NULL;
    }
    dataset_table = NULL;
    dataset_table_count = 0;
}

void change_music(uint16_t track_id) {
    if (!apu) return;
    if (track_id == audio_state.current_music_track) return;

    /* Assembly line 14-16: STOP_SOUND_EFFECT when transitions are enabled */
    if (!ow.disabled_transitions) {
        play_sfx(0);
    }

    audio_lock();

    /* Sound stone recordings play seamlessly (no stop before play) */
    if (track_id < MUSIC_SOUNDSTONE_RECORDING_GIANT_STEP ||
        track_id > MUSIC_SOUNDSTONE_RECORDING_FIRE_SPRING) {
        /* Signal SPC700 to stop current music.
           Must use the bit-flip protocol on port 1 (matching WRITE_APU_PORT1),
           otherwise audio_effect_bit_flipper desynchronizes from the SPC700
           engine's internal tracker, causing subsequent port 1 writes to be
           silently ignored. We inline the protocol here because
           write_apu_port1() would try to acquire audio_mutex (already held). */
        apu->inPorts[1] = 0x01 | audio_effect_bit_flipper;
        audio_effect_bit_flipper ^= 0x80;
        apu->inPorts[0] = 0x00;

        /* Let the SPC700 engine process the stop command.
           Assembly STOP_MUSIC waits for the engine to acknowledge (all voices
           keyed off, echo disabled, clean idle state). Run a few frames of SPC
           cycles so the engine reaches idle before packs are uploaded. */
        for (int i = 0; i < 3; i++) {
            apu_runCycles(apu, 17066);
        }
    }

    audio_state.current_music_track = track_id;

    if (track_id == 0 || track_id > dataset_table_count) {
        audio_unlock();
        return;
    }

    uint16_t idx = track_id - 1;
    uint8_t primary   = dataset_table[idx * DATASET_ENTRY_SIZE + 0];
    uint8_t secondary = dataset_table[idx * DATASET_ENTRY_SIZE + 1];
    uint8_t sequence  = dataset_table[idx * DATASET_ENTRY_SIZE + 2];

    bool any_pack_loaded = false;

    /* Load primary sample pack if changed */
    if (primary != audio_state.current_primary_sample_pack && primary != 0xFF) {
        audio_state.current_primary_sample_pack = primary;
        audio_load_pack(primary);
        any_pack_loaded = true;
    }

    /* Load secondary sample pack if changed */
    if (secondary != audio_state.current_secondary_sample_pack && secondary != 0xFF &&
        secondary != audio_state.unknown_7eb543) {
        audio_state.current_secondary_sample_pack = secondary;
        audio_load_pack(secondary);
        any_pack_loaded = true;
    }

    /* Load sequence pack if changed */
    if (sequence != audio_state.current_sequence_pack && sequence != 0xFF) {
        audio_state.current_sequence_pack = sequence;
        audio_load_pack(sequence);
        any_pack_loaded = true;
    }

    if (any_pack_loaded) {
        /* Restart SPC700 at engine entry point, matching LOAD_SPC700_DATA
           which always restarts execution at $0500 after the terminal block.
           This re-initializes the engine's internal state (DSP registers,
           instrument tables, echo settings) with the freshly loaded data. */
        apu->spc->resetWanted = false;
        apu->spc->pc = SPC700_ENGINE_ENTRY;
        apu->spc->stopped = false;

        /* Let the engine initialize before sending the track command */
        for (int i = 0; i < 10; i++) {
            apu_runCycles(apu, 17066);
        }
    }

    /* Send play command: write track number to port 0.
       The SPC700 engine reads port 0 and starts playing. */
    apu->inPorts[0] = (uint8_t)track_id;

    audio_unlock();
}

void play_sfx(uint16_t sfx_id) {
    if (!apu) return;

    uint8_t id = (uint8_t)sfx_id;
    if (id == 0) {
        /* Special case: SFX 0 writes $57 directly to port 3 */
        audio_lock();
        apu->inPorts[3] = 0x57;
        audio_unlock();
        return;
    }

    /* Queue the SFX with upper bit flipper (matching PLAY_SOUND assembly).
       Queue is only touched by main thread, no lock needed for queue ops. */
    sfx_queue[sfx_queue_end] = id | sfx_upper_bit_flipper;
    sfx_queue_end = (sfx_queue_end + 1) & 7;
    sfx_upper_bit_flipper ^= 0x80;
}

void stop_music(void) {
    if (!apu) return;

    audio_lock();
    apu->inPorts[0] = 0x00;
    audio_state.current_music_track = 0xFFFF;
    audio_unlock();
}

void write_apu_port1(uint8_t value) {
    /* Port of WRITE_APU_PORT1 (asm/overworld/write_apu_port1.asm).
     * ORs value with the bit-flip state, writes to APU port 1,
     * then toggles bit 7 of the flipper. The SPC700 engine detects
     * new port 1 commands by watching bit 7 toggle. */
    if (!apu) return;
    audio_lock();
    apu->inPorts[1] = value | audio_effect_bit_flipper;
    audio_effect_bit_flipper ^= 0x80;
    audio_unlock();
}

void write_apu_port2(uint8_t value) {
    /* Port of WRITE_APU_PORT2 (asm/overworld/write_apu_port2.asm).
     * Directly writes a byte to APU port 2 (no bit-flip protocol). */
    if (!apu) return;
    audio_lock();
    apu->inPorts[2] = value;
    audio_unlock();
}

uint16_t get_current_music(void) {
    return audio_state.current_music_track;
}

void audio_invalidate_music_cache(void) {
    audio_state.current_music_track = 0xFFFF;
}

void audio_process_sfx_queue(void) {
    if (!apu) return;

    /* Dequeue one SFX per frame and write to port 3 (matching PROCESS_SFX_QUEUE) */
    if (sfx_queue_start != sfx_queue_end) {
        audio_lock();
        apu->inPorts[3] = sfx_queue[sfx_queue_start];
        audio_unlock();
        sfx_queue_start = (sfx_queue_start + 1) & 7;
    }
}

void audio_generate_samples(int16_t *buffer, int samples) {
    if (!apu) {
        memset(buffer, 0, samples * 2 * sizeof(int16_t));
        return;
    }

    audio_lock();

    /* Run ~17066 SPC700 cycles per frame (1.024 MHz / 60 fps).
       The DSP generates one sample every 32 SPC cycles. */
    apu_runCycles(apu, 17066);
    dsp_getSamples(apu->dsp, buffer, samples);

    audio_unlock();
}

/* audio_lock/audio_unlock delegate to platform-provided mutex. */
void audio_lock(void) {
    platform_audio_lock();
}

void audio_unlock(void) {
    platform_audio_unlock();
}

#endif /* ENABLE_AUDIO */
