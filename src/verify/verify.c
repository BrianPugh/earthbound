#ifdef ENABLE_VERIFY

#include "verify/verify.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* Port headers FIRST — before prefix macros are active */
#include "include/binary.h"
#include "snes/ppu.h"

/* Activate LakeSnes symbol renaming, then include LakeSnes headers */
#include "lk_prefix.h"
#include "snes/snes.h"

/*
 * Sync strategy (game-frame-aligned comparison):
 *
 * The port processes one game frame per call to verify_frame(). The SNES
 * emulator may need multiple runFrame() calls to complete one game frame
 * due to lag frames (game logic exceeding one VBlank) or loading time
 * (decompression/DMA taking real cycles).
 *
 * We detect game frame boundaries by checking if the CPU is in the
 * WAIT_UNTIL_NEXT_FRAME spin loop — the point where game logic has
 * finished and the CPU is waiting for the next NMI.
 *
 * Phases:
 *   1. IDLE: Port's CGRAM[0] is still 0 (no palette loaded yet). Just advance
 *      the emulator one frame per port frame. No comparison.
 *
 *   2. SYNCING: Port has loaded its first palette (CGRAM[0] != 0). Run the
 *      emulator forward until it reaches a game frame boundary with matching
 *      CGRAM. This catches the emulator up past its boot sequence.
 *
 *   3. COMPARING: Both are synced. For each port game frame, loop runFrame()
 *      until the emulator reaches a game frame boundary, then compare PPU
 *      state and key RAM variables.
 */
typedef enum {
    VERIFY_IDLE,       /* waiting for port to load its first palette */
    VERIFY_SYNCING,    /* running emulator forward to catch up */
    VERIFY_COMPARING,  /* synced, comparing game-frame-aligned */
} VerifyPhase;

static Snes *emu = NULL;
static uint32_t verify_frame_count = 0;  /* port frames seen */
static uint32_t emu_frame_count = 0;     /* emulator frames run */
static uint32_t compare_frame_count = 0; /* game frames compared */
static uint32_t mismatch_count = 0;
static bool verify_active = false;
static VerifyPhase phase = VERIFY_IDLE;
static bool skip_next_emu_advance = false; /* post-sync alignment */
static bool skip_oam_next_compare = false; /* OAM not yet DMAed at sync point */

#define SYNC_MAX_FRAMES    600   /* max emulator frames per sync call (10s) */
#define MAX_SNES_FRAMES    120   /* max SNES frames per game frame (2s safety) */
#define MISMATCH_LOG_MAX   20    /* stop detailed logging after this many mismatches */

/* Audio sample buffer (LakeSnes requires one, we discard it) */
static int16_t audio_buf[735 * 2];

/* --- Game frame boundary detection ---
 *
 * RAM addresses from build/earthbound.map:
 *   WAIT_UNTIL_NEXT_FRAME  $C08756
 *   NEW_FRAME_STARTED      $7E002B
 *   FRAME_COUNTER          $7E0002
 *   ACTIONSCRIPT_STATE     $7E9641
 *
 * The spin loop within WAIT_UNTIL_NEXT_FRAME:
 *   $C0875F: @UNKNOWN0:  LDA NEW_FRAME_STARTED   (NMI-based spin)
 *   $C08762:              BEQ @UNKNOWN0
 *   $C08769: @WAITFORNOTVBLANK: LDA f:HVBJOY      (HVBJOY-based spin)
 *   $C0876D:              BMI @WAITFORNOTVBLANK
 *   $C0876F: @WAITFORVBLANK: LDA f:HVBJOY
 *   $C08773:              BPL @WAITFORVBLANK
 *
 * After snes_runFrame() returns, if game logic completed, the CPU is back
 * in one of these spin loops with NEW_FRAME_STARTED == 0.
 */
#define EMU_INIDISP_MIRROR     0x000D
#define EMU_NEW_FRAME_STARTED  0x002B
#define EMU_FRAME_COUNTER      0x0002
#define EMU_ACTIONSCRIPT_STATE 0x9641

/* PC range covering all wait/spin paths within WAIT_UNTIL_NEXT_FRAME.
 * Bank is $C0 for HiROM (JSL sets PBR from the $C0xxxx linker address). */
#define WAIT_FUNC_PC_LO  0x875F
#define WAIT_FUNC_PC_HI  0x8773

static bool at_game_frame_boundary(void) {
    uint8_t k = emu->cpu->k;
    /* HiROM: bank $00 $8000-$FFFF mirrors bank $C0; accept both */
    if (k != 0xC0 && k != 0x00) return false;
    uint16_t pc = emu->cpu->pc;
    /* CPU is in WAIT_UNTIL_NEXT_FRAME spin AND NEW_FRAME_STARTED is clear
     * -> game has processed the NMI and is waiting for the next one */
    return (pc >= WAIT_FUNC_PC_LO && pc <= WAIT_FUNC_PC_HI
            && emu->ram[EMU_NEW_FRAME_STARTED] == 0);
}

/* Reconstruct the emulator's INIDISP register value from LakeSnes PPU state */
static uint8_t emu_get_inidisp(void) {
    return (emu->ppu->forcedBlank ? 0x80 : 0) | emu->ppu->brightness;
}

/* --- SIGTRAP / debugger break ---
 *
 * On first mismatch, raise(SIGTRAP) to break into an attached debugger.
 * If no debugger is attached, SIGTRAP would kill the process — so we
 * install a signal handler that just clears a flag and lets us continue.
 */
static volatile sig_atomic_t debugger_available = 1;

static void sigtrap_handler(int sig) {
    (void)sig;
    debugger_available = 0;
}

/* --- Logging helpers --- */

static void log_mismatch(void) {
    if (mismatch_count > MISMATCH_LOG_MAX) return;

    fprintf(stderr, "verify: MISMATCH #%u at game frame %u "
            "(port=%u emu=%u) CPU: PBR=$%02X PC=$%04X SP=$%04X\n",
            mismatch_count, compare_frame_count,
            verify_frame_count, emu_frame_count,
            emu->cpu->k, emu->cpu->pc, emu->cpu->sp);

    /* Log key emulator RAM variables */
    uint16_t emu_fc = read_u16_le(&emu->ram[EMU_FRAME_COUNTER]);
    fprintf(stderr, "  EMU RAM: FRAME_COUNTER=%u ACTIONSCRIPT_STATE=%u "
            "NEW_FRAME_STARTED=%u\n",
            emu_fc,
            emu->ram[EMU_ACTIONSCRIPT_STATE],
            emu->ram[EMU_NEW_FRAME_STARTED]);

    if (mismatch_count == MISMATCH_LOG_MAX) {
        fprintf(stderr, "verify: suppressing further detailed output "
                "(hit %u mismatches)\n", MISMATCH_LOG_MAX);
    }
}

/* --- Dump files with port+emu frame in name --- */

static void dump_binary(const char *name, const void *data, size_t size) {
    char path[256];
    snprintf(path, sizeof(path), "debug/verify_%s_p%06u_e%06u.bin",
             name, verify_frame_count, emu_frame_count);
    FILE *f = fopen(path, "wb");
    if (f) {
        fwrite(data, 1, size, f);
        fclose(f);
    }
}

static void mirror_input(uint16_t pad_state) {
    static const struct { uint16_t mask; int button; } map[] = {
        { 0x8000,  0 },  /* B */
        { 0x4000,  1 },  /* Y */
        { 0x2000,  2 },  /* Select */
        { 0x1000,  3 },  /* Start */
        { 0x0800,  4 },  /* Up */
        { 0x0400,  5 },  /* Down */
        { 0x0200,  6 },  /* Left */
        { 0x0100,  7 },  /* Right */
        { 0x0080,  8 },  /* A */
        { 0x0040,  9 },  /* X */
        { 0x0020, 10 },  /* L */
        { 0x0010, 11 },  /* R */
    };

    for (int i = 0; i < 12; i++) {
        lk_snes_setButtonState(emu, 1, map[i].button, (pad_state & map[i].mask) != 0);
    }
}

/* --- PPU comparison functions --- */

/* Count VRAM mismatches, log and dump on first occurrence per frame */
static bool compare_vram(void) {
    bool match = true;
    int first_mismatch = -1;
    int mismatch_words = 0;

    for (int i = 0; i < 0x8000; i++) {
        uint16_t word = emu->ppu->vram[i];
        uint8_t lo = word & 0xFF;
        uint8_t hi = (word >> 8) & 0xFF;

        if (ppu.vram[i * 2] != lo || ppu.vram[i * 2 + 1] != hi) {
            if (first_mismatch < 0) first_mismatch = i;
            mismatch_words++;
            match = false;
        }
    }

    if (!match && mismatch_count < MISMATCH_LOG_MAX) {
        uint16_t word = emu->ppu->vram[first_mismatch];
        fprintf(stderr, "  VRAM: %d words differ, first at $%04X "
                "(emu=$%04X port=$%02X%02X)\n",
                mismatch_words, first_mismatch, word,
                ppu.vram[first_mismatch * 2 + 1], ppu.vram[first_mismatch * 2]);

        mkdir("debug", 0755);
        uint8_t emu_vram_bytes[0x10000];
        for (int i = 0; i < 0x8000; i++) {
            emu_vram_bytes[i * 2]     = emu->ppu->vram[i] & 0xFF;
            emu_vram_bytes[i * 2 + 1] = (emu->ppu->vram[i] >> 8) & 0xFF;
        }
        dump_binary("vram_emu", emu_vram_bytes, 0x10000);
        dump_binary("vram_port", ppu.vram, 0x10000);
    }

    return match;
}

static bool compare_cgram(void) {
    bool match = true;
    int first_mismatch = -1;
    int mismatch_colors = 0;

    for (int i = 0; i < 256; i++) {
        if (emu->ppu->cgram[i] != ppu.cgram[i]) {
            if (first_mismatch < 0) first_mismatch = i;
            mismatch_colors++;
            match = false;
        }
    }

    if (!match && mismatch_count < MISMATCH_LOG_MAX) {
        fprintf(stderr, "  CGRAM: %d colors differ, first at %d "
                "(emu=$%04X port=$%04X)\n",
                mismatch_colors, first_mismatch,
                emu->ppu->cgram[first_mismatch], ppu.cgram[first_mismatch]);

        mkdir("debug", 0755);
        dump_binary("cgram_emu", emu->ppu->cgram, 256 * 2);
        dump_binary("cgram_port", ppu.cgram, 256 * 2);
    }

    return match;
}

static bool compare_oam(void) {
    bool match = true;

    /* Compare main OAM (512 bytes) */
    uint8_t emu_oam_bytes[512];
    for (int i = 0; i < 256; i++) {
        emu_oam_bytes[i * 2]     = emu->ppu->oam[i] & 0xFF;
        emu_oam_bytes[i * 2 + 1] = (emu->ppu->oam[i] >> 8) & 0xFF;
    }

    if (memcmp(emu_oam_bytes, ppu.oam, 512) != 0) {
        if (mismatch_count < MISMATCH_LOG_MAX) {
            for (int i = 0; i < 512; i++) {
                if (emu_oam_bytes[i] != ((uint8_t *)ppu.oam)[i]) {
                    fprintf(stderr, "  OAM: mismatch at byte %d "
                            "(emu=$%02X port=$%02X)\n",
                            i, emu_oam_bytes[i], ((uint8_t *)ppu.oam)[i]);
                    break;
                }
            }
        }
        match = false;
    }

    /* Compare high OAM (32 bytes) */
    if (memcmp(emu->ppu->highOam, ppu.oam_hi, 32) != 0) {
        if (mismatch_count < MISMATCH_LOG_MAX) {
            for (int i = 0; i < 32; i++) {
                if (emu->ppu->highOam[i] != ppu.oam_hi[i]) {
                    fprintf(stderr, "  OAM hi: mismatch at byte %d "
                            "(emu=$%02X port=$%02X)\n",
                            i, emu->ppu->highOam[i], ppu.oam_hi[i]);
                    break;
                }
            }
        }
        match = false;
    }

    if (!match && mismatch_count < MISMATCH_LOG_MAX) {
        mkdir("debug", 0755);
        dump_binary("oam_emu", emu_oam_bytes, 512);
        dump_binary("oam_port", ppu.oam, 512);
        dump_binary("oam_hi_emu", emu->ppu->highOam, 32);
        dump_binary("oam_hi_port", ppu.oam_hi, 32);
    }

    return match;
}

/* --- Register/RAM comparison --- */

static bool compare_ram_vars(void) {
    bool match = true;

    /* BG1 scroll (set by NMI from mirrors) */
    uint16_t emu_bg1h = emu->ppu->bgLayer[0].hScroll;
    uint16_t emu_bg1v = emu->ppu->bgLayer[0].vScroll;
    if (emu_bg1h != ppu.bg_hofs[0] || emu_bg1v != ppu.bg_vofs[0]) {
        if (mismatch_count < MISMATCH_LOG_MAX) {
            fprintf(stderr, "  BG1 scroll: emu=(%u,%u) port=(%u,%u)\n",
                    emu_bg1h, emu_bg1v, ppu.bg_hofs[0], ppu.bg_vofs[0]);
        }
        match = false;
    }

    /* Brightness / forced blank */
    uint8_t emu_inidisp = emu_get_inidisp();
    if (emu_inidisp != ppu.inidisp) {
        if (mismatch_count < MISMATCH_LOG_MAX) {
            fprintf(stderr, "  INIDISP: emu=$%02X port=$%02X\n",
                    emu_inidisp, ppu.inidisp);
        }
        match = false;
    }

    /* Layer enable (TM) — reconstruct from LakeSnes layer[].mainScreenEnabled */
    uint8_t emu_tm = 0;
    for (int i = 0; i < 5; i++) {
        if (emu->ppu->layer[i].mainScreenEnabled) emu_tm |= (1 << i);
    }
    if (emu_tm != ppu.tm) {
        if (mismatch_count < MISMATCH_LOG_MAX) {
            fprintf(stderr, "  TM: emu=$%02X port=$%02X\n", emu_tm, ppu.tm);
        }
        match = false;
    }

    return match;
}

/* --- Orchestrator --- */

static bool compare_all(void) {
    bool vram_ok  = compare_vram();
    bool cgram_ok = compare_cgram();
    bool oam_ok;
    if (skip_oam_next_compare) {
        /* At sync point, OAM_CLEAR values haven't been DMAed to PPU yet */
        skip_oam_next_compare = false;
        oam_ok = true;
    } else {
        oam_ok = compare_oam();
    }
    bool regs_ok  = compare_ram_vars();

    if (!vram_ok || !cgram_ok || !oam_ok || !regs_ok) {
        if (mismatch_count < MISMATCH_LOG_MAX) {
            fprintf(stderr, "  [VRAM:%s CGRAM:%s OAM:%s REGS:%s]\n",
                    vram_ok  ? "OK" : "FAIL",
                    cgram_ok ? "OK" : "FAIL",
                    oam_ok   ? "OK" : "FAIL",
                    regs_ok  ? "OK" : "FAIL");
        }
        return false;
    }
    return true;
}

/* Check if the emulator's CGRAM matches the port's first N entries */
static bool cgram_matches(int n) {
    for (int i = 0; i < n; i++) {
        if (emu->ppu->cgram[i] != ppu.cgram[i]) return false;
    }
    return true;
}

/* --- Init / Frame / Shutdown --- */

bool verify_init(const char *rom_path) {
    FILE *f = fopen(rom_path, "rb");
    if (!f) {
        fprintf(stderr, "verify: failed to open ROM: %s\n", rom_path);
        return false;
    }

    fseek(f, 0, SEEK_END);
    long rom_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t *rom_data = (uint8_t *)malloc(rom_size);
    if (!rom_data) {
        fclose(f);
        fprintf(stderr, "verify: failed to allocate %ld bytes for ROM\n", rom_size);
        return false;
    }

    if ((long)fread(rom_data, 1, rom_size, f) != rom_size) {
        free(rom_data);
        fclose(f);
        fprintf(stderr, "verify: failed to read ROM\n");
        return false;
    }
    fclose(f);

    emu = lk_snes_init();
    if (!emu) {
        free(rom_data);
        fprintf(stderr, "verify: failed to init emulator\n");
        return false;
    }

    lk_snes_setSamples(emu, audio_buf, 735);

    if (!lk_snes_loadRom(emu, rom_data, (int)rom_size)) {
        free(rom_data);
        lk_snes_free(emu);
        emu = NULL;
        fprintf(stderr, "verify: failed to load ROM\n");
        return false;
    }

    free(rom_data);
    verify_frame_count = 0;
    emu_frame_count = 0;
    compare_frame_count = 0;
    mismatch_count = 0;
    verify_active = true;
    phase = VERIFY_IDLE;

    /* Install SIGTRAP handler so we don't die if no debugger is attached */
    signal(SIGTRAP, sigtrap_handler);

    fprintf(stderr, "verify: initialized, waiting for PPU sync...\n");
    return true;
}

bool verify_frame(uint16_t pad_state) {
    if (!verify_active || !emu) return true;

    verify_frame_count++;

    switch (phase) {
    case VERIFY_IDLE:
        /* Wait for the port to load its first palette */
        if (ppu.cgram[0] == 0) {
            /* Port hasn't loaded anything yet, advance emulator in step */
            lk_snes_runFrame(emu);
            emu_frame_count++;
            return true;
        }
        /* Port has palette data, begin syncing */
        fprintf(stderr, "verify: port loaded palette at port frame %u "
                "(CGRAM[0]=$%04X), syncing emulator...\n",
                verify_frame_count, ppu.cgram[0]);
        phase = VERIFY_SYNCING;
        /* fall through */

    case VERIFY_SYNCING: {
        /* Run emulator forward until it reaches a game frame boundary
         * with matching CGRAM and INIDISP, or we hit the per-call limit.
         *
         * INIDISP matching is critical: during fades with delay>1, there
         * are multiple game boundaries per brightness step. Without
         * INIDISP matching, sync can land on a sub-frame where the game
         * has already bumped INIDISP_MIRROR, causing the next NMI to
         * show a brightness one step ahead of the port. */
        int sync_attempts = 0;
        while (sync_attempts < SYNC_MAX_FRAMES) {
            lk_snes_runFrame(emu);
            emu_frame_count++;
            sync_attempts++;

            if (at_game_frame_boundary() && cgram_matches(16)
                    && emu_get_inidisp() == ppu.inidisp) {
                phase = VERIFY_COMPARING;
                /* The emu's game code has already advanced INIDISP_MIRROR
                 * for the next frame (it runs within the same runFrame after
                 * the NMI). The port hasn't advanced yet — it's still on the
                 * first sub-frame of this brightness step. Skip one emu
                 * advance so they stay aligned. */
                skip_next_emu_advance = true;
                /* At sync, the emu's OAM_CLEAR values are in RAM but haven't
                 * been DMAed to PPU OAM yet (that happens in the next NMI).
                 * Skip OAM comparison for the first post-sync frame. */
                skip_oam_next_compare = true;
                fprintf(stderr, "verify: SYNCED at port=%u emu=%u (%d frames) "
                        "INIDISP=$%02X\n",
                        verify_frame_count, emu_frame_count, sync_attempts,
                        ppu.inidisp);
                return true;
            }
        }

        /* Still not synced, keep trying next port frame */
        fprintf(stderr, "verify: sync still searching (emu=%u, PC=$%02X:%04X "
                "emu_inidisp=$%02X port_inidisp=$%02X)\n",
                emu_frame_count, emu->cpu->k, emu->cpu->pc,
                emu_get_inidisp(), ppu.inidisp);
        return true;
    }

    case VERIFY_COMPARING: {
        mirror_input(pad_state);

        if (skip_next_emu_advance) {
            /* Post-sync alignment: the emu is already at the correct
             * hardware state from the sync frame. The port's next frame
             * is still at the same brightness (second sub-frame of a
             * multi-frame delay). Compare without advancing the emu. */
            skip_next_emu_advance = false;
        } else {
            int snes_frames_this_tick = 0;

            do {
                lk_snes_runFrame(emu);
                emu_frame_count++;
                snes_frames_this_tick++;
            } while (!at_game_frame_boundary() && snes_frames_this_tick < MAX_SNES_FRAMES);

            if (snes_frames_this_tick > 1) {
                fprintf(stderr, "verify: %d SNES frames for port frame %u (lag/loading)\n",
                        snes_frames_this_tick, verify_frame_count);
            }

            if (snes_frames_this_tick >= MAX_SNES_FRAMES) {
                fprintf(stderr, "verify: TIMEOUT - emu stuck at PC=$%02X:%04X\n",
                        emu->cpu->k, emu->cpu->pc);
                return false;
            }
        }

        compare_frame_count++;

        /* Compare PPU + selective RAM */
        bool ok = compare_all();
        if (!ok) {
            mismatch_count++;
            log_mismatch();
            if (mismatch_count == 1 && debugger_available) {
                /* First mismatch: break into debugger if attached */
                raise(SIGTRAP);
            }
        }
        return ok;
    }
    }

    return true; /* unreachable, but satisfies compiler */
}

void verify_shutdown(void) {
    if (emu) {
        lk_snes_free(emu);
        emu = NULL;
    }
    verify_active = false;
    fprintf(stderr, "verify: shutdown - %u port frames, %u emu frames, "
            "%u compared, %u mismatches\n",
            verify_frame_count, emu_frame_count,
            compare_frame_count, mismatch_count);
}

uint32_t verify_get_frame(void) {
    return verify_frame_count;
}

#endif /* ENABLE_VERIFY */
