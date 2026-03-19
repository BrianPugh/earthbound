#include "core/math.h"
#include "game/battle_bg.h"   /* sine_table[] */
#include <stdbool.h>

/* RNG state — two 16-bit variables, matching assembly's RAND_A/RAND_B.
 * Initialized by MOVE_INT_CONSTANT $56781234, RAND_A in reset.asm:
 *   RAND_A = 0x1234, RAND_B = 0x5678 (little-endian 32-bit store). */
RNGState rng_state = { .a = 0x1234, .b = 0x5678 };

void rng_seed(uint32_t seed) {
    rng_state.a = (uint16_t)(seed & 0xFFFF);
    rng_state.b = (uint16_t)(seed >> 16);
    if (rng_state.a == 0 && rng_state.b == 0) {
        rng_state.a = 0x1234;
        rng_state.b = 0x5678;
    }
}

/* Port of assembly RAND (asm/system/math/rand.asm).
 * Uses 8x8 hardware multiply (WRMPYA*WRMPYB), bit rotations, and
 * two state variables (RAND_A, RAND_B). Returns 0-255. */
uint8_t rng_next_byte(void) {
    uint8_t a_lo = (uint8_t)rng_state.a;
    uint8_t b_lo = (uint8_t)rng_state.b;

    /* Hardware multiply: STA f:WRMPYA writes b_lo to $4202, a_lo to $4203 */
    uint16_t mult = (uint16_t)b_lo * (uint16_t)a_lo;

    /* combined = (a_lo << 8) | b_lo; rng_state.b = combined + 0x6D */
    uint16_t combined = ((uint16_t)a_lo << 8) | b_lo;
    uint32_t sum = (uint32_t)combined + 0x006D;
    rng_state.b = (uint16_t)sum;
    bool carry = (sum >> 16) & 1;

    /* LDA f:RDMPYL; ROR; ROR — two rotates through carry on mult result */
    uint16_t val = mult;
    bool new_carry;

    new_carry = val & 1;
    val = (val >> 1) | (carry ? 0x8000 : 0);
    carry = new_carry;

    new_carry = val & 1;
    val = (val >> 1) | (carry ? 0x8000 : 0);
    carry = new_carry;

    uint16_t saved = val; /* PHA */

    /* AND #$0003; CLC; ADC RAND_A; ROR; BCC/ORA #$8000; STA RAND_A */
    sum = (uint32_t)(val & 0x0003) + (uint32_t)rng_state.a;
    carry = (sum >> 16) & 1;
    val = (uint16_t)sum;

    new_carry = val & 1;
    val = (val >> 1) | (carry ? 0x8000 : 0);
    carry = new_carry;

    if (carry) val |= 0x8000;
    /* carry unchanged by BCC/ORA */

    rng_state.a = val;

    /* PLA; ROR; ROR; AND #$00FF — compute return value */
    val = saved;

    new_carry = val & 1;
    val = (val >> 1) | (carry ? 0x8000 : 0);
    carry = new_carry;

    new_carry = val & 1;
    val = (val >> 1) | (carry ? 0x8000 : 0);

    return (uint8_t)(val & 0xFF);
}

uint16_t rng_next(void) {
    return (uint16_t)rng_next_byte();
}

uint32_t mul16(uint16_t a, uint16_t b) {
    return (uint32_t)a * (uint32_t)b;
}

uint16_t div32_16(uint32_t dividend, uint16_t divisor, uint16_t *remainder) {
    if (divisor == 0) {
        if (remainder) *remainder = 0;
        return 0xFFFF;
    }
    if (remainder) *remainder = (uint16_t)(dividend % divisor);
    return (uint16_t)(dividend / divisor);
}

/* COSINE_SINE (asm/system/math/cosine_sine.asm)
 *
 * Port of the SNES Mode 7 multiply: M7A (16-bit signed) x M7B (8-bit signed).
 * Assembly:
 *   SEP #$20        ; 8-bit A
 *   STA f:M7A       ; low byte of value
 *   XBA
 *   STA f:M7A       ; high byte of value
 *   LDA SINE_LOOKUP_TABLE,X  ; 8-bit signed sine
 *   STA f:M7B       ; multiply
 *   REP #$20        ; 16-bit A
 *   LDA f:MPYM      ; read low 16 bits of 24-bit product
 *
 * Returns low 16 bits of (value x sine_table[angle]). */
int16_t cosine_sine(int16_t value, uint8_t angle) {
    return (int16_t)(((int32_t)value * (int32_t)sine_table[angle]) >> 8);
}

/* COSINE — cos(angle) = sin(angle - 64).
 * Assembly subtracts 0x40 from X before falling into COSINE_SINE. */
int16_t cosine_func(int16_t value, uint8_t angle) {
    return cosine_sine(value, (uint8_t)((angle - 64) & 0xFF));
}
