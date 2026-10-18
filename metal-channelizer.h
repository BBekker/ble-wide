// SPDX-License-Identifier: MIT
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Input includes 1280 preceding samples. Default output is 40 contiguous
// complex channel arrays, each (samples - 1280) / 80 samples long. The optional
// frame-major layout is retained for numerical and performance comparisons.
class MetalChannelizer {
  struct Impl;
  std::unique_ptr<Impl> impl;

public:
  MetalChannelizer();
  ~MetalChannelizer();
  MetalChannelizer(const MetalChannelizer &) = delete;
  void run(const int16_t *iq, size_t samples, std::vector<float> &bins, bool planar = true);
  double gpu_seconds() const;
  std::string device_name() const;
};
