#include "core/decomp.h"
#include <string.h>

/*
 * EarthBound decompression algorithm.
 * Port of asm/system/decomp.asm (DECOMP function).
 *
 * Command byte format:
 *   Bits 7-5 = operation type
 *   Bits 4-0 = length (for non-extended commands)
 *
 * If bits 7-5 == 0x07 (0xE0 masked): extended format
 *   - Bits 4-2 of command byte, shifted left 3, become the real operation type
 *   - Bits 1-0 of command byte are extra offset bits (DECOMP_TEMP_UNREAD)
 *   - Next byte is the length
 *   - Length is incremented by 1
 *
 * Operations (after masking bits 7-5):
 *   0x00: Copy N bytes verbatim from stream
 *   0x20: Fill N bytes with single byte from stream
 *   0x40: Fill N words with single word from stream
 *   0x60: Fill N bytes with incrementing byte from stream
 *   0x80: Copy N bytes from previously decompressed data (forward)
 *   0xA0: Copy N bytes with bit-reversal from decompressed data
 *   0xC0: Copy N bytes backwards from decompressed data
 *
 * Stream terminator: 0xFF
 */

/* Reverse bits of a byte (for the 0xA0 operation) */
static uint8_t reverse_bits(uint8_t b) {
    /* The ASM does 8 iterations of: ASL temp, ROR accum
       which shifts bits from temp into accum through carry,
       effectively reversing the bit order */
    uint8_t result = 0;
    for (int i = 0; i < 8; i++) {
        result >>= 1;
        if (b & 0x80) result |= 0x80;
        b <<= 1;
    }
    return result;
}

size_t decomp(const uint8_t *src, size_t src_size, uint8_t *dst, size_t dst_max) {
    DecompStream s;
    decomp_stream_init(&s, src, src_size, dst, dst_max);
    return decomp_stream_advance(&s, dst_max);
}

/* ---- Streaming decompression ---- */

void decomp_stream_init(DecompStream *s, const uint8_t *src, size_t src_size,
                        uint8_t *dst, size_t dst_max) {
    s->src = src;
    s->src_size = src_size;
    s->si = 0;
    s->dst = dst;
    s->di = 0;
    s->dst_max = dst_max;
    s->done = false;
}

size_t decomp_stream_advance(DecompStream *s, size_t target) {
    if (s->done || !s->src) return s->di;
    if (target > s->dst_max) target = s->dst_max;

    const uint8_t *src = s->src;
    size_t src_size = s->src_size;
    uint8_t *dst = s->dst;
    size_t si = s->si;
    size_t di = s->di;
    size_t dst_max = s->dst_max;

    while (si < src_size && di < target) {
        uint8_t cmd = src[si];
        if (cmd == 0xFF) { s->done = true; break; }

        uint8_t op;
        uint16_t length;

        if ((cmd & 0xE0) == 0xE0) {
            op = (cmd << 3) & 0xE0;
            uint8_t len_hi = cmd & 0x03;
            si++;
            if (si >= src_size) { s->done = true; break; }
            length = ((uint16_t)len_hi << 8) | src[si];
            length += 1;
            si++;
        } else {
            op = cmd & 0xE0;
            length = (cmd & 0x1F) + 1;
            si++;
        }

        switch (op) {
        case 0x00:
            for (uint16_t i = 0; i < length; i++) {
                if (si >= src_size || di >= dst_max) goto stream_done;
                dst[di++] = src[si++];
            }
            break;
        case 0x20:
            if (si >= src_size) goto stream_done;
            { uint8_t fill = src[si++];
              for (uint16_t i = 0; i < length; i++) {
                  if (di >= dst_max) goto stream_done;
                  dst[di++] = fill;
              }
            }
            break;
        case 0x40:
            if (si + 1 >= src_size) goto stream_done;
            { uint8_t lo = src[si++]; uint8_t hi = src[si++];
              for (uint16_t i = 0; i < length; i++) {
                  if (di + 1 >= dst_max) goto stream_done;
                  dst[di++] = lo; dst[di++] = hi;
              }
            }
            break;
        case 0x60:
            if (si >= src_size) goto stream_done;
            { uint8_t val = src[si++];
              for (uint16_t i = 0; i < length; i++) {
                  if (di >= dst_max) goto stream_done;
                  dst[di++] = val++;
              }
            }
            break;
        case 0x80:
        default:
            if (si + 1 >= src_size) goto stream_done;
            { uint16_t offset = ((uint16_t)src[si] << 8) | src[si + 1]; si += 2;
              size_t ref = (size_t)offset;
              for (uint16_t i = 0; i < length; i++) {
                  if (di >= dst_max) goto stream_done;
                  uint8_t b = (ref < di) ? dst[ref] : 0;
                  dst[di++] = b;
                  ref++;
              }
            }
            break;
        case 0xA0:
            if (si + 1 >= src_size) goto stream_done;
            { uint16_t offset = ((uint16_t)src[si] << 8) | src[si + 1]; si += 2;
              size_t ref = (size_t)offset;
              for (uint16_t i = 0; i < length; i++) {
                  if (di >= dst_max) goto stream_done;
                  uint8_t b = (ref < di) ? dst[ref] : 0;
                  dst[di++] = reverse_bits(b);
                  ref++;
              }
            }
            break;
        case 0xC0:
            if (si + 1 >= src_size) goto stream_done;
            { uint16_t offset = ((uint16_t)src[si] << 8) | src[si + 1]; si += 2;
              size_t ref = (size_t)offset;
              for (uint16_t i = 0; i < length; i++) {
                  if (di >= dst_max) goto stream_done;
                  uint8_t b = (ref < di) ? dst[ref] : 0;
                  dst[di++] = b;
                  if (ref > 0) ref--;
              }
            }
            break;
        }
    }

stream_done:
    s->si = si;
    s->di = di;
    if (di >= dst_max) s->done = true;
    return di;
}

