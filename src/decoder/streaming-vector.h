// SPDX-License-Identifier: MIT
#pragma once
#include <cstddef>
#include <cstdint>
#if defined(__aarch64__)
#include <arm_neon.h>
#endif

// Stateless I/Q arithmetic only. Preserve double-precision sample power and
// the scalar multiply/add order; don't change packet thresholds or noise state.
struct StreamingVector {
    static constexpr size_t capacity = 64;
    alignas(64) double energy[capacity];
    alignas(64) float real[capacity], imag[capacity];
#if defined(__aarch64__)
    // Compare four synchronized symbols at once. Angles start 64 samples
    // before the address end; 68 entries permit a padded final vector load.
    // Float interpolation/addition and double comparison match symbol().
    static unsigned address_errors(const float *angles, int phase, double offset, uint32_t aa) {
        const unsigned quarter = unsigned(phase + 6), start = quarter / 4;
        const float fraction = (quarter & 3) * .25f;
        const int32_t shifts[4] = {0, -1, -2, -3};
        const auto threshold = vdupq_n_f64(2 * offset);
        auto errors = vdupq_n_u32(0);
        for (unsigned k = 0; k < 32; k += 4) {
            auto samples = vld2q_f32(angles + start + 2 * k);
            auto first = samples.val[0], second = samples.val[1];
            if (fraction != 0) {
                auto following = vld2q_f32(angles + start + 2 * k + 2).val[0];
                first = vfmaq_n_f32(first, vsubq_f32(second, first), fraction);
                second = vfmaq_n_f32(second, vsubq_f32(following, second), fraction);
            }
            auto sum = vaddq_f32(first, second);
            auto low = vcgtq_f64(vcvt_f64_f32(vget_low_f32(sum)), threshold);
            auto high = vcgtq_f64(vcvt_f64_f32(vget_high_f32(sum)), threshold);
            auto received = vcombine_u32(vmovn_u64(low), vmovn_u64(high));
            auto bits = vandq_u32(vshlq_u32(vdupq_n_u32(aa >> k), vld1q_s32(shifts)), vdupq_n_u32(1));
            auto expected = vsubq_u32(vdupq_n_u32(0), bits);
            errors = vaddq_u32(errors, vshrq_n_u32(veorq_u32(received, expected), 31));
        }
        return vaddvq_u32(errors);
    }
#endif
    void prepare(const float *iq, size_t n, float previous_i, float previous_q) {
        size_t k = 0;
#if defined(__aarch64__)
        float32x4_t pi = vdupq_n_f32(previous_i), pq = vdupq_n_f32(previous_q);
        for (; k + 4 <= n; k += 4) {
            const auto x = vld2q_f32(iq + 2 * k);
            auto a = vextq_f32(pi, x.val[0], 3), b = vextq_f32(pq, x.val[1], 3);
            vst1q_f32(real + k, vfmaq_f32(vmulq_f32(b, x.val[1]), a, x.val[0]));
            vst1q_f32(imag + k, vfmaq_f32(vnegq_f32(vmulq_f32(b, x.val[0])), a, x.val[1]));
            auto il = vcvt_f64_f32(vget_low_f32(x.val[0])), ql = vcvt_f64_f32(vget_low_f32(x.val[1]));
            auto ih = vcvt_f64_f32(vget_high_f32(x.val[0])), qh = vcvt_f64_f32(vget_high_f32(x.val[1]));
            vst1q_f64(energy + k, vaddq_f64(vmulq_f64(il, il), vmulq_f64(ql, ql)));
            vst1q_f64(energy + k + 2, vaddq_f64(vmulq_f64(ih, ih), vmulq_f64(qh, qh)));
            pi = x.val[0]; pq = x.val[1];
        }
        if (k) { previous_i = iq[2*k-2]; previous_q = iq[2*k-1]; }
#endif
        for (; k < n; ++k) {
            float i = iq[2*k], q = iq[2*k+1];
            energy[k] = double(i)*i + double(q)*q;
            real[k] = previous_i*i + previous_q*q;
            imag[k] = previous_i*q - previous_q*i;
            previous_i = i; previous_q = q;
        }
    }
};
