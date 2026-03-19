#include "game/fade.h"
#include "game/overworld.h"
#include "snes/ppu.h"

#include "game_main.h"

FadeState fade_state;

void fade_to_brightness(uint8_t target, uint8_t step, uint8_t delay) {
    fade_state.target = target;
    uint8_t current = ppu.inidisp & 0x0F;
    if (target > current)
        fade_state.step = (int8_t)step;
    else if (target < current)
        fade_state.step = -(int8_t)step;
    else {
        fade_state.fading = false;
        return;
    }
    fade_state.delay = delay;
    fade_state.counter = 0;
    fade_state.fading = true;
}

void fade_in(uint8_t step, uint8_t delay) {
    /* Assembly FADE_IN (fade_in.asm) does NOT modify INIDISP_MIRROR.
     * It only sets step (positive) and delay.  The caller is responsible
     * for setting initial brightness/force-blank before calling. */
    fade_to_brightness(0x0F, step, delay);
}

void fade_out(uint8_t step, uint8_t delay) {
    fade_to_brightness(0x00, step, delay);
}

bool fade_update(void) {
    if (!fade_state.fading) return false;

    /* Per-frame guard: prevent double-updates when both explicit callers
     * and wait_for_vblank (NMI equivalent) call fade_update in the same frame.
     * Assembly NMI handler updates fade exactly once per vblank. */
    if (fade_state.updated_this_frame) return true;
    fade_state.updated_this_frame = true;

    if (fade_state.counter < fade_state.delay) {
        fade_state.counter++;
        return true;
    }
    fade_state.counter = 0;

    /* Port of NMI handler fade logic (irq_nmi.asm lines 99-114).
     * Assembly: LDA INIDISP_MIRROR / AND #$0F / ADC step / STA INIDISP_MIRROR
     * The AND+STA overwrites the full byte, naturally clearing force blank
     * during fade-in. On fade-out completion (result<0), sets 0x80 (force blank). */
    uint8_t current = ppu.inidisp & 0x0F;
    int16_t next = (int16_t)current + fade_state.step;

    if (next < 0) {
        /* Fade out complete: force blank + disable HDMA
         * Assembly (irq_nmi.asm:103-106): STZ HDMAEN_MIRROR / LDA #$0080 */
        ppu.inidisp = 0x80;
        ppu.window_hdma_active = false;
        fade_state.step = 0;  /* Assembly (irq_nmi.asm:112): STZ step */
        fade_state.fading = false;
    } else if (next >= 0x10) {
        /* Fade in complete: clamp to max brightness
         * Assembly (irq_nmi.asm:108-110): CMP #$0010 / BCC / LDA #$000F */
        ppu.inidisp = 0x0F;
        fade_state.step = 0;  /* Assembly (irq_nmi.asm:112): STZ step */
        fade_state.fading = false;
    } else {
        /* Still fading: write full byte (assembly overwrites INIDISP_MIRROR).
         * This naturally clears force blank bit during fade-in. */
        ppu.inidisp = (uint8_t)next;
    }

    return fade_state.fading;
}

bool fade_active(void) {
    return fade_state.fading;
}

void fade_new_frame(void) {
    fade_state.updated_this_frame = false;
}

void set_force_blank(bool blank) {
    if (blank)
        /* Assembly FORCE_BLANK_AND_WAIT_VBLANK writes 0x80 to INIDISP_MIRROR,
         * which replaces the full byte: force blank ON + brightness 0.
         * This is critical because fade_in later checks brightness to decide
         * whether a fade is needed. */
        ppu.inidisp = 0x80;
    else
        ppu.inidisp &= ~0x80;
}

/* WAIT_FOR_FADE_COMPLETE: Port of asm/system/palette/wait_for_fade_complete.asm.
 * Loops until fade step becomes 0 (fade finished), calling OAM_CLEAR +
 * UPDATE_SCREEN + WAIT_UNTIL_NEXT_FRAME each iteration. */
void wait_for_fade_complete(void) {
    while (fade_active()) {
        oam_clear();
        update_screen();
        wait_for_vblank();
    }
}
