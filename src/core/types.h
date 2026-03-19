#ifndef CORE_TYPES_H
#define CORE_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* SNES screen dimensions (hardware constants — do not change) */
#define SNES_WIDTH  256
#define SNES_HEIGHT 224

/* Viewport dimensions (configurable via CMake -DVIEWPORT_WIDTH=N -DVIEWPORT_HEIGHT=N) */
#ifndef VIEWPORT_WIDTH
#define VIEWPORT_WIDTH  SNES_WIDTH
#endif
#ifndef VIEWPORT_HEIGHT
#define VIEWPORT_HEIGHT SNES_HEIGHT
#endif

/* Padding from SNES area to viewport edges (0 at native resolution) */
#define VIEWPORT_PAD_LEFT  ((VIEWPORT_WIDTH - SNES_WIDTH) / 2)
#define VIEWPORT_PAD_TOP   ((VIEWPORT_HEIGHT - SNES_HEIGHT) / 2)

/* Viewport center coordinates */
#define VIEWPORT_CENTER_X  (VIEWPORT_WIDTH / 2)
#define VIEWPORT_CENTER_Y  (VIEWPORT_HEIGHT / 2)

/* Internal line buffer width: at least SNES_WIDTH to hold non-filling content
 * without overflow when the viewport is narrower than the SNES native res. */
#define LINE_BUF_WIDTH  (VIEWPORT_WIDTH > SNES_WIDTH ? VIEWPORT_WIDTH : SNES_WIDTH)

/* Window scale factor */
#define WINDOW_SCALE 3

/* Target framerate */
#define TARGET_FPS 60

/* Fast-forward speed multiplier (Tab key toggle) */
#ifndef FAST_FORWARD_MULTIPLIER
#define FAST_FORWARD_MULTIPLIER 3
#endif

/* SNES memory sizes */
#define VRAM_SIZE    0x10000  /* 64KB in bytes (32K words) */
#define CGRAM_SIZE   512      /* 256 colors x 2 bytes */
#define OAM_SIZE     544      /* 512 + 32 bytes */
#define WRAM_SIZE    0x20000  /* 128KB */

/* Framebuffer pixel type — always BGR565 (16-bit).
 * SNES source colors are BGR555 (5 bits per channel).  BGR565 preserves
 * all color information and converts from BGR555 with a single shift
 * (same channel order, just widen green by 1 bit).  Halves framebuffer RAM
 * compared to RGB888, which matters on embedded targets.
 *
 * BGR565 layout: BBBBBGGGGGGRRRRR
 *   R = bits  4:0  (5 bits)
 *   G = bits 10:5  (6 bits, 5 bits of precision from SNES)
 *   B = bits 15:11 (5 bits) */
typedef uint16_t pixel_t;

/* Colour helpers (SNES BGR555 format) */
#define BGR555(r, g, b) (((b) << 10) | ((g) << 5) | (r))
#define BGR555_R(c) ((c) & 0x1F)
#define BGR555_G(c) (((c) >> 5) & 0x1F)
#define BGR555_B(c) (((c) >> 10) & 0x1F)

/* Convert BGR555 to RGB888 (used by debug/VRAM visualization) */
static inline uint32_t bgr555_to_rgb888(uint16_t bgr) {
    uint8_t r = (BGR555_R(bgr) * 255 + 15) / 31;
    uint8_t g = (BGR555_G(bgr) * 255 + 15) / 31;
    uint8_t b = (BGR555_B(bgr) * 255 + 15) / 31;
    return (r << 16) | (g << 8) | b;
}

/* Convert BGR555 to pixel_t (BGR565).
 * Same channel order — just shift the upper 10 bits (B+G) left by 1
 * to make room for the 6th green bit: 0BBBBBGGGGGRRRRR → BBBBBGGGGGGRRRRR */
static inline pixel_t bgr555_to_pixel(uint16_t bgr) {
    return (pixel_t)(((bgr & 0x7FE0) << 1) | (bgr & 0x001F));
}

/* Apply SNES brightness (0-15) to a pixel */
static inline pixel_t pixel_apply_brightness(pixel_t px, uint8_t brightness) {
    uint16_t r = px & 0x1F;
    uint16_t g = (px >> 6) & 0x1F;
    uint16_t b = (px >> 11) & 0x1F;
    r = (r * brightness) / 15;
    g = (g * brightness) / 15;
    b = (b * brightness) / 15;
    return (pixel_t)(r | (g << 6) | (b << 11));
}

/* Construct a pixel_t (BGR565) from 8-bit R, G, B components */
#define PIXEL_RGB(r, g, b) \
    (pixel_t)(((b) >> 3 << 11) | (((g) & 0xFC) << 3) | (((r) & 0xF8) >> 3))

/* Convert pixel_t (BGR565) to RGB888 (uint32_t) */
static inline uint32_t pixel_to_rgb888(pixel_t px) {
    uint8_t r5 = px & 0x1F;
    uint8_t g6 = (px >> 5) & 0x3F;
    uint8_t b5 = (px >> 11) & 0x1F;
    uint8_t r = (r5 * 255 + 15) / 31;
    uint8_t g = (g6 * 255 + 31) / 63;
    uint8_t b = (b5 * 255 + 15) / 31;
    return (r << 16) | (g << 8) | b;
}

/* Packed struct helper */
#ifdef _MSC_VER
#define PACKED_STRUCT __pragma(pack(push, 1))
#define END_PACKED_STRUCT __pragma(pack(pop))
#else
#define PACKED_STRUCT _Pragma("pack(push, 1)")
#define END_PACKED_STRUCT _Pragma("pack(pop)")
#endif

/* Static assertion for struct sizes */
#define ASSERT_STRUCT_SIZE(type, size) \
    _Static_assert(sizeof(type) == (size), "sizeof(" #type ") != " #size)

/* Verbosity levels: 0=errors only, 1=+warnings, 2=+trace */
extern int verbose_level;

#define LOG_WARN(...)  do { if (verbose_level >= 1) fprintf(stderr, __VA_ARGS__); } while (0)
#define LOG_TRACE(...) do { if (verbose_level >= 2) fprintf(stderr, __VA_ARGS__); } while (0)

/* Hard failure for unimplemented/unknown code paths — prints and aborts */
#define FATAL(...) do { fprintf(stderr, "FATAL: " __VA_ARGS__); abort(); } while (0)

#endif /* CORE_TYPES_H */
