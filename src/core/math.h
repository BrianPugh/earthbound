#ifndef CORE_MATH_H
#define CORE_MATH_H

#include "core/types.h"

typedef struct {
    uint16_t a;
    uint16_t b;
} RNGState;
extern RNGState rng_state;

/* Random number generator — port of assembly RAND (rand.asm).
 * Uses two 16-bit state variables (RAND_A, RAND_B) and the SNES
 * hardware 8x8 multiply with bit rotations. NOT a simple LCG. */
void rng_seed(uint32_t seed);
uint16_t rng_next(void);     /* Returns rng_next_byte() widened to 16-bit */
uint8_t rng_next_byte(void); /* Matches assembly RAND, returns 0-255 */

/* 16x16 -> 32 multiply */
uint32_t mul16(uint16_t a, uint16_t b);

/* 32/16 -> 16 divide with remainder */
uint16_t div32_16(uint32_t dividend, uint16_t divisor, uint16_t *remainder);

/* COSINE_SINE (asm/system/math/cosine_sine.asm)
 * Computes value * sine_table[angle] using SNES Mode 7 signed multiply.
 * angle is 0-255 (256 = full circle), sine_table is signed 8-bit.
 * Returns low 16 bits of the 24-bit product. */
int16_t cosine_sine(int16_t value, uint8_t angle);

/* COSINE — same as cosine_sine but with 90° phase shift (cos = sin(angle-64)). */
int16_t cosine_func(int16_t value, uint8_t angle);

#endif /* CORE_MATH_H */
