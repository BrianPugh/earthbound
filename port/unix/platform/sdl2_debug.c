#include "platform/platform.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "snes/ppu.h"
#include "game/window.h"
#include "core/state_dump.h"

static int debug_dump_counter = 0;

/* Write a simple BMP file from an RGB888 (uint32_t) framebuffer */
static void write_bmp(const char *path, const uint32_t *fb, int w, int h) {
    FILE *f = fopen(path, "wb");
    if (!f) return;

    uint32_t row_size = (uint32_t)(w * 3 + 3) & ~3u; /* padded to 4 bytes */
    uint32_t pixel_size = row_size * (uint32_t)h;
    uint32_t file_size = 54 + pixel_size;

    /* BMP header */
    uint8_t hdr[54];
    memset(hdr, 0, 54);
    hdr[0] = 'B'; hdr[1] = 'M';
    hdr[2] = file_size; hdr[3] = file_size >> 8;
    hdr[4] = file_size >> 16; hdr[5] = file_size >> 24;
    hdr[10] = 54; /* pixel data offset */
    hdr[14] = 40; /* DIB header size */
    hdr[18] = w; hdr[19] = w >> 8;
    /* BMP stores height as negative for top-down */
    int32_t neg_h = -h;
    memcpy(&hdr[22], &neg_h, 4);
    hdr[26] = 1; /* planes */
    hdr[28] = 24; /* bpp */

    fwrite(hdr, 1, 54, f);

    /* Max row: 256 pixels * 3 bytes + 3 padding = 771 bytes */
    uint8_t row[772];
    memset(row, 0, row_size);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint32_t px = fb[y * w + x];
            row[x * 3 + 0] = px & 0xFF;         /* B */
            row[x * 3 + 1] = (px >> 8) & 0xFF;  /* G */
            row[x * 3 + 2] = (px >> 16) & 0xFF; /* R */
        }
        fwrite(row, 1, row_size, f);
    }
    fclose(f);
}

/* Dump PPU state to files in debug/ directory.
 * Called with the current framebuffer (from locked texture or local buffer). */
void platform_debug_dump_ppu(const pixel_t *framebuffer) {
    mkdir("debug", 0755);

    char path[256];

    /* Screenshot — convert RGB565 framebuffer to RGB888 for BMP */
    snprintf(path, sizeof(path), "debug/screenshot_%03d.bmp", debug_dump_counter);
    {
        static uint32_t screenshot_rgb888[VIEWPORT_WIDTH * VIEWPORT_HEIGHT];
        for (int i = 0; i < VIEWPORT_WIDTH * VIEWPORT_HEIGHT; i++)
            screenshot_rgb888[i] = pixel_to_rgb888(framebuffer[i]);
        write_bmp(path, screenshot_rgb888, VIEWPORT_WIDTH, VIEWPORT_HEIGHT);
    }
    printf("Debug: wrote %s\n", path);

    /* VRAM raw dump */
    snprintf(path, sizeof(path), "debug/vram_%03d.bin", debug_dump_counter);
    FILE *f = fopen(path, "wb");
    if (f) { fwrite(ppu.vram, 1, VRAM_SIZE, f); fclose(f); }
    printf("Debug: wrote %s\n", path);

    /* CGRAM dump (256 colors as text + binary) */
    snprintf(path, sizeof(path), "debug/cgram_%03d.txt", debug_dump_counter);
    f = fopen(path, "w");
    if (f) {
        for (int i = 0; i < 256; i++) {
            uint16_t c = ppu.cgram[i];
            fprintf(f, "[%3d] $%04X  R=%2d G=%2d B=%2d\n",
                    i, c, BGR555_R(c), BGR555_G(c), BGR555_B(c));
        }
        fclose(f);
    }
    printf("Debug: wrote %s\n", path);

    snprintf(path, sizeof(path), "debug/cgram_%03d.bin", debug_dump_counter);
    f = fopen(path, "wb");
    if (f) { fwrite(ppu.cgram, 2, 256, f); fclose(f); }

    /* PPU registers */
    snprintf(path, sizeof(path), "debug/ppu_regs_%03d.txt", debug_dump_counter);
    f = fopen(path, "w");
    if (f) {
        fprintf(f, "INIDISP  = $%02X (brightness=%d, fblank=%d)\n",
                ppu.inidisp, ppu.inidisp & 0xF, (ppu.inidisp >> 7) & 1);
        fprintf(f, "BGMODE   = $%02X (mode=%d, bg3prio=%d, tilesizes=%d%d%d%d)\n",
                ppu.bgmode, ppu.bgmode & 7, (ppu.bgmode >> 3) & 1,
                (ppu.bgmode >> 4) & 1, (ppu.bgmode >> 5) & 1,
                (ppu.bgmode >> 6) & 1, (ppu.bgmode >> 7) & 1);
        fprintf(f, "OBSEL    = $%02X (base=%d, gap=%d, size=%d)\n",
                ppu.obsel, ppu.obsel & 7, (ppu.obsel >> 3) & 3, (ppu.obsel >> 5) & 7);
        fprintf(f, "MOSAIC   = $%02X\n", ppu.mosaic);
        for (int i = 0; i < 4; i++) {
            uint8_t sc = ppu.bg_sc[i];
            fprintf(f, "BG%dSC    = $%02X (map_base=$%04X, size_h=%d, size_v=%d)\n",
                    i + 1, sc, (sc & 0xFC) << 8, (sc & 1) ? 64 : 32, (sc & 2) ? 64 : 32);
        }
        fprintf(f, "BG12NBA  = $%02X (bg1_base=$%04X, bg2_base=$%04X)\n",
                ppu.bg_nba[0],
                (ppu.bg_nba[0] & 0xF) * 0x1000,
                ((ppu.bg_nba[0] >> 4) & 0xF) * 0x1000);
        fprintf(f, "BG34NBA  = $%02X (bg3_base=$%04X, bg4_base=$%04X)\n",
                ppu.bg_nba[1],
                (ppu.bg_nba[1] & 0xF) * 0x1000,
                ((ppu.bg_nba[1] >> 4) & 0xF) * 0x1000);
        for (int i = 0; i < 4; i++) {
            fprintf(f, "BG%d_HOFS = $%04X  BG%d_VOFS = $%04X\n",
                    i + 1, ppu.bg_hofs[i], i + 1, ppu.bg_vofs[i]);
        }
        fprintf(f, "TM       = $%02X (layers: %s%s%s%s%s)\n", ppu.tm,
                (ppu.tm & 0x01) ? "BG1 " : "", (ppu.tm & 0x02) ? "BG2 " : "",
                (ppu.tm & 0x04) ? "BG3 " : "", (ppu.tm & 0x08) ? "BG4 " : "",
                (ppu.tm & 0x10) ? "OBJ " : "");
        fprintf(f, "TS       = $%02X\n", ppu.ts);
        fprintf(f, "CGWSEL   = $%02X\n", ppu.cgwsel);
        fprintf(f, "CGADSUB  = $%02X\n", ppu.cgadsub);
        fclose(f);
    }
    printf("Debug: wrote %s\n", path);

    /* win.bg2_buffer dump (text layer software tilemap) */
    snprintf(path, sizeof(path), "debug/bg2buf_%03d.bin", debug_dump_counter);
    f = fopen(path, "wb");
    if (f) { fwrite(win.bg2_buffer, 1, BG2_BUFFER_SIZE, f); fclose(f); }
    printf("Debug: wrote %s\n", path);

    debug_dump_counter++;
}

/* Render VRAM tiles as an image for visual inspection.
   Renders 4bpp tiles in a 16-tile-wide grid. */
void platform_debug_dump_vram_image(void) {
    mkdir("debug", 0755);

    /* Render as 4bpp: 64KB / 32 bytes per tile = 2048 tiles
       Arrange as 32 columns x 64 rows = 2048 tiles
       Each tile is 8x8 = image is 256x512 */
    int cols = 32, rows = 64;
    int img_w = cols * 8, img_h = rows * 8;
    /* Static buffer sized for the largest dump (2bpp: 256x1024 = 262144 pixels) */
    static uint32_t debug_img_buf[256 * 1024];
    uint32_t *img = debug_img_buf;
    memset(img, 0, (size_t)img_w * img_h * sizeof(uint32_t));

    for (int t = 0; t < 2048; t++) {
        int tx = t % cols;
        int ty = t / cols;
        uint32_t tile_addr = (uint32_t)t * 32; /* 4bpp = 32 bytes/tile */

        for (int py = 0; py < 8; py++) {
            uint8_t indices[8];
            if (tile_addr + 32 <= VRAM_SIZE) {
                /* Decode as 4bpp */
                const uint8_t *bp01 = &ppu.vram[tile_addr + py * 2];
                const uint8_t *bp23 = &ppu.vram[tile_addr + 16 + py * 2];
                for (int px = 0; px < 8; px++) {
                    int bit = 7 - px;
                    indices[px] = ((bp01[0] >> bit) & 1)
                                | (((bp01[1] >> bit) & 1) << 1)
                                | (((bp23[0] >> bit) & 1) << 2)
                                | (((bp23[1] >> bit) & 1) << 3);
                }
            } else {
                memset(indices, 0, 8);
            }

            for (int px = 0; px < 8; px++) {
                /* Use CGRAM palette 0 for visualization */
                uint16_t bgr = ppu.cgram[indices[px]];
                uint32_t rgb = bgr555_to_rgb888(bgr);
                img[(ty * 8 + py) * img_w + (tx * 8 + px)] = rgb;
            }
        }
    }

    char path[256];
    snprintf(path, sizeof(path), "debug/vram_tiles_%03d.bmp", debug_dump_counter - 1);
    write_bmp(path, img, img_w, img_h);
    printf("Debug: wrote %s (4bpp tiles, %dx%d)\n", path, img_w, img_h);

    /* Also render as 2bpp for BG3 inspection */
    int cols2 = 32, rows2 = 128;
    int img_w2 = cols2 * 8, img_h2 = rows2 * 8;
    uint32_t *img2 = debug_img_buf; /* Reuse same buffer (4bpp already written) */
    memset(img2, 0, (size_t)img_w2 * img_h2 * sizeof(uint32_t));

    for (int t = 0; t < cols2 * rows2; t++) {
        int tx2 = t % cols2;
        int ty2 = t / cols2;
        uint32_t ta = (uint32_t)t * 16; /* 2bpp = 16 bytes/tile */

        for (int py = 0; py < 8; py++) {
            uint8_t indices2[8];
            if (ta + 16 <= VRAM_SIZE) {
                const uint8_t *rd = &ppu.vram[ta + py * 2];
                for (int px = 0; px < 8; px++) {
                    int bit = 7 - px;
                    indices2[px] = ((rd[0] >> bit) & 1) | (((rd[1] >> bit) & 1) << 1);
                }
            } else {
                memset(indices2, 0, 8);
            }

            for (int px = 0; px < 8; px++) {
                /* Grayscale for 2bpp: 0=black, 1=dark, 2=light, 3=white */
                uint8_t val = indices2[px] * 85;
                img2[(ty2 * 8 + py) * img_w2 + (tx2 * 8 + px)] =
                    (val << 16) | (val << 8) | val;
            }
        }
    }

    snprintf(path, sizeof(path), "debug/vram_2bpp_%03d.bmp", debug_dump_counter - 1);
    write_bmp(path, img2, img_w2, img_h2);
    printf("Debug: wrote %s (2bpp tiles, %dx%d)\n", path, img_w2, img_h2);
}

void platform_debug_dump_state(void) {
    mkdir("debug", 0755);
    char path[256];
    snprintf(path, sizeof(path), "debug/state_%03d.bin", debug_dump_counter++);
    if (state_dump_save(path))
        printf("Debug: wrote %s\n", path);
    else
        printf("Debug: failed to write %s\n", path);
}
