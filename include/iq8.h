/* SPDX-License-Identifier: MIT */
/* IQ81: same 16-byte little-endian header as IQ41, shift 0..7,
 * 2048 samples, then signed I, Q bytes (4096 bytes per datagram).
 * UDP may carry one record (4112 bytes) or two concatenated records (8224).
 * Each retains its own sequence/scale; sender workers may use interleaved
 * sequences. Two records require an IPv4 MTU of 8252 bytes. Firmware events
 * remain separate SDR1 datagrams. Validate both headers before accepting a pair.
 * Scale is shared by a block, preserving relative amplitude on reconstruction.
 */
#ifndef W1700K_IQ8_H
#define W1700K_IQ8_H
#include "iq4.h"
#define IQ8_MAGIC 0x31385149u
static inline unsigned iq8_scale(const uint32_t *src, size_t n) {
    unsigned peak = iq_peak(src, n), shift = 0;
    while (shift < 7 && ((peak + (shift ? 1u << (shift - 1) : 0)) >> shift) > 127)
        ++shift;
    return shift;
}
static inline int iq8_round(int x, unsigned shift) {
    if (shift) {
        x += 1 << (shift - 1);
        x = x >= 0 ? x >> shift : -((-x + (1 << shift) - 1) >> shift);
    }
    return x < -128 ? -128 : x > 127 ? 127 : x;
}
static inline void iq8_pack(const uint32_t *src, uint8_t *dst, size_t n, unsigned shift) {
    size_t k = 0;
#ifdef __ARM_NEON
    const int16x8_t sh = vdupq_n_s16(-(int)shift);
    for (; k + 8 <= n; k += 8) {
        uint16x8x2_t pair = vld2q_u16((const uint16_t *)(src + k));
        int16x8_t q = vshrq_n_s16(vshlq_n_s16(vreinterpretq_s16_u16(pair.val[0]), 2), 2);
        int16x8_t i = vshrq_n_s16(vshlq_n_s16(vreinterpretq_s16_u16(pair.val[1]), 2), 2);
        int8x8x2_t out;
        out.val[0] = vqmovn_s16(vrshlq_s16(i, sh));
        out.val[1] = vqmovn_s16(vrshlq_s16(q, sh));
        vst2_s8((int8_t *)(dst + 2*k), out);
    }
#endif
    for (; k < n; ++k) {
        dst[2*k] = (uint8_t)iq8_round(iq4_signed14(src[k] >> 16), shift);
        dst[2*k+1] = (uint8_t)iq8_round(iq4_signed14(src[k]), shift);
    }
}
static inline uint32_t iq8_unpack(const uint8_t *b, unsigned shift) {
    int i = ((int)(b[0] ^ 128) - 128) * (1 << shift);
    int q = ((int)(b[1] ^ 128) - 128) * (1 << shift);
    i = i < -8192 ? -8192 : i > 8191 ? 8191 : i;
    q = q < -8192 ? -8192 : q > 8191 ? 8191 : q;
    return ((uint32_t)i & 0x3fff) << 16 | ((uint32_t)q & 0x3fff);
}
/* Same raw-word layout as iq8_unpack, eight complex samples per vector group.
 * Widen before scaling; shift 7 can exceed the signed 14-bit range, so retain
 * the scalar decoder's clipping (including -128 and +127 at every scale). */
static inline void iq8_unpack_block(const uint8_t *src, uint32_t *dst, size_t n, unsigned shift) {
    size_t k = 0;
#ifdef __ARM_NEON
    const int16x8_t sh = vdupq_n_s16((int)shift);
    const int16x8_t lo = vdupq_n_s16(-8192), hi = vdupq_n_s16(8191);
    const uint16x8_t mask = vdupq_n_u16(0x3fff);
    for (; k + 8 <= n; k += 8) {
        int8x8x2_t pair = vld2_s8((const int8_t *)(src + 2*k));
        int16x8_t i = vminq_s16(hi, vmaxq_s16(lo, vshlq_s16(vmovl_s8(pair.val[0]), sh)));
        int16x8_t q = vminq_s16(hi, vmaxq_s16(lo, vshlq_s16(vmovl_s8(pair.val[1]), sh)));
        uint16x8x2_t out;
        out.val[0] = vandq_u16(vreinterpretq_u16_s16(q), mask);
        out.val[1] = vandq_u16(vreinterpretq_u16_s16(i), mask);
        vst2q_u16((uint16_t *)(dst + k), out);
    }
#endif
    for (; k < n; ++k) dst[k] = iq8_unpack(src + 2*k, shift);
}
#endif
