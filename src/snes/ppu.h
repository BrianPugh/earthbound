#ifndef SNES_PPU_H
#define SNES_PPU_H

#include "core/types.h"

/* BG mode register bits */
#define BGMODE_MODE_MASK  0x07
#define BGMODE_BG3_PRIO   0x08
#define BGMODE_BG1_16X16  0x10
#define BGMODE_BG2_16X16  0x20
#define BGMODE_BG3_16X16  0x40
#define BGMODE_BG4_16X16  0x80

/* OBSEL register bits */
#define OBSEL_BASE_MASK   0x07
#define OBSEL_GAP_MASK    0x18
#define OBSEL_SIZE_MASK   0xE0

/* Mosaic register bits */
#define MOSAIC_SIZE_SHIFT 4
#define MOSAIC_BG_MASK    0x0F

/* OAM sprite entry (4 bytes per sprite, 128 sprites) */
PACKED_STRUCT
typedef struct {
    uint8_t x;
    uint8_t y;
    uint8_t tile;
    uint8_t attr;  /* vhoopppc - vflip, hflip, priority(2), palette(3), tile_msb */
} OAMEntry;
END_PACKED_STRUCT

/* OAM high table (32 bytes, 2 bits per sprite: x_msb + size) */

/* Per-layer viewport mode (C port extension) */
typedef enum {
    BG_VIEWPORT_CENTER = 0, /* render at SNES_WIDTH centered (default) */
    BG_VIEWPORT_FILL   = 1, /* render at VIEWPORT_WIDTH with tilemap wrapping */
    BG_VIEWPORT_CLAMP  = 2, /* render at SNES_WIDTH centered, edge-extend borders */
} BGViewportMode;

typedef struct {
    /* Memory */
    uint8_t vram[VRAM_SIZE];        /* 64KB VRAM */
    uint16_t cgram[256];            /* 256 BGR555 palette entries */
    OAMEntry oam[128];              /* 128 sprite entries */
    uint8_t oam_hi[32];            /* OAM high table */
    int16_t oam_full_x[128];       /* Full 16-bit X for expanded viewport rendering */
    int16_t oam_full_y[128];       /* Full 16-bit Y for expanded viewport rendering */

    /* Registers */
    uint8_t inidisp;               /* $2100 - display control */
    uint8_t obsel;                 /* $2101 - object size and base */
    uint8_t bgmode;                /* $2105 - BG mode */
    uint8_t mosaic;                /* $2106 - mosaic */

    uint8_t bg_sc[4];              /* $2107-$210A - BG tilemap address/size */
    uint8_t bg_nba[2];             /* $210B-$210C - BG tile data base */

    uint16_t bg_hofs[4];           /* $210D-$2114 - BG horizontal scroll */
    uint16_t bg_vofs[4];           /* BG vertical scroll */

    uint8_t tm;                    /* $212C - main screen layers */
    uint8_t ts;                    /* $212D - sub screen layers */
    uint8_t tmw;                   /* $212E - main screen window mask */
    uint8_t tsw;                   /* $212F - sub screen window mask */

    uint8_t cgwsel;                /* $2130 - color math control A */
    uint8_t cgadsub;               /* $2131 - color math control B */
    uint8_t coldata_r;             /* fixed color red */
    uint8_t coldata_g;             /* fixed color green */
    uint8_t coldata_b;             /* fixed color blue */

    uint8_t w12sel;                /* $2123 - window 1/2 mask for BG1/2 */
    uint8_t w34sel;                /* $2124 - window 1/2 mask for BG3/4 */
    uint8_t wobjsel;               /* $2125 - window 1/2 mask for OBJ/COL */
    uint8_t wh0, wh1, wh2, wh3;   /* $2126-$2129 - window positions */
    uint8_t wbglog;                /* $212A - window logic for BG */
    uint8_t wobjlog;               /* $212B - window logic for OBJ/COL */

    /* VRAM address state */
    uint16_t vram_addr;            /* current VRAM word address */
    uint8_t vmain;                 /* $2115 - VRAM address increment mode */

    /* OAM address state */
    uint16_t oam_addr;

    /* Per-scanline window positions (HDMA emulation for oval window) */
    uint8_t wh0_table[VIEWPORT_HEIGHT]; /* window 1 left per scanline */
    uint8_t wh1_table[VIEWPORT_HEIGHT]; /* window 1 right per scanline */
    bool window_hdma_active;        /* use per-scanline tables instead of static wh0/wh1 */

    uint8_t wh2_table[VIEWPORT_HEIGHT]; /* window 2 left per scanline */
    uint8_t wh3_table[VIEWPORT_HEIGHT]; /* window 2 right per scanline */
    bool window2_hdma_active;       /* use per-scanline tables instead of static wh2/wh3 */

    /* Per-scanline TM/TS override (HDMA emulation for letterbox effect) */
    uint8_t tm_per_scanline[VIEWPORT_HEIGHT];
    uint8_t ts_per_scanline[VIEWPORT_HEIGHT];
    bool tm_hdma_active;

    /* Per-layer viewport mode (C port extension, not real SNES hardware).
     * Controls how each BG layer fills the extended viewport area.
     * Has no effect when VIEWPORT_WIDTH equals SNES_WIDTH.
     * 64-tile-wide tilemaps always fill regardless. */
    BGViewportMode bg_viewport_fill[4];

    /* Sprite offsets (C port extension). Added to all OAM positions
     * during rendering. Used to center SNES-coordinate sprites in
     * scenes that use explicit viewport fill. Defaults to 0. */
    int16_t sprite_x_offset;
    int16_t sprite_y_offset;
} PPUState;

/* Global PPU state */
extern PPUState ppu;

/* Initialize PPU to power-on state */
void ppu_init(void);

/* Reset PPU registers (like a soft reset) */
void ppu_reset(void);

/* Clear color math, window masking, mosaic, fixed color, and per-scanline
 * HDMA override flags.  Call on screen transitions to prevent stale battle
 * effects from bleeding into the next scene. */
void ppu_clear_effects(void);

/* Write to PPU register (address is $21xx offset) */
void ppu_write(uint16_t addr, uint8_t value);

/* Write a word to VRAM at the current address */
void ppu_vram_write(uint16_t data);

/* Set VRAM address */
void ppu_set_vram_addr(uint16_t addr);

/* Write to CGRAM */
void ppu_cgram_write(uint8_t index, uint16_t color);

/* Bulk VRAM write (simulates DMA) */
void ppu_vram_dma(const uint8_t *src, uint16_t vram_word_addr, uint16_t byte_count);

/* Bulk CGRAM write (simulates DMA) */
void ppu_cgram_dma(const uint8_t *src, uint8_t start_color, uint16_t byte_count);

/* Bulk OAM write (simulates DMA) */
void ppu_oam_dma(const uint8_t *src, uint16_t byte_count);

/* Render the current PPU state, calling send_scanline for each output row.
 * The callback receives the scanline Y coordinate and a VIEWPORT_WIDTH-pixel
 * buffer.  No persistent framebuffer is needed. */
typedef void (*scanline_callback_t)(int y, const pixel_t *pixels);
void ppu_render_frame(scanline_callback_t send_scanline);

/* Number of independent render contexts (tile cache + working buffers).
 * Set to 2 via build system when dual-core PPU rendering is enabled. */
#ifndef PPU_NUM_RENDER_CONTEXTS
#define PPU_NUM_RENDER_CONTEXTS 1
#endif

/* Pre-build the shadow palette (BGR555→BGR565) from current CGRAM.
 * Must be called before ppu_render_frame_ex().  ppu_render_frame()
 * calls this automatically; dual-core callers must call it explicitly
 * before signaling core 1 to ensure the palette is visible to both cores. */
void ppu_prepare_palette(void);

/* Extended render entry point for dual-core rendering.
 * ctx_id selects which tile cache and working buffers to use (0 or 1).
 * y_start/y_end/y_stride control the scanline range and step.
 * Borders (top/bottom black lines) are only emitted when y_stride == 1. */
void ppu_render_frame_ex(int ctx_id, int y_start, int y_end, int y_stride,
                         scanline_callback_t send_scanline);

#ifdef PPU_PROFILE
typedef struct {
    uint32_t total, clear, bg, obj, win, composite, send;
    bool ready;
} PPUProfile;
extern PPUProfile ppu_profile;
#endif

#endif /* SNES_PPU_H */
