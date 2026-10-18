// SPDX-License-Identifier: MIT
#include "waterfall.h"
#include <cassert>
#include <random>
#include <vector>

struct Row { double start, end; std::array<float, 40> db; };
int main() {
  std::mt19937 rng(571);
  for (bool planar : {false, true}) {
    Waterfall reference, batched;
    std::vector<Row> a, b;
    size_t time = 0;
    auto save_a = [&](double x, double y, const auto &db) { a.push_back({x,y,db}); };
    auto save_b = [&](double x, double y, const auto &db) { b.push_back({x,y,db}); };
    for (unsigned epoch = 0; epoch < 2; epoch++) {
      reference.reset(); batched.reset(); time += 517;
      for (size_t frames : {size_t(0), size_t(1), size_t(8192), size_t(39999), size_t(83001), size_t(25)}) {
        std::vector<float> interleaved(frames*80), bins(frames*80);
        for (auto &x : interleaved) x = (int(rng()%16000)-8000) / 17.f;
        for (size_t t = 0; t < frames; t++) {
          for (size_t ch = 0; ch < 40; ch++)
            for (size_t q = 0; q < 2; q++)
              bins[planar ? 2*(ch*frames+t)+q : 80*t+2*ch+q] = interleaved[80*t+2*ch+q];
          reference.run(interleaved.data()+80*t, (time+t)/2e6, save_a);
        }
        batched.batch(bins.data(), frames, planar, time/2e6, save_b);
        time += frames;
      }
    }
    assert(a.size() == b.size() && !a.empty());
    for (size_t k = 0; k < a.size(); k++) {
      assert(std::abs(a[k].start-b[k].start) < 1e-12);
      assert(std::abs(a[k].end-b[k].end) < 1e-12);
      // Vector display-power reductions may sum in a different order. This
      // tolerance is far below the viewer's 0.1 dB output resolution.
      for (unsigned ch = 0; ch < 40; ++ch)
        assert(std::abs(a[k].db[ch] - b[k].db[ch]) < 0.0001f);
    }
  }
}
