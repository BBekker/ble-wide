// SPDX-License-Identifier: MIT
#include "channelizer.h"
#include "metal-channelizer.h"
#include <cassert>
#include <cstdio>
#include <random>
#include <vector>

int main() {
  MetalChannelizer gpu;
  std::mt19937 rng(571);
  double worst = 0;
  // Full/partial jobs, reused buffers, silence, impulse, quantized wideband
  // noise.
  for (size_t n :
       {size_t(80), size_t(31*80), size_t(32*80), size_t(33*80), size_t(2048), size_t(655360), size_t(131), size_t(0)}) {
    for (int pattern = 0; pattern < 3; ++pattern) {
      std::vector<int16_t> iq(2 * (1280 + n));
      if (pattern == 1 && n)
        iq[2 * 1280] = 8191;
      if (pattern == 2)
        for (auto &x : iq)
          x = (int(rng() % 128) - 64) * 32;
      Channelizer cpu;
      std::vector<float> ref, got;
      size_t frame = 0;
      cpu.run(iq.data(), iq.size() / 2, [&](const fftwf_complex *v) {
        if (frame++ < 16)
          return;
        for (int ch = 0; ch < 40; ++ch) {
          ref.push_back(v[Channelizer::bin(ch)][0]);
          ref.push_back(v[Channelizer::bin(ch)][1]);
        }
      });
      std::vector<float> planar;
      gpu.run(iq.data(), iq.size() / 2, got, false);
      gpu.run(iq.data(), iq.size() / 2, planar, true);
      assert(planar.size() == got.size());
      size_t frames = got.size() / 80;
      for (size_t t = 0; t < frames; t++)
        for (size_t ch = 0; ch < 40; ch++)
          for (size_t q = 0; q < 2; q++)
            assert(planar[2*(ch*frames+t)+q] == got[80*t+2*ch+q]);
      assert(got.size() == ref.size());
      for (size_t k = 0; k < ref.size(); ++k) {
        double err = std::abs(double(got[k]) - ref[k]);
        worst = std::max(worst, err);
        // Single-precision FFT operation order differs; no gain/phase tolerance
        // shortcut.
        if (err > .002 + 2e-6 * std::abs(ref[k])) {
          std::fprintf(stderr,
                       "n=%zu pattern=%d bin=%zu cpu=%g gpu=%g error=%g\n", n,
                       pattern, k, ref[k], got[k], err);
          return 1;
        }
      }
    }
  }
  std::printf("Metal/CPU filter equivalence passed; max absolute error %.9g\n",
              worst);
}
