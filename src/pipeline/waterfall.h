// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#ifdef __APPLE__
#include <Accelerate/Accelerate.h>
#endif

// Reuse the 40 BLE outputs of the existing 80-point FFT channelizer.
// Accumulate every complex output: no packet-sized burst is skipped by sampling.
class Waterfall {
  std::array<double, 40> energy{};
  unsigned count = 0;
  double start = 0;
public:
  static constexpr unsigned frames_per_row = 40000; // 20 ms at 2 Msps
  void reset() { energy.fill(0); count = 0; }
  template<class F> void run(const float *bins, double time, F emit) {
    if (!count) start = time;
    for (int ch = 0; ch < 40; ch++) {
      float i = bins[2 * ch], q = bins[2 * ch + 1];
      energy[ch] += double(i) * i + double(q) * q;
    }
    if (++count != frames_per_row) return;
    std::array<float, 40> db;
    for (int ch = 0; ch < 40; ch++)
      db[ch] = float(10 * std::log10(std::max(1e-12, energy[ch] / count / (8192.0 * 8192.0))));
    emit(start, time + .5e-6, db);
    reset();
  }
  // Walk each channel contiguously and preserve row boundaries. Accelerate's
  // vector reduction changes summation order only for display power; decoder
  // signal/noise measurements keep their double-precision sample history.
  template<class F> void batch(const float *bins, size_t frames, bool planar,
                              double time, F emit) {
    size_t at = 0;
    while (at < frames) {
      if (!count) start = time + at / 2e6;
      size_t take = std::min(frames - at, size_t(frames_per_row - count));
      for (unsigned ch = 0; ch < 40; ch++) {
        const float *p = planar ? bins + 2 * (ch * frames + at) : bins + 80 * at + 2 * ch;
        const size_t stride = planar ? 2 : 80;
#ifdef __APPLE__
        if (planar) {
          float sum;
          vDSP_svesq(p, 1, &sum, 2 * take);
          energy[ch] += sum;
          continue;
        }
#endif
        double sum = energy[ch];
        for (size_t k = 0; k < take; k++) {
          float i = p[k * stride], q = p[k * stride + 1];
          sum += double(i) * i + double(q) * q;
        }
        energy[ch] = sum;
      }
      count += unsigned(take);
      at += take;
      if (count == frames_per_row) {
        std::array<float, 40> db;
        for (int ch = 0; ch < 40; ch++)
          db[ch] = float(10 * std::log10(std::max(1e-12, energy[ch] / count / (8192.0 * 8192.0))));
        emit(start, time + at / 2e6, db);
        reset();
      }
    }
  }
};
