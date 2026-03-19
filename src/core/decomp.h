#ifndef CORE_DECOMP_H
#define CORE_DECOMP_H

#include "core/types.h"

/* Decompress LZHAL-format data.
   src: pointer to compressed data
   src_size: size of compressed data buffer
   dst: pointer to output buffer (must be large enough)
   Returns: number of bytes written to dst, or 0 on error */
size_t decomp(const uint8_t *src, size_t src_size, uint8_t *dst, size_t dst_max);

/* ---- Streaming decompression ----
 *
 * Allows incremental decompression: decompress N bytes at a time, resuming
 * from where the previous call left off. The output buffer must remain valid
 * between calls (the decompressor back-references prior output by absolute
 * offset).
 *
 * Usage:
 *   DecompStream stream;
 *   decomp_stream_init(&stream, src, src_size, dst, dst_max);
 *   while (stream.di < needed) {
 *       decomp_stream_advance(&stream, stream.di + chunk_size);
 *   }
 *   // dst[0..stream.di-1] contains decompressed data
 */
typedef struct {
    const uint8_t *src;
    size_t src_size;
    size_t si;       /* current read position in compressed stream */
    uint8_t *dst;
    size_t di;       /* current write position (total bytes decompressed) */
    size_t dst_max;  /* maximum output size */
    bool done;       /* true when stream is exhausted or hit terminator */
} DecompStream;

/* Initialize a streaming decompression context. */
void decomp_stream_init(DecompStream *s, const uint8_t *src, size_t src_size,
                        uint8_t *dst, size_t dst_max);

/* Decompress until di >= target or the stream ends.
 * Returns the new di (total bytes decompressed so far). */
size_t decomp_stream_advance(DecompStream *s, size_t target);

#endif /* CORE_DECOMP_H */
