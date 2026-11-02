#include "game/audio.h"

#ifdef EB_ENABLE_AUDIO

#include "game/overworld.h"
#include "game/game_state.h"
#include "platform/platform.h"
#include "data/assets.h"
#include "include/binary.h"

#include <stdio.h>
#include <string.h>

#include "core/log.h"

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

/* One frame of audio is AUDIO_FRAME_SAMPLES DSP samples (one per 32 SPC700
 * cycles): 534 * 60 fps = 32040 Hz, the real APU's rate (its resonator runs
 * ~0.1% above the nominal 1.024 MHz; snes9x and the recorder use it too). The
 * frame's samples are read 1:1 from lakesnes's ring behind audio_read_pos, so
 * none is dropped or repeated. */
#define AUDIO_FRAME_SAMPLES 534
#define SPC_CYCLES_PER_SAMPLE 32
#define SPC_CYCLES_PER_FRAME (AUDIO_FRAME_SAMPLES * SPC_CYCLES_PER_SAMPLE)
#define DSP_RING_SAMPLES 0x400  /* lakesnes Dsp.sampleBuffer */

/* APU state */
static Apu *apu;
AudioState audio_state;
static uint16_t audio_read_pos;  /* next unread sample, in Dsp.sampleOffset units */

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

static bool spc_loader_ready(const Apu *a) { return a->outPorts[0] == 0xAA && a->outPorts[1] == 0xBB; }
static bool spc_executed(const Apu *a) { return a->outPorts[0] == 0 && a->outPorts[1] == 0; }
static bool spc_echoed_stop(const Apu *a) { return a->outPorts[0] == 0; }

/* APU command queue: the CPU side of CHANGE_MUSIC and friends, run on the SPC
 * timeline.
 *
 * The asm blocks the CPU in STOP_MUSIC and LOAD_SPC700_DATA while the SPC700
 * answers, and the DSP keeps rendering all that time (the old track's key-off
 * release and echo tail). The port doesn't stall the game for that: the port
 * writes and waits are queued here in program order and audio_generate_samples
 * executes them between SPC cycle slices, so the audio rendered while the
 * engine stops and loads is part of the output stream. Port writes made while
 * the queue is busy join it, so they reach the SPC after the transfer, as the
 * blocked CPU's later writes would; the SFX queue isn't drained meanwhile.
 *
 * Nothing in the game reads the APU, so the only observable is the audio. When
 * nobody pulls samples (headless without --record-audio, fast-forward), the
 * queue is run inline after APU_UNPULLED_FRAMES frames, or when it fills. */
enum { APU_OP_PORT, APU_OP_WAIT_STOP_ECHO, APU_OP_UPLOAD };
typedef struct {
    const uint8_t *data;  /* APU_OP_UPLOAD: pack block-transfer data */
    uint32_t size;
    uint8_t kind;
    uint8_t port;         /* APU_OP_PORT */
    uint8_t value;
} ApuOp;
#define APU_OP_QUEUE_SIZE 32
#define APU_UNPULLED_FRAMES 30
/* Busy-waits have no timeout in the asm; this one only guards against a hang.
 * The engine reads port 0 on its tempo tick, so the $FF answer can take a few
 * frames (~73000 cycles seen). */
#define APU_WAIT_MAX_CYCLES (30 * SPC_CYCLES_PER_FRAME)
/* SPC cycles run between two checks of a pending wait */
#define APU_WAIT_SLICE 256
static ApuOp apu_ops[APU_OP_QUEUE_SIZE];
static uint8_t apu_ops_head;
static uint8_t apu_ops_count;
static uint8_t apu_op_phase;   /* APU_OP_UPLOAD progress */
static int apu_op_waited;      /* SPC cycles spent in the head op's current wait */
static uint8_t frames_unpulled;

/* Wait helper for the head op: true once cond holds or the guard expires. */
static bool apu_wait_done(bool (*cond)(const Apu *), const char *what) {
    if (cond(apu)) return true;
    if (apu_op_waited < APU_WAIT_MAX_CYCLES) return false;
    LOG_WARN("audio: %s\n", what);
    return true;
}

/* LOAD_SPC700_DATA (asm/audio/load_spc700_data.asm) sent to the running engine.
 * WAIT_FOR_SPC700 writes $00FF to APUIO0/1 until it reads $BBAA: the engine's
 * $FF command (main.spc700.s UNK075E) keys off all voices and enters its own
 * RAM loader UNK0EE1, which answers $AA/$BB. The blocks then cross the ports;
 * here they are copied straight into APU RAM (same end state). The terminal
 * header (APUIO2/3 = $0500, APUIO1 = 0, APUIO0 = $CC) makes the loader set
 * CONTROL = $31 and RET into the engine, which ignores the $0500 address: the
 * engine is not restarted, so its state ($0420-$04FF, which start's
 * UNK16A5 zeroes, holding STEREO_MONO_FLAG, and the zero page) survives.
 * @WAIT_EXECUTE then waits for APUIO0/1 to read 0. Only the first load
 * (INITIALIZE_MUSIC_SUBSYSTEM, before the engine runs) goes through the IPL
 * and starts at $0500 (audio_init). Returns true when the upload is done. */
static bool apu_upload_step(const ApuOp *op) {
    switch (apu_op_phase) {
    case 0:
        if (!spc_loader_ready(apu)) {
            apu->inPorts[2] = 0x00;  /* WAIT_FOR_SPC700: STZ APUIO2 / STZ APUIO0 */
            apu->inPorts[3] = 0x00;
            apu->inPorts[0] = 0x00;
            apu->inPorts[1] = 0x00;
            apu->inPorts[0] = 0xFF;  /* LDA #$00FF; STA APUIO0 */
            apu->inPorts[1] = 0x00;
        }
        apu_op_phase = 1;
        apu_op_waited = 0;
        EB_FALLTHROUGH;
    case 1:
        if (!apu_wait_done(spc_loader_ready, "SPC700 loader didn't answer $BBAA"))
            return false;
        audio_upload_pack(op->data, op->size);
        apu->inPorts[2] = (uint8_t)(SPC700_ENGINE_ENTRY & 0xFF);
        apu->inPorts[3] = (uint8_t)(SPC700_ENGINE_ENTRY >> 8);
        apu->inPorts[1] = 0x00;
        apu->inPorts[0] = 0xCC;
        apu_op_phase = 2;
        apu_op_waited = 0;
        EB_FALLTHROUGH;
    default:
        /* @WAIT_BLOCK_ACK sees the $CC echo, but the engine replaces it with 0
         * a few instructions later, so wait for @WAIT_EXECUTE's condition only. */
        return apu_wait_done(spc_executed, "SPC700 engine didn't resume after an upload");
    }
}

/* Run queued ops until one has to wait for the SPC700. Returns true while an
 * op is waiting (the caller runs SPC cycles and calls again), false once the
 * queue is empty. Caller holds the audio lock. */
static bool apu_ops_step(void) {
    while (apu_ops_count) {
        const ApuOp *op = &apu_ops[apu_ops_head];
        bool done;
        switch (op->kind) {
        case APU_OP_PORT:
            apu->inPorts[op->port] = op->value;
            done = true;
            break;
        case APU_OP_WAIT_STOP_ECHO:
            /* STOP_MUSIC @WAIT_APU_READY: READ_APU_IO0 until the engine's
             * READ_PORT echoes the 0 back (asm/audio/stop_music.asm). */
            done = apu_wait_done(spc_echoed_stop, "SPC700 didn't echo the music stop");
            break;
        default:
            done = apu_upload_step(op);
            break;
        }
        if (!done) return true;
        apu_ops_head = (uint8_t)((apu_ops_head + 1) % APU_OP_QUEUE_SIZE);
        apu_ops_count--;
        apu_op_phase = 0;
        apu_op_waited = 0;
    }
    return false;
}

/* Run SPC cycles, slicing them while an op waits so it sees the answer on time. */
static void apu_run_cycles(int cycles) {
    while (cycles > 0) {
        int n = cycles;
        bool waiting = apu_ops_step();
        if (waiting && n > APU_WAIT_SLICE) n = APU_WAIT_SLICE;
        int ran = apu_runCycles(apu, n);
        if (waiting) apu_op_waited += ran;
        cycles -= ran;
    }
    apu_ops_step();  /* apply what became ready at the end of the slice */
}

/* Run the whole queue now; the audio it renders is not heard (no consumer). */
static void apu_ops_drain_inline(void) {
    while (apu_ops_step()) {
        apu_op_waited += apu_runCycles(apu, APU_WAIT_SLICE);
    }
    audio_read_pos = apu->dsp->sampleOffset;
}

static void apu_op_push(ApuOp op) {
    if (apu_ops_count == APU_OP_QUEUE_SIZE)
        apu_ops_drain_inline();
    if (apu_ops_count == 0) {
        apu_op_phase = 0;
        apu_op_waited = 0;
    }
    apu_ops[(apu_ops_head + apu_ops_count) % APU_OP_QUEUE_SIZE] = op;
    apu_ops_count++;
}

/* CPU write to an APU port: immediate when the queue is idle, else in order
 * behind the pending transfer. Caller holds the audio lock. */
static void apu_port_write(uint8_t port, uint8_t value) {
    if (apu_ops_count == 0) {
        apu->inPorts[port] = value;
        return;
    }
    ApuOp op = { .kind = APU_OP_PORT, .port = port, .value = value };
    apu_op_push(op);
}

static void apu_queue_upload(const uint8_t *data, size_t size) {
    ApuOp op = { .data = data, .size = (uint32_t)size, .kind = APU_OP_UPLOAD };
    apu_op_push(op);
}

/* Load an audio pack by index into APU RAM. `to_engine`: through the running
 * engine's loader (CHANGE_MUSIC); else raw (the boot load, see audio_init). */
static bool audio_load_pack(uint8_t pack_index, bool to_engine) {
    size_t size = ASSET_SIZE(ASSET_AUDIOPACKS(pack_index));
    const uint8_t *data = ASSET_DATA(ASSET_AUDIOPACKS(pack_index));
    if (!data) {
        LOG_WARN("audio: failed to load audiopacks/%d.ebm\n", pack_index);
        return false;
    }
    if (to_engine)
        apu_queue_upload(data, size);
    else
        audio_upload_pack(data, size);
    return true;
}

void audio_init(void) {
    /* Load dataset table from donor ROM asset */
    size_t dataset_size = ASSET_SIZE(ASSET_MUSIC_DATASET_TABLE_BIN);
    dataset_table = ASSET_DATA(ASSET_MUSIC_DATASET_TABLE_BIN);
    if (!dataset_table) {
        LOG_WARN("audio: failed to load music/dataset_table.bin\n");
        return;
    }
    dataset_table_count = dataset_size / DATASET_ENTRY_SIZE;

    apu = apu_init(NULL);
    if (!apu) {
        LOG_WARN("audio: apu_init failed (allocation failure)\n");
        return;
    }
    apu->palTiming = false;  /* NTSC */
    apu_reset(apu);

    /* CURRENT_MUSIC_TRACK is plain BSS in the original: RESET clears it to 0 (no
     * track); only STOP_MUSIC sets $FFFF */
    audio_state.current_music_track = 0;
    audio_state.current_primary_sample_pack = 0xFF;
    audio_state.current_secondary_sample_pack = 0xFF;
    audio_state.current_sequence_pack = 0xFF;

    sfx_queue_end = 0;
    sfx_queue_start = 0;
    sfx_upper_bit_flipper = 0;
    audio_effect_bit_flipper = 0;
    memset(sfx_queue, 0, sizeof(sfx_queue));
    apu_ops_head = 0;
    apu_ops_count = 0;
    apu_op_phase = 0;
    apu_op_waited = 0;
    frames_unpulled = 0;

    /* Load initial pack: dataset_table[0].sequence_pack (pack 1).
       This contains the SPC700 engine binary + base sample data.
       Port of INITIALIZE_MUSIC_SUBSYSTEM. */
    uint8_t init_pack = dataset_table[0 * DATASET_ENTRY_SIZE + 2];  /* sequence_pack of entry 0 */
    audio_state.unknown_7eb543 = init_pack;
    audio_state.current_secondary_sample_pack = init_pack;
    audio_load_pack(init_pack, false);

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
        apu_runCycles(apu, SPC_CYCLES_PER_FRAME);
    }
    audio_read_pos = apu->dsp->sampleOffset;

    /* STEREO_MONO_FLAG ($0431) keeps the engine's power-on 0 (stereo) until
     * FILE_SELECT_INIT applies the save's sound_setting (audio_set_channels). */
}

/* SET_AUDIO_CHANNELS (asm/audio/set_num_channels.asm): LOAD_SPC700_DATA of
 * one STEREO_MONO_DATA block, writing the engine's STEREO_MONO_FLAG ($0431):
 * 0 when mode is 0, else 1. The engine centres every voice's pan while the
 * flag is nonzero (main.spc700.s UNK09B8), so mode 0 is stereo and nonzero is
 * mono; callers pass sound_setting - 1. Like any upload it goes through the
 * engine's $FF stop + loader, so the playing music stops. */
#define SPC_STEREO_MONO_FLAG 0x0431
void audio_set_channels(uint16_t mode) {
    if (!apu) return;
    /* STEREO_MONO_DATA (asm/data/stereo_mono_data.asm): one 1-byte block at
     * $0431, then the terminal size word. Static: the queued upload reads it
     * later. */
    static const uint8_t block[2][7] = {
        { 0x01, 0x00, SPC_STEREO_MONO_FLAG & 0xFF, SPC_STEREO_MONO_FLAG >> 8, 0, 0x00, 0x00 },
        { 0x01, 0x00, SPC_STEREO_MONO_FLAG & 0xFF, SPC_STEREO_MONO_FLAG >> 8, 1, 0x00, 0x00 },
    };
    audio_lock();
    apu_queue_upload(block[mode ? 1 : 0], sizeof(block[0]));
    audio_unlock();
}

void audio_shutdown(void) {
    if (apu) {
        apu_free(apu);
        apu = NULL;
        apu_ops_count = 0;
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
        /* WRITE_APU_PORT1($01), then STOP_MUSIC: APUIO0 = 0 and wait for the
         * engine to echo it. The bit-flip protocol is inlined because
         * write_apu_port1() would take the (held) audio lock. The wait runs
         * on the SPC timeline (see the APU command queue), so the old track's
         * release is rendered and heard. */
        apu_port_write(1, 0x01 | audio_effect_bit_flipper);
        audio_effect_bit_flipper ^= 0x80;
        apu_port_write(0, 0x00);
        ApuOp wait = { .kind = APU_OP_WAIT_STOP_ECHO };
        apu_op_push(wait);
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


    /* Load primary sample pack if changed */
    if (primary != audio_state.current_primary_sample_pack && primary != 0xFF) {
        audio_state.current_primary_sample_pack = primary;
        audio_load_pack(primary, true);
    }

    /* Load secondary sample pack if changed */
    if (secondary != audio_state.current_secondary_sample_pack && secondary != 0xFF &&
        secondary != audio_state.unknown_7eb543) {
        audio_state.current_secondary_sample_pack = secondary;
        audio_load_pack(secondary, true);
    }

    /* Load sequence pack if changed */
    if (sequence != audio_state.current_sequence_pack && sequence != 0xFF) {
        audio_state.current_sequence_pack = sequence;
        audio_load_pack(sequence, true);
    }

    /* Send play command: write track number to port 0.
       The SPC700 engine reads port 0 and starts playing. */
    apu_port_write(0, (uint8_t)track_id);

    audio_unlock();
}

void play_sfx(uint16_t sfx_id) {
    if (!apu) return;

    uint8_t id = (uint8_t)sfx_id;
    if (id == 0) {
        /* Special case: SFX 0 writes $57 directly to port 3 */
        audio_lock();
        apu_port_write(3, 0x57);
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
    apu_port_write(0, 0x00);
    audio_state.current_music_track = 0xFFFF;
    audio_unlock();
}

void audio_resync_after_load(void) {
    /* Called after a savestate load. The restored audio_state names the music that
     * SHOULD be playing, but the live SPC700/DSP keeps playing the pre-load track
     * (its ~64 KB of APU RAM is not part of the snapshot). Force change_music() to
     * actually re-issue: clear its caches (the track id + the loaded-pack ids) so it
     * stops the old track, re-uploads the saved track's packs, and restarts it.
     * Not sample-seamless — the song restarts — but the correct music plays and the
     * stale one stops. (A seamless resume would require snapshotting the full APU.) */
    if (!apu) return;

    uint16_t track = audio_state.current_music_track;

    /* Drop any sound effects queued in the pre-load run. Do NOT restore/alter the
     * bit-flippers (sfx_upper_bit_flipper / audio_effect_bit_flipper): they track the
     * toggle parity the live, non-snapshotted SPC700 expects, so they must keep their
     * current values — restoring save-time parity would desync the command protocol. */
    sfx_queue_start = 0;
    sfx_queue_end   = 0;

    /* The live APU's STEREO_MONO_FLAG may come from another session (or a fresh
     * process); re-apply the restored save's sound setting the way
     * FILE_SELECT_INIT does. 0 = not chosen yet (engine default, stereo). */
    if (game_state.sound_setting)
        audio_set_channels((uint16_t)(game_state.sound_setting - 1));

    audio_state.current_music_track          = 0xFFFF;
    audio_state.current_primary_sample_pack   = 0xFF;
    audio_state.current_secondary_sample_pack = 0xFF;
    audio_state.current_sequence_pack         = 0xFF;

    if (track == 0 || track == 0xFFFF) {
        stop_music();
        return;
    }
    change_music(track);
}

void write_apu_port1(uint8_t value) {
    /* Port of WRITE_APU_PORT1 (asm/overworld/write_apu_port1.asm).
     * ORs value with the bit-flip state, writes to APU port 1,
     * then toggles bit 7 of the flipper. The SPC700 engine detects
     * new port 1 commands by watching bit 7 toggle. */
    if (!apu) return;
    audio_lock();
    apu_port_write(1, value | audio_effect_bit_flipper);
    audio_effect_bit_flipper ^= 0x80;
    audio_unlock();
}

void write_apu_port2(uint8_t value) {
    /* Port of WRITE_APU_PORT2 (asm/overworld/write_apu_port2.asm).
     * Directly writes a byte to APU port 2 (no bit-flip protocol). */
    if (!apu) return;
    audio_lock();
    apu_port_write(2, value);
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

    /* Dequeue one SFX per frame and write to port 3 (matching PROCESS_SFX_QUEUE).
     * Not while an APU transfer is pending: the asm's CPU is blocked in it. */
    audio_lock();
    if (apu_ops_count && ++frames_unpulled > APU_UNPULLED_FRAMES)
        apu_ops_drain_inline();
    if (apu_ops_count == 0 && sfx_queue_start != sfx_queue_end) {
        apu->inPorts[3] = sfx_queue[sfx_queue_start];
        sfx_queue_start = (sfx_queue_start + 1) & 7;
    }
    audio_unlock();
}

void audio_generate_samples(int16_t *buffer, int samples) {
    if (!apu) {
        memset(buffer, 0, samples * 2 * sizeof(int16_t));
        return;
    }

    audio_lock();

    /* Run the SPC700 until a frame of new samples is ready, then read them
     * (resampled to `samples`; callers pass AUDIO_FRAME_SAMPLES, so 1:1). */
    frames_unpulled = 0;
    const Dsp *dsp = apu->dsp;
    uint16_t avail;
    while ((avail = (uint16_t)(dsp->sampleOffset - audio_read_pos)) < AUDIO_FRAME_SAMPLES)
        apu_run_cycles((AUDIO_FRAME_SAMPLES - avail) * SPC_CYCLES_PER_SAMPLE);
    if (avail > DSP_RING_SAMPLES)  /* overwritten already: take the newest */
        audio_read_pos = (uint16_t)(dsp->sampleOffset - AUDIO_FRAME_SAMPLES);
    for (int i = 0; i < samples; i++) {
        uint16_t k = (uint16_t)(audio_read_pos + (uint32_t)i * AUDIO_FRAME_SAMPLES / (uint32_t)samples);
        buffer[i * 2] = dsp->sampleBuffer[(k & (DSP_RING_SAMPLES - 1)) * 2];
        buffer[i * 2 + 1] = dsp->sampleBuffer[(k & (DSP_RING_SAMPLES - 1)) * 2 + 1];
    }
    audio_read_pos = (uint16_t)(audio_read_pos + AUDIO_FRAME_SAMPLES);

    audio_unlock();
}

/* audio_lock/audio_unlock delegate to platform-provided mutex. */
void audio_lock(void) {
    platform_audio_lock();
}

void audio_unlock(void) {
    platform_audio_unlock();
}

#endif /* EB_ENABLE_AUDIO */
