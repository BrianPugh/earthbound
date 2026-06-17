#ifndef CORE_TYPES_H
#define CORE_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* SNES screen dimensions (hardware constants — do not change) */
#define SNES_WIDTH  256
#define SNES_HEIGHT 224

/* Viewport dimensions (configurable via -DEB_VIEWPORT_WIDTH=N -DEB_VIEWPORT_HEIGHT=N) */
#ifndef EB_VIEWPORT_WIDTH
#define EB_VIEWPORT_WIDTH  SNES_WIDTH
#endif
#ifndef EB_VIEWPORT_HEIGHT
#define EB_VIEWPORT_HEIGHT SNES_HEIGHT
#endif

/* Padding from SNES area to viewport edges (0 at native resolution) */
#define EB_VIEWPORT_PAD_LEFT  ((EB_VIEWPORT_WIDTH - SNES_WIDTH) / 2)
#define EB_VIEWPORT_PAD_TOP   ((EB_VIEWPORT_HEIGHT - SNES_HEIGHT) / 2)

/* Viewport center coordinates */
#define EB_VIEWPORT_CENTER_X  (EB_VIEWPORT_WIDTH / 2)
#define EB_VIEWPORT_CENTER_Y  (EB_VIEWPORT_HEIGHT / 2)

/* Internal line buffer width: at least SNES_WIDTH to hold non-filling content
 * without overflow when the viewport is narrower than the SNES native res. */
#define LINE_BUF_WIDTH  (EB_VIEWPORT_WIDTH > SNES_WIDTH ? EB_VIEWPORT_WIDTH : SNES_WIDTH)

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

/* Framebuffer pixel type — 16-bit. Default layout is BGR565; ports whose
 * panel hardware expects RGB565 can compile with -DEB_PIXEL_RGB565=1 to
 * swap the red/blue channel positions everywhere a pixel is *constructed*
 * (palette conversion, PIXEL_RGB literals). The arithmetic operators on
 * pixel_t (blend_colors, pixel_apply_brightness) are bit-position symmetric
 * and work for either layout unchanged — green stays at bits [10:5] in both.
 *
 * Cost of the toggle: a one-time tweak of bgr555_to_pixel() and PIXEL_RGB();
 * the per-frame palette shadow (ppu_render.c::cgram_render) carries the chosen
 * layout into every downstream pixel for free, saving the port a per-pixel
 * swap in send_scanline().
 *
 * BGR565 layout: BBBBBGGGGGGRRRRR — R[4:0], G[10:5], B[15:11]
 * RGB565 layout: RRRRRGGGGGGBBBBB — R[15:11], G[10:5], B[4:0] */
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

/* Convert BGR555 to pixel_t.
 * BGR565 path: same channel order, shift the upper 10 bits (B+G) left by 1
 *   to make room for the 6th green bit: 0BBBBBGGGGGRRRRR → BBBBBGGGGGGRRRRR.
 * RGB565 path: also swap R↔B between bits [4:0] and [15:11]. */
static inline pixel_t bgr555_to_pixel(uint16_t bgr) {
#ifdef EB_PIXEL_RGB565
    return (pixel_t)(((bgr & 0x001F) << 11) |     /* R: [4:0]   → [15:11] */
                     ((bgr & 0x03E0) << 1)  |     /* G: [9:5]   → [10:6]  */
                     ((bgr >> 10) & 0x001F));     /* B: [14:10] → [4:0]   */
#else
    return (pixel_t)(((bgr & 0x7FE0) << 1) | (bgr & 0x001F));
#endif
}

/* Apply SNES brightness (0-15) to a pixel. Bit-position symmetric — works
 * for both BGR565 and RGB565 because green is at [10:5] in both and the
 * outer 5-bit fields are treated identically. */
static inline pixel_t pixel_apply_brightness(pixel_t px, uint8_t brightness) {
    uint16_t r = px & 0x1F;
    uint16_t g = (px >> 6) & 0x1F;
    uint16_t b = (px >> 11) & 0x1F;
    r = (r * brightness) / 15;
    g = (g * brightness) / 15;
    b = (b * brightness) / 15;
    return (pixel_t)(r | (g << 6) | (b << 11));
}

/* Construct a pixel_t from 8-bit R, G, B components. */
#ifdef EB_PIXEL_RGB565
#define PIXEL_RGB(r, g, b) \
    (pixel_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))
#else
#define PIXEL_RGB(r, g, b) \
    (pixel_t)(((b) >> 3 << 11) | (((g) & 0xFC) << 3) | (((r) & 0xF8) >> 3))
#endif

/* Convert pixel_t to RGB888 (uint32_t) */
static inline uint32_t pixel_to_rgb888(pixel_t px) {
#ifdef EB_PIXEL_RGB565
    uint8_t r5 = (px >> 11) & 0x1F;
    uint8_t g6 = (px >> 5)  & 0x3F;
    uint8_t b5 = px         & 0x1F;
#else
    uint8_t r5 = px         & 0x1F;
    uint8_t g6 = (px >> 5)  & 0x3F;
    uint8_t b5 = (px >> 11) & 0x1F;
#endif
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

/* ABI-stable pointer field (savestate cross-platform format, build item #3 part B).
 * A raw pointer member is 4 bytes on 32-bit (embedded ARM ILP32) and 8 on 64-bit
 * (desktop LP64), which makes any savestate struct that embeds one have a different
 * size and field offsets across ABIs. A struct serialized verbatim must be
 * byte-identical on both. Declaring a pointer field with ABI_PTR_ALIGN (force 8-byte
 * alignment on every ABI) followed by ABI_PTR_PAD(name) (a 4-byte tail filler on
 * 32-bit only) makes the field occupy the SAME 8-byte, 8-aligned slot everywhere, so
 * the enclosing struct's size + every field offset match across ABIs — with no change
 * at the pointer's use sites. The slot's bytes are meaningless across a load anyway
 * (the live pointer is rebound from a serializable companion; see state_dump_load).
 * Usage:
 *     uint16_t *ABI_PTR_ALIGN content_tilemap;
 *     ABI_PTR_PAD(content_tilemap)
 */
#define ABI_PTR_ALIGN __attribute__((aligned(8)))
#if defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ < 8
#  define ABI_PTR_PAD(name) uint32_t name##_abi_pad;
#else
#  define ABI_PTR_PAD(name)
#endif

#endif /* CORE_TYPES_H */
