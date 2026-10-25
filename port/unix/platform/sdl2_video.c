#include "platform/platform.h"
#include "game/smooth_camera.h"
#include "snes/ppu.h"
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Initial window scale factor (window is resizable) */
#ifndef EB_WINDOW_SCALE
#define EB_WINDOW_SCALE 3
#endif

static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *texture;

/* Locked texture state (valid between begin_frame and end_frame) */
static pixel_t *locked_pixels;
static int locked_pitch;

/* Headless render target, so debug dumps (scripted playtests) have pixels */
static pixel_t headless_fb[EB_VIEWPORT_WIDTH * EB_VIEWPORT_HEIGHT];

/* Count of frames rendered, so scripted screenshots can tell whether the
 * current frame reached the framebuffer (sdl2_input_script.c) */
int video_frames_rendered;

/* ---- Smooth camera (presentation only; game/smooth_camera.h) ----------------
 * The stock camera scrolls BG by the integer leader position, so the scenery
 * judders 0/1/2 px per frame while Ness stays put (the original does this
 * too). With --smooth-camera, an eligible frame is rendered twice: the normal
 * pass (which also records which output pixels the party's sprites won) and a
 * pass with the party's OAM entries parked off-screen (scenery only). The
 * present step then draws the scenery shifted by the leader's sub-pixel
 * fraction, rounded to whole physical pixels, and the party pixels unshifted
 * on top. Game state is never modified (the parked OAM Y is restored before
 * returning).
 *
 * Both images share one ARGB8888 texture (scenery on top, the masked party
 * below), so a smoothed frame still costs one texture upload, which is the
 * expensive step on some renderers (~1 ms on macOS Metal); the normal
 * texture is not touched on those frames. */
static SDL_Texture *smooth_tex;      /* EB_VIEWPORT_WIDTH x 2*EB_VIEWPORT_HEIGHT */
static pixel_t scenery_fb[EB_VIEWPORT_WIDTH * EB_VIEWPORT_HEIGHT];
static uint8_t party_mask[EB_VIEWPORT_WIDTH * EB_VIEWPORT_HEIGHT];
static bool smooth_pending;          /* this frame presents smoothed */
static uint16_t smooth_frac_x, smooth_frac_y;

/* Headless capture of the final scaled output (EB_SMOOTH_CAMERA_CAPTURE=PATH):
 * a software renderer draws each rendered frame exactly as the window would at
 * EB_WINDOW_SCALE, and the result is appended to PATH as raw rgb24. It follows
 * the recorder's pause state (--record-paused, script 'record on|off'); add
 * --record-video /dev/null so headless frame-skip can't drop a frame. */
static FILE *capture_out;
static SDL_Surface *capture_surface;
static SDL_Renderer *capture_renderer;
static SDL_Texture *capture_texture;
static uint8_t capture_rgb[EB_VIEWPORT_WIDTH * EB_WINDOW_SCALE *
                           EB_VIEWPORT_HEIGHT * EB_WINDOW_SCALE * 3];

/* Optional timing report (EB_SMOOTH_CAMERA_PROFILE=1) */
static bool smooth_profile;
static uint64_t prof_a_ticks, prof_b_ticks, prof_present_ticks, prof_plain_ticks;
static uint32_t prof_plain_frames;
static uint32_t prof_frames;

/* pixel_t -> ARGB8888, built once with SDL's own conversion so the colours
 * match the normal BGR565 texture exactly (SDL_ConvertPixels per frame costs
 * ~1 ms per image; the table lookup is a few hundred microseconds). */
static uint32_t *pixel_to_argb;

static bool create_smooth_textures(SDL_Renderer *r) {
    if (!pixel_to_argb) {
        static uint16_t all[65536];
        for (int i = 0; i < 65536; i++)
            all[i] = (uint16_t)i;
        pixel_to_argb = malloc(65536 * sizeof(uint32_t));
        if (!pixel_to_argb ||
            SDL_ConvertPixels(65536, 1, SDL_PIXELFORMAT_BGR565, all, sizeof(all),
                              SDL_PIXELFORMAT_ARGB8888, pixel_to_argb,
                              65536 * sizeof(uint32_t)) != 0)
            return false;
    }
    smooth_tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888,
                                   SDL_TEXTUREACCESS_STREAMING,
                                   EB_VIEWPORT_WIDTH, EB_VIEWPORT_HEIGHT * 2);
    if (!smooth_tex)
        return false;
    SDL_SetTextureBlendMode(smooth_tex, SDL_BLENDMODE_BLEND);
    return true;
}

static void open_capture(const char *path) {
    capture_out = fopen(path, "wb");
    if (!capture_out) {
        fprintf(stderr, "smooth camera capture: cannot open %s\n", path);
        return;
    }
    capture_surface = SDL_CreateRGBSurfaceWithFormat(
        0, EB_VIEWPORT_WIDTH * EB_WINDOW_SCALE, EB_VIEWPORT_HEIGHT * EB_WINDOW_SCALE,
        24, SDL_PIXELFORMAT_RGB24);
    if (capture_surface)
        capture_renderer = SDL_CreateSoftwareRenderer(capture_surface);
    if (capture_renderer) {
        SDL_RenderSetLogicalSize(capture_renderer, EB_VIEWPORT_WIDTH, EB_VIEWPORT_HEIGHT);
        SDL_RenderSetIntegerScale(capture_renderer, SDL_TRUE);
        capture_texture = SDL_CreateTexture(capture_renderer, SDL_PIXELFORMAT_BGR565,
                                            SDL_TEXTUREACCESS_STREAMING,
                                            EB_VIEWPORT_WIDTH, EB_VIEWPORT_HEIGHT);
    }
    if (!capture_texture || !create_smooth_textures(capture_renderer)) {
        fprintf(stderr, "smooth camera capture: renderer setup failed: %s\n", SDL_GetError());
        fclose(capture_out);
        capture_out = NULL;
        return;
    }
    printf("CAPTURE video %dx%d rgb24\n", EB_VIEWPORT_WIDTH * EB_WINDOW_SCALE,
           EB_VIEWPORT_HEIGHT * EB_WINDOW_SCALE);
    fflush(stdout);
}

/* The scale the frame will be presented at (0 = not presented) */
static float present_scale(void) {
    float sx = 0, sy = 0;
    SDL_Renderer *r = platform_headless ? capture_renderer : renderer;
    if (!r)
        return 0;
    SDL_RenderGetScale(r, &sx, &sy);
    return sx < sy ? sx : sy;
}

static void scenery_send_scanline(int y, const pixel_t *pixels) {
    memcpy(&scenery_fb[y * EB_VIEWPORT_WIDTH], pixels, EB_VIEWPORT_WIDTH * sizeof(pixel_t));
}

static scanline_stamp_cb_t current_stamp_cb;

static void send_scanline_with_stamp(int y, const pixel_t *pixels) {
    current_stamp_cb(y, (pixel_t *)pixels);
    platform_video_send_scanline(y, pixels);
}

static void scenery_send_scanline_with_stamp(int y, const pixel_t *pixels) {
    current_stamp_cb(y, (pixel_t *)pixels);
    scenery_send_scanline(y, pixels);
}

/* Desktop override of src/platform/platform_render.c (linker precedence: this
 * object resolves the symbol, so the library's default member is not pulled).
 * With the smooth camera off this is exactly the default sequence. */
void platform_render_frame(scanline_stamp_cb_t fps_overlay_cb) {
    static SmoothCameraFrame sf;
    current_stamp_cb = fps_overlay_cb;
    scanline_callback_t send = fps_overlay_cb ? send_scanline_with_stamp
                                              : platform_video_send_scanline;

    smooth_pending = smooth_camera_enabled && present_scale() >= 2.0f &&
                     smooth_camera_prepare(&sf);

    platform_video_begin_frame();
    if (!smooth_pending) {
        uint64_t t = smooth_profile ? SDL_GetPerformanceCounter() : 0;
        ppu_render_frame(send);
        if (smooth_profile) {
            prof_plain_ticks += SDL_GetPerformanceCounter() - t;
            prof_plain_frames++;
        }
        platform_video_end_frame();
        return;
    }

    uint64_t t0 = SDL_GetPerformanceCounter();
    ppu_set_obj_mask(sf.party_oam, party_mask);
    ppu_render_frame(send);
    ppu_set_obj_mask(NULL, NULL);
    uint64_t t1 = SDL_GetPerformanceCounter();

    /* Scenery pass: park the party's OAM entries below the screen, render,
     * then put them back so the game sees its OAM unchanged. */
    int16_t saved_y[128];
    for (int i = 0; i < 128; i++) {
        saved_y[i] = ppu.oam_full_y[i];
        if (sf.party_oam[i])
            ppu.oam_full_y[i] = EB_VIEWPORT_HEIGHT;
    }
    ppu_render_frame(fps_overlay_cb ? scenery_send_scanline_with_stamp
                                    : scenery_send_scanline);
    memcpy(ppu.oam_full_y, saved_y, sizeof(saved_y));
    uint64_t t2 = SDL_GetPerformanceCounter();

    smooth_frac_x = sf.frac_x;
    smooth_frac_y = sf.frac_y;
    platform_video_end_frame(); /* adds the composite (not vsync) to prof_present_ticks */

    if (smooth_profile) {
        prof_a_ticks += t1 - t0;
        prof_b_ticks += t2 - t1;
        if (++prof_frames == 300) {
            double us = 1e6 / (double)SDL_GetPerformanceFrequency() / prof_frames;
            double plain = prof_plain_frames
                ? 1e6 / (double)SDL_GetPerformanceFrequency() * prof_plain_ticks / prof_plain_frames
                : 0;
            printf("SMOOTHPROF/300fr: masked pass %.0f us, scenery pass %.0f us, "
                   "composite %.0f us (plain single pass %.0f us over %u frames)\n",
                   prof_a_ticks * us, prof_b_ticks * us, prof_present_ticks * us,
                   plain, (unsigned)prof_plain_frames);
            prof_plain_ticks = 0;
            prof_plain_frames = 0;
            fflush(stdout);
            prof_a_ticks = prof_b_ticks = prof_present_ticks = 0;
            prof_frames = 0;
        }
    }
}

/* Draw this frame onto r: the normal texture, or the smoothed composite. */
static void present_to(SDL_Renderer *r, SDL_Texture *normal_tex) {
    SDL_RenderClear(r);
    if (!smooth_pending) {
        SDL_RenderCopy(r, normal_tex, NULL, NULL);
        return;
    }

    void *pixels;
    int pitch;
    if (SDL_LockTexture(smooth_tex, NULL, &pixels, &pitch) != 0)
        return;
    for (int y = 0; y < EB_VIEWPORT_HEIGHT; y++) {
        uint32_t *scenery_row = (uint32_t *)((uint8_t *)pixels + (size_t)y * pitch);
        uint32_t *party_row =
            (uint32_t *)((uint8_t *)pixels + (size_t)(y + EB_VIEWPORT_HEIGHT) * pitch);
        const pixel_t *s_src = &scenery_fb[y * EB_VIEWPORT_WIDTH];
        const pixel_t *p_src = &headless_fb[y * EB_VIEWPORT_WIDTH];
        const uint8_t *m = &party_mask[y * EB_VIEWPORT_WIDTH];
        for (int x = 0; x < EB_VIEWPORT_WIDTH; x++) {
            scenery_row[x] = pixel_to_argb[s_src[x]];
            party_row[x] = m[x] ? pixel_to_argb[p_src[x]] : 0;
        }
    }
    SDL_UnlockTexture(smooth_tex);

    /* Offset in logical pixels, snapped to whole physical pixels so every
     * source pixel stays exactly `scale` wide. The camera is at
     * integer + frac, so the scenery moves left/up by frac. */
    float scale = present_scale();
    float ox = SDL_roundf((float)smooth_frac_x * scale / 65536.0f) / scale;
    float oy = SDL_roundf((float)smooth_frac_y * scale / 65536.0f) / scale;

    SDL_Rect clip = {0, 0, EB_VIEWPORT_WIDTH, EB_VIEWPORT_HEIGHT};
    SDL_RenderSetClipRect(r, &clip);
    /* Underlay at the integer position: fills the < 1 px strip the shifted
     * copy uncovers at the right/bottom edge with the neighbouring rendered
     * column/row instead of black. */
    const SDL_Rect scenery_src = {0, 0, EB_VIEWPORT_WIDTH, EB_VIEWPORT_HEIGHT};
    const SDL_Rect party_src = {0, EB_VIEWPORT_HEIGHT, EB_VIEWPORT_WIDTH, EB_VIEWPORT_HEIGHT};
    SDL_RenderCopy(r, smooth_tex, &scenery_src, NULL);
    SDL_FRect dst = {-ox, -oy, (float)EB_VIEWPORT_WIDTH, (float)EB_VIEWPORT_HEIGHT};
    SDL_RenderCopyF(r, smooth_tex, &scenery_src, &dst);
    SDL_RenderCopy(r, smooth_tex, &party_src, NULL);
    SDL_RenderSetClipRect(r, NULL);
}

bool record_is_paused(void); /* sdl2_record.c: --record-paused / 'record on|off' */

static void capture_frame(void) {
    if (record_is_paused())
        return;
    if (!smooth_pending)
        SDL_UpdateTexture(capture_texture, NULL, headless_fb, EB_VIEWPORT_WIDTH * sizeof(pixel_t));
    present_to(capture_renderer, capture_texture);
    SDL_RenderPresent(capture_renderer);
    const int w = EB_VIEWPORT_WIDTH * EB_WINDOW_SCALE;
    const int h = EB_VIEWPORT_HEIGHT * EB_WINDOW_SCALE;
    for (int y = 0; y < h; y++)
        memcpy(&capture_rgb[(size_t)y * w * 3],
               (const uint8_t *)capture_surface->pixels + (size_t)y * capture_surface->pitch,
               (size_t)w * 3);
    fwrite(capture_rgb, 1, sizeof(capture_rgb), capture_out);
}

/* The last rendered frame (headless_fb also mirrors windowed frames) */
const pixel_t *video_last_frame(void) {
    return headless_fb;
}

bool platform_video_init(void) {
    const char *env = getenv("EB_SMOOTH_CAMERA");
    if (env && env[0] && strcmp(env, "0") != 0)
        smooth_camera_enabled = true;
    env = getenv("EB_SMOOTH_CAMERA_PROFILE");
    smooth_profile = env && env[0] && strcmp(env, "0") != 0;

    if (platform_headless) {
        env = getenv("EB_SMOOTH_CAMERA_CAPTURE");
        if (env && env[0])
            open_capture(env);
        return true;  /* No window needed */
    }

    window = SDL_CreateWindow(
        "EarthBound",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        EB_VIEWPORT_WIDTH * EB_WINDOW_SCALE, EB_VIEWPORT_HEIGHT * EB_WINDOW_SCALE,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE
    );
    if (!window) return false;

    renderer = SDL_CreateRenderer(window, -1,
        SDL_RENDERER_ACCELERATED);
    if (!renderer) return false;

    /* Enable VSync — use runtime call for reliable driver support */
    SDL_RenderSetVSync(renderer, 1);

    /* Integer scaling for crisp pixels */
    SDL_RenderSetLogicalSize(renderer, EB_VIEWPORT_WIDTH, EB_VIEWPORT_HEIGHT);
    SDL_RenderSetIntegerScale(renderer, SDL_TRUE);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");

    texture = SDL_CreateTexture(renderer,
        SDL_PIXELFORMAT_BGR565,
        SDL_TEXTUREACCESS_STREAMING,
        EB_VIEWPORT_WIDTH, EB_VIEWPORT_HEIGHT);
    if (!texture) return false;

    if (smooth_camera_enabled && !create_smooth_textures(renderer))
        return false;

    return true;
}

void platform_video_shutdown(void) {
    if (capture_renderer) SDL_DestroyRenderer(capture_renderer); /* frees its textures */
    if (capture_surface)  SDL_FreeSurface(capture_surface);
    if (capture_out)      fclose(capture_out);
    capture_renderer = NULL;
    capture_surface = NULL;
    capture_out = NULL;
    free(pixel_to_argb);
    pixel_to_argb = NULL;
    if (texture)  SDL_DestroyTexture(texture);
    if (renderer) SDL_DestroyRenderer(renderer);
    if (window)   SDL_DestroyWindow(window);
    texture = NULL;
    renderer = NULL;
    window = NULL;
}

void platform_video_begin_frame(void) {
    video_frames_rendered++;
    if (platform_headless) {
        locked_pixels = headless_fb;
        locked_pitch = EB_VIEWPORT_WIDTH * sizeof(pixel_t);
        return;
    }
    if (smooth_pending) {
        /* The frame goes to smooth_tex at present time; the normal pass
         * still lands in headless_fb (debug dumps, party pixels). */
        locked_pixels = headless_fb;
        locked_pitch = EB_VIEWPORT_WIDTH * sizeof(pixel_t);
        return;
    }
    void *pixels;
    int pitch;
    if (SDL_LockTexture(texture, NULL, &pixels, &pitch) == 0) {
        locked_pixels = (pixel_t *)pixels;
        locked_pitch = pitch;
    } else {
        locked_pixels = NULL;
    }
}

void platform_video_send_scanline(int y, const pixel_t *pixels) {
    if (!platform_headless)
        memcpy(&headless_fb[y * EB_VIEWPORT_WIDTH], pixels, EB_VIEWPORT_WIDTH * sizeof(pixel_t));
    if (!locked_pixels || (locked_pixels == headless_fb && !platform_headless))
        return; /* windowed smooth frame: already mirrored above */
    uint8_t *dst = (uint8_t *)locked_pixels + y * locked_pitch;
    memcpy(dst, pixels, EB_VIEWPORT_WIDTH * sizeof(pixel_t));
}

pixel_t *platform_video_get_framebuffer(void) {
    return locked_pixels;
}

void platform_video_end_frame(void) {
    if (platform_headless) {
        if (capture_out) {
            uint64_t t0 = SDL_GetPerformanceCounter();
            capture_frame();
            prof_present_ticks += smooth_pending ? SDL_GetPerformanceCounter() - t0 : 0;
        }
        smooth_pending = false;
        return; /* headless_fb stays readable for the debug dump */
    }
    if (locked_pixels && locked_pixels != headless_fb)
        SDL_UnlockTexture(texture);
    locked_pixels = NULL;
    uint64_t t0 = SDL_GetPerformanceCounter();
    present_to(renderer, texture);
    prof_present_ticks += smooth_pending ? SDL_GetPerformanceCounter() - t0 : 0;
    SDL_RenderPresent(renderer);
    smooth_pending = false;
}

void platform_video_set_vsync(bool enabled) {
    if (renderer)
        SDL_RenderSetVSync(renderer, enabled ? 1 : 0);
}
