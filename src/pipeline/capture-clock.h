// SPDX-License-Identifier: MIT
#pragma once
#include <cstddef>
#include <cstdint>

// Decoder counters count emitted 2 Msps samples. This clock also accounts for
// missing raw input and partial 80-sample frames discarded at segment resets.
// Call segment() only after flushing all queued pipeline output.
class CaptureClock {
  uint64_t raw = 0, output = 0, base_raw = 0, base_output = 0;
  bool fresh = true;
public:
  void segment() { fresh = true; }
  void input(size_t n) {
    if (fresh) {
      base_raw = raw;
      base_output = output;
      fresh = false;
    }
    raw += n;
  }
  void missing(uint64_t n) { raw += n; }
  void frame(size_t n = 1) { output += n; }
  double frame_time() const { return base_raw / 160e6 + (output - base_output) / 2e6; }
  double now() const { return raw / 160e6; }
  double packet(uint64_t sample) const {
    // Decoder sample marks the access-address start. Subtract the 8 us
    // preamble; the common FIR/demodulation delay cancels in interval timing.
    return base_raw / 160e6 + (double(sample) - base_output) / 2e6 - 8e-6;
  }
};
