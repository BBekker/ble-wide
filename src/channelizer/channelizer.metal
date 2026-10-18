// SPDX-License-Identifier: MIT
#include <metal_stdlib>
using namespace metal;

// One group per output time. The first 80 lanes compute polyphase FIR sums;
// An 80-point mixed-radix (5 x 16) inverse transform emits 40 BLE bins. Tables are identical
// to the CPU path. Full preceding history is supplied by each independent job.
kernel void channelize(device const short2 *iq [[buffer(0)]],
                       device float2 *out [[buffer(1)]],
                       constant float *h [[buffer(2)]],
                       constant float2 *osc [[buffer(3)]],
                       constant float2 *twiddle [[buffer(4)]],
                       constant float2 *roots5 [[buffer(5)]],
                       constant uint *ble_bins [[buffer(6)]],
                       constant uint2 &layout [[buffer(7)]],
                       uint frame [[threadgroup_position_in_grid]],
                       uint lane [[thread_index_in_threadgroup]]) {
    threadgroup float2 poly[80], stage[80];
    if (lane < 80) {
        float2 sum = 0;
        for (uint t = 0; t < 16; ++t) {
            uint k = 1280 + frame * 80 + 79 - lane - t * 80;
            float2 x = float2(iq[k]);
            float2 o = osc[k % 160];
            float2 mixed = float2(x.x * o.x - x.y * o.y, x.x * o.y + x.y * o.x);
            sum += mixed * h[t * 80 + lane];
        }
        poly[lane] = sum;
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
    if (lane < 80) {
        uint q = lane / 16, r = lane % 16;
        float2 sum = 0;
        for (uint s = 0; s < 5; ++s) {
            float2 x = poly[r + 16*s], w = roots5[q*5+s];
            sum += float2(x.x*w.x - x.y*w.y, x.x*w.y + x.y*w.x);
        }
        stage[lane] = sum;
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
    if (lane < 40) {
        uint q = ble_bins[lane] % 5;
        float2 sum = 0;
        for (uint p = 0; p < 16; ++p) {
            float2 x = stage[q*16+p], w = twiddle[p * 40 + lane];
            sum += float2(x.x * w.x - x.y * w.y, x.x * w.y + x.y * w.x);
        }
        // Separate channel streams are written in the transform itself.
        // This avoids an intermediate buffer and another GPU dispatch.
        out[layout.y ? lane * layout.x + frame : frame * 40 + lane] = sum;
    }
}

