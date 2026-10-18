// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#if defined(__aarch64__)
#include <arm_neon.h>
#endif

// Delay noise learning until acquisition can identify the preamble/address.
// Otherwise freezing only during payload decode still admits the strongest
// part of the burst before synchronization. Each power value is committed at
// most once; this does not replay any IQ or payload decoding.
class PacketNoise {
  static constexpr uint64_t delay = 256; // 128 us: leading carrier plus acquisition latency
  std::array<double, delay> powers{};
  uint64_t filled = 0, first = 0, last = 0;
  bool excluding = false;
  // Separate display measurement; the learning filters below use every
  // accepted sample regardless of this window length.
  // Require a complete contiguous 32 us window of accepted noise samples.
  std::array<double, 64> recent{};
  unsigned recent_count = 0, recent_index = 0;

  std::array<double, 2> learned{{0, 1}};

public:
  double background() const { return learned[0]; }
  double floor() const { return learned[1]; }
  uint64_t included = 0, excluded = 0;
  void reset() { *this = PacketNoise{}; }
  double measured_power() const {
    if (recent_count != recent.size())
      return std::numeric_limits<double>::quiet_NaN();
    // This display measurement is requested only at packet acquisition.
    // Reduce the window here instead of maintaining a serial running sum at
    // 2 Msps. Acquisition thresholds use the independent learned filters.
#if defined(__aarch64__)
    auto a = vdupq_n_f64(0), b = a;
    for (unsigned k = 0; k < recent.size(); k += 4) {
      a = vaddq_f64(a, vld1q_f64(recent.data() + k));
      b = vaddq_f64(b, vld1q_f64(recent.data() + k + 2));
    }
    return vaddvq_f64(vaddq_f64(a, b)) / recent.size();
#else
    double sum = 0;
    for (double value : recent) sum += value;
    return sum / recent.size();
#endif
  }
  void packet(uint64_t begin, uint64_t now) {
    // Merge overlapping queued exclusions (including very short failed frames).
    if (!excluding || (now > last && now - last > delay))
      first = begin;
    else
      first = std::min(first, begin);
    last = now;
    excluding = true;
  }
  void observe(double power, uint64_t now, bool decoding) {
    if (decoding)
      last = now + 8; // guard the decision/filter tail by 4 us
    double delayed = powers[now & (delay - 1)];
    powers[now & (delay - 1)] = power;
    if (filled < delay) {
      ++filled;
      return;
    }
    uint64_t at = now - delay;
    if (excluding && at >= first && at <= last) {
      recent_count = recent_index = 0;
      ++excluded;
      return;
    }
#if defined(__aarch64__)
    // Independent background/floor recurrences share a two-lane update.
    // Preserve sample order and the scalar FMA in each lane.
    auto state = vld1q_f64(learned.data());
    auto value = vdupq_n_f64(delayed);
    const double fast[2] = {.0001, .001}, slow[2] = {.0001, .00001};
    auto rate = vbslq_f64(vcltq_f64(value, state), vld1q_f64(fast), vld1q_f64(slow));
    vst1q_f64(learned.data(), vfmaq_f64(state, rate, vsubq_f64(value, state)));
#else
    learned[0] += .0001 * (delayed - learned[0]);
    learned[1] += (delayed < learned[1] ? .001 : .00001) * (delayed - learned[1]);
#endif
    if (recent_count < recent.size())
      ++recent_count;
    recent[recent_index] = delayed;
    recent_index = (recent_index + 1) & 63;
    ++included;
  }
};
