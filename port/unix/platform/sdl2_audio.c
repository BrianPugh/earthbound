#include "platform/platform.h"
#include "game_main.h"
#include "game/audio.h"

#ifdef ENABLE_AUDIO

#include <SDL.h>
#include <string.h>

#define AUDIO_SAMPLE_RATE 32000
#define AUDIO_BUFFER_SAMPLES 1024
#define SAMPLES_PER_FRAME 534  /* 32040 / 60 ~ 534 for NTSC */

static SDL_AudioDeviceID audio_device;
static SDL_mutex *audio_mutex;

/* Overflow buffer for fractional frames between SDL callbacks.
   audio_generate_samples always produces exactly SAMPLES_PER_FRAME samples
   to keep the SPC700/DSP running at the correct rate. When SDL requests
   a non-multiple of SAMPLES_PER_FRAME, leftover samples are buffered here. */
static int16_t overflow_buf[SAMPLES_PER_FRAME * 2];  /* stereo */
static int overflow_count = 0;

static void audio_callback(void *userdata, Uint8 *stream, int len) {
    (void)userdata;
    if (game_is_fast_forward()) {
        memset(stream, 0, len);
        return;
    }
    int16_t *out = (int16_t *)stream;
    int total_samples = len / (int)(2 * sizeof(int16_t));  /* stereo */
    int filled = 0;

    /* Drain overflow from previous callback */
    if (overflow_count > 0) {
        int to_copy = overflow_count;
        if (to_copy > total_samples) to_copy = total_samples;
        memcpy(out, overflow_buf, to_copy * 2 * sizeof(int16_t));
        filled += to_copy;
        if (to_copy < overflow_count) {
            memmove(overflow_buf, &overflow_buf[to_copy * 2],
                    (overflow_count - to_copy) * 2 * sizeof(int16_t));
        }
        overflow_count -= to_copy;
    }

    /* Generate full frames until we have enough */
    while (filled < total_samples) {
        int16_t frame_buf[SAMPLES_PER_FRAME * 2];
        SDL_LockMutex(audio_mutex);
        audio_generate_samples(frame_buf, SAMPLES_PER_FRAME);
        SDL_UnlockMutex(audio_mutex);

        int remaining = total_samples - filled;
        if (remaining >= SAMPLES_PER_FRAME) {
            memcpy(&out[filled * 2], frame_buf,
                   SAMPLES_PER_FRAME * 2 * sizeof(int16_t));
            filled += SAMPLES_PER_FRAME;
        } else {
            /* Partial frame: copy what fits, save rest as overflow */
            memcpy(&out[filled * 2], frame_buf,
                   remaining * 2 * sizeof(int16_t));
            overflow_count = SAMPLES_PER_FRAME - remaining;
            memcpy(overflow_buf, &frame_buf[remaining * 2],
                   overflow_count * 2 * sizeof(int16_t));
            filled += remaining;
        }
    }
}

bool platform_audio_init(void) {
    audio_mutex = SDL_CreateMutex();
    if (!audio_mutex) {
        fprintf(stderr, "audio: failed to create mutex\n");
        return false;
    }

    audio_init();

    SDL_AudioSpec want;
    SDL_memset(&want, 0, sizeof(want));
    want.freq = AUDIO_SAMPLE_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = AUDIO_BUFFER_SAMPLES;
    want.callback = audio_callback;

    audio_device = SDL_OpenAudioDevice(NULL, 0, &want, NULL, 0);
    if (audio_device == 0) {
        fprintf(stderr, "audio: failed to open audio device: %s\n", SDL_GetError());
        return false;
    }

    SDL_PauseAudioDevice(audio_device, 0);  /* start playback */
    return true;
}

void platform_audio_shutdown(void) {
    if (audio_device) {
        SDL_PauseAudioDevice(audio_device, 1);
        SDL_CloseAudioDevice(audio_device);
        audio_device = 0;
    }
    audio_shutdown();
    if (audio_mutex) {
        SDL_DestroyMutex(audio_mutex);
        audio_mutex = NULL;
    }
}

void platform_audio_lock(void) {
    if (audio_mutex) SDL_LockMutex(audio_mutex);
}

void platform_audio_unlock(void) {
    if (audio_mutex) SDL_UnlockMutex(audio_mutex);
}

#else /* !ENABLE_AUDIO */

bool platform_audio_init(void) { return true; }
void platform_audio_shutdown(void) {}
void platform_audio_lock(void) {}
void platform_audio_unlock(void) {}

#endif /* ENABLE_AUDIO */
