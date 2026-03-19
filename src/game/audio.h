#ifndef GAME_AUDIO_H
#define GAME_AUDIO_H

#include "core/types.h"

#ifdef ENABLE_AUDIO

typedef struct {
    uint16_t current_music_track;
    uint8_t  current_primary_sample_pack;
    uint8_t  current_secondary_sample_pack;
    uint8_t  current_sequence_pack;
    uint8_t  unknown_7eb543;
} AudioState;
extern AudioState audio_state;

/* Initialize the audio engine (creates APU, loads SPC700 firmware) */
void audio_init(void);

/* Shut down the audio engine */
void audio_shutdown(void);

/* Change music track (uploads required packs, sends play command) */
void change_music(uint16_t track_id);

/* Play a sound effect */
void play_sfx(uint16_t sfx_id);

/* Stop current music */
void stop_music(void);

/* Write to APU port 1 with bit-flip protocol (matching WRITE_APU_PORT1 ASM).
   Used by init_intro to send audio effect commands (e.g., value 2). */
void write_apu_port1(uint8_t value);

/* Write to APU port 2 directly (matching WRITE_APU_PORT2 ASM).
   Used by Giygas death sequence for static noise toggling. */
void write_apu_port2(uint8_t value);

/* Get currently playing track */
uint16_t get_current_music(void);

/* Invalidate the music track cache so the next change_music() call
   will take effect even if the same track ID is requested.
   Port of `STA CURRENT_MUSIC_TRACK` with #$FFFF in assembly. */
void audio_invalidate_music_cache(void);

/* Process queued sound effects (call once per frame from main loop) */
void audio_process_sfx_queue(void);

/* Generate audio samples via APU emulation (called from audio thread) */
void audio_generate_samples(int16_t *buffer, int samples);

/* Lock/unlock audio mutex for thread-safe APU access.
 * Implemented as thin wrappers around platform_audio_lock/unlock. */
void audio_lock(void);
void audio_unlock(void);

#else /* !ENABLE_AUDIO */

typedef struct {
    uint16_t current_music_track;
} AudioState;
static inline void audio_init(void) {}
static inline void audio_shutdown(void) {}
static inline void change_music(uint16_t track_id) { (void)track_id; }
static inline void play_sfx(uint16_t sfx_id) { (void)sfx_id; }
static inline void stop_music(void) {}
static inline void write_apu_port1(uint8_t value) { (void)value; }
static inline void write_apu_port2(uint8_t value) { (void)value; }
static inline uint16_t get_current_music(void) { return 0; }
static inline void audio_invalidate_music_cache(void) {}
static inline void audio_process_sfx_queue(void) {}
static inline void audio_generate_samples(int16_t *buffer, int samples) { (void)buffer; (void)samples; }
static inline void audio_lock(void) {}
static inline void audio_unlock(void) {}

#endif /* ENABLE_AUDIO */

#endif /* GAME_AUDIO_H */
