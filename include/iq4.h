/* SPDX-License-Identifier: MIT */
/* Experimental IQ41 wire format, little endian:
 * u32 magic, u16 scale_shift (0..11), u16 samples (2048), u64 DMA sequence,
 * then one byte/sample: signed I in high nibble, signed Q in low nibble.
 * Reconstruct each component as signed_nibble * 2^scale_shift, clamped to
 * signed 14 bits. Firmware events retain the existing SDR1 header/format.
 * One scale per block preserves physical amplitude across scale changes.
 */
#ifndef W1700K_IQ4_H
#define W1700K_IQ4_H
#include <stddef.h>
#include <stdint.h>
#ifdef __ARM_NEON
#include <arm_neon.h>
#endif
#define IQ4_MAGIC 0x31345149u
#define IQ4_SAMPLES 2048u

static inline int iq4_signed14(unsigned v) { return (int)(v & 0x1fff) - (int)(v & 0x2000); }

static inline unsigned iq_peak(const uint32_t *src, size_t n) {
    unsigned peak = 0;
    size_t k = 0;
#ifdef __ARM_NEON
    int16x8_t peaks = vdupq_n_s16(0);
    for (; k + 4 <= n; k += 4) {
        int16x8_t x = vreinterpretq_s16_u32(vld1q_u32(src + k));
        x = vshrq_n_s16(vshlq_n_s16(x, 2), 2);
        peaks = vmaxq_s16(peaks, vabsq_s16(x));
    }
    peak = (unsigned)vmaxvq_s16(peaks);
#endif
    for (; k < n; ++k) {
        int i = iq4_signed14(src[k] >> 16), q = iq4_signed14(src[k]);
        unsigned a = (unsigned)(i < 0 ? -i : i), b = (unsigned)(q < 0 ? -q : q);
        if (a > peak)
            peak = a;
        if (b > peak)
            peak = b;
    }
    return peak;
}

static inline unsigned iq4_scale(const uint32_t *src, size_t n) {
    unsigned peak = iq_peak(src, n), shift = 0;
    while (shift < 11 && ((peak + (shift ? 1u << (shift - 1) : 0)) >> shift) > 7)
        ++shift;
    return shift;
}

static inline int iq4_round(int x, unsigned shift) {
    /* Nearest, ties toward positive infinity, matching NEON vrshl. */
    if (shift) {
        x += 1 << (shift - 1);
        x = x >= 0 ? x >> shift : -((-x + (1 << shift) - 1) >> shift);
    }
    return x < -8 ? -8 : x > 7 ? 7 : x;
}

static inline void iq4_pack(const uint32_t *src, uint8_t *dst, size_t n, unsigned shift) {
    size_t k = 0;
#ifdef __ARM_NEON
    const int16x8_t sh = vdupq_n_s16(-(int)shift);
    for (; k + 8 <= n; k += 8) {
        /* Raw halfwords are Q,I. Deinterleave before narrowing to nibbles. */
        uint16x8x2_t pair = vld2q_u16((const uint16_t *)(src + k));
        int16x8_t q = vshrq_n_s16(vshlq_n_s16(vreinterpretq_s16_u16(pair.val[0]), 2), 2);
        int16x8_t i = vshrq_n_s16(vshlq_n_s16(vreinterpretq_s16_u16(pair.val[1]), 2), 2);
        q = vmaxq_s16(vdupq_n_s16(-8), vminq_s16(vdupq_n_s16(7), vrshlq_s16(q, sh)));
        i = vmaxq_s16(vdupq_n_s16(-8), vminq_s16(vdupq_n_s16(7), vrshlq_s16(i, sh)));
        uint8x8_t qb = vand_u8(vreinterpret_u8_s8(vmovn_s16(q)), vdup_n_u8(15));
        uint8x8_t ib = vshl_n_u8(vreinterpret_u8_s8(vmovn_s16(i)), 4);
        vst1_u8(dst + k, vorr_u8(ib, qb));
    }
#endif
    for (; k < n; ++k)
        dst[k] = (uint8_t)(((unsigned)iq4_round(iq4_signed14(src[k] >> 16), shift) & 15) << 4 |
                           ((unsigned)iq4_round(iq4_signed14(src[k]), shift) & 15));
}

static inline uint32_t iq4_unpack(uint8_t b, unsigned shift) {
    int i = ((int)(b >> 4) ^ 8) - 8, q = ((int)(b & 15) ^ 8) - 8;
    i *= 1 << shift;
    q *= 1 << shift;
    i = i < -8192 ? -8192 : i > 8191 ? 8191 : i;
    q = q < -8192 ? -8192 : q > 8191 ? 8191 : q;
    return ((uint32_t)i & 0x3fff) << 16 | ((uint32_t)q & 0x3fff);
}
#endif
