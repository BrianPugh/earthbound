/*
 * Headless video/audio recording for playtest replays (playtest/pt.py video).
 *
 *   --record-video PATH   write every frame as raw rgb24 (EB_VIEWPORT_WIDTH x
 *                         EB_VIEWPORT_HEIGHT, 60 fps) to PATH, usually a FIFO
 *                         read by ffmpeg. Prints "RECORD video WxH fps=60"
 *                         before opening PATH, so a driver can start ffmpeg
 *                         with the right size (opening a FIFO blocks until
 *                         the reader opens it).
 *   --record-audio PATH   write the SPC700 output as raw s16le stereo,
 *                         AUDIO_SAMPLES_PER_FRAME samples per frame (an
 *                         effective rate of 534 * 60 = 32040 Hz) to PATH.
 *   --record-paused       start paused; the script's 'record on' starts it.
 *
 * Frames are captured once per frame from platform_input_poll(), i.e. after
 * that frame's render. Headless frame-skip may drop a render; the recorder
 * then renders the frame itself (as the script 'shot' command does), which
 * reads PPU state only, so the game plays identically with or without
 * recording. The audio emulator is write-only from the game's side (nothing
 * reads APU output ports), so running it does not change game state either.
 * While paused, audio is still generated (and dropped) so the music is in
 * step when recording resumes.
 */
#include "platform/platform.h"
#include "game/audio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* sdl2_video.c */
extern int video_frames_rendered;
const pixel_t *video_last_frame(void);

#define AUDIO_SAMPLES_PER_FRAME 534 /* as sdl2_audio.c: 32040 / 60 */

static FILE *video_out;
static FILE *audio_out;
static bool record_paused;
static bool audio_enabled;
static int last_render_count = -1;
static uint32_t frames_recorded;
static uint8_t rgb_frame[EB_VIEWPORT_WIDTH * EB_VIEWPORT_HEIGHT * 3];

static void record_close(void) {
    if (!video_out && !audio_out)
        return;
    if (video_out) {
        fclose(video_out);
        video_out = NULL;
    }
    if (audio_out) {
        fclose(audio_out);
        audio_out = NULL;
    }
    printf("RECORD done frames=%u\n", (unsigned)frames_recorded);
    fflush(stdout);
}

static void register_close(void) {
    static bool registered;
    if (!registered) {
        atexit(record_close);
        registered = true;
    }
}

bool record_open_video(const char *path) {
    printf("RECORD video %dx%d fps=%d\n", EB_VIEWPORT_WIDTH, EB_VIEWPORT_HEIGHT, TARGET_FPS);
    fflush(stdout);
    video_out = fopen(path, "wb");
    if (!video_out) {
        fprintf(stderr, "record: cannot open %s\n", path);
        return false;
    }
    setvbuf(video_out, NULL, _IOFBF, 1 << 20);
    register_close();
    return true;
}

bool record_open_audio(const char *path) {
#ifdef EB_ENABLE_AUDIO
    audio_out = fopen(path, "wb");
    if (!audio_out) {
        fprintf(stderr, "record: cannot open %s\n", path);
        return false;
    }
    printf("RECORD audio s16le stereo %d samples/frame\n", AUDIO_SAMPLES_PER_FRAME);
    fflush(stdout);
    register_close();
    return true;
#else
    (void)path;
    fprintf(stderr, "record: this build has no audio (EB_ENABLE_AUDIO off)\n");
    return false;
#endif
}

/* Start the audio emulator for a headless recording (after game_init) */
void record_start_audio(void) {
#ifdef EB_ENABLE_AUDIO
    if (audio_out && !audio_enabled) {
        audio_init();
        audio_enabled = true;
    }
#endif
}

bool record_active(void) {
    return video_out != NULL || audio_out != NULL;
}

void record_set_paused(bool paused) {
    record_paused = paused;
}

bool record_is_paused(void) {
    return record_paused;
}

uint32_t record_frames(void) {
    return frames_recorded;
}

/* Capture this frame. Call once per frame, after the frame's render. */
void record_frame(void) {
    if (!video_out && !audio_out)
        return;
    if (video_out && !record_paused) {
        if (video_frames_rendered == last_render_count)
            platform_render_frame(NULL); /* frame-skip dropped this render */
        const pixel_t *fb = video_last_frame();
        uint8_t *o = rgb_frame;
        for (int i = 0; i < EB_VIEWPORT_WIDTH * EB_VIEWPORT_HEIGHT; i++) {
            uint32_t px = pixel_to_rgb888(fb[i]);
            *o++ = (uint8_t)(px >> 16);
            *o++ = (uint8_t)(px >> 8);
            *o++ = (uint8_t)px;
        }
        fwrite(rgb_frame, 1, sizeof(rgb_frame), video_out);
    }
    last_render_count = video_frames_rendered;
#ifdef EB_ENABLE_AUDIO
    if (audio_enabled) {
        int16_t samples[AUDIO_SAMPLES_PER_FRAME * 2];
        audio_generate_samples(samples, AUDIO_SAMPLES_PER_FRAME);
        if (audio_out && !record_paused)
            fwrite(samples, sizeof(int16_t), AUDIO_SAMPLES_PER_FRAME * 2, audio_out);
    }
#endif
    if (!record_paused)
        frames_recorded++;
}
