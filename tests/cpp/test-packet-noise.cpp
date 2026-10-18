// SPDX-License-Identifier: MIT
#include "decoder/packet-noise.h"
#include <cassert>
#include <cmath>
#include <random>
#include <vector>
int main() {
  PacketNoise n;
  for (uint64_t base : {uint64_t(0), uint64_t(10003)}) {
    n.reset();
    double background = 0, floor = 1;
    uint64_t accepted = 0, rejected = 0;
    for (uint64_t k = 0; k < 6500; k++) {
      if (k == 4100)
        n.packet(base + 4000, base + k);
      bool burst = k >= 4000 && k <= 6008;
      n.observe(burst ? 1e10 : 100 + k % 4, base + k, k >= 4100 && k <= 6000);
      if (k < 256)
        continue;
      uint64_t at = k - 256;
      if (at >= 4000 && at <= 6008) {
        rejected++;
        continue;
      }
      double e = 100 + at % 4;
      background += .0001 * (e - background);
      floor += (e < floor ? .001 : .00001) * (e - floor);
      accepted++;
      assert(n.background() == background && n.floor() == floor);
    }
    assert(n.included == accepted && n.excluded == rejected &&
           rejected == 2009);
  }
  // Actual noise changes must still be learned, rather than freezing forever.
  n.reset();
  for (uint64_t k = 0; k < 200000; k++)
    n.observe(k < 100000 ? 100 : 1000, k, false);
  assert(n.background() > 999 && n.excluded == 0);
  // A full, recent window is required; neither warm-up zeros nor packet
  // samples may contribute to the displayed background measurement.
  n.reset();
  assert(std::isnan(n.measured_power()));
  for (uint64_t k = 0; k < 319; ++k) n.observe(100, k, false);
  assert(std::isnan(n.measured_power()));
  n.observe(100, 319, false);
  assert(n.measured_power() == 100);
  // The carrier starts 80 us before sync; its power must never be learned.
  for (uint64_t k = 320; k < 1000; ++k) {
    if (k == 560) n.packet(400, k);
    n.observe(k >= 400 && k <= 608 ? 1e10 : 100, k, k >= 560 && k <= 600);
    if (k >= 656 && k < 928) assert(std::isnan(n.measured_power()));
  }
  assert(n.measured_power() == 100);
  // Once a new 32 us window has passed through the delay, a step in real
  // background power must be reflected without the old 5 ms smoothing lag.
  for (uint64_t k = 1000; k < 1320; ++k) n.observe(10, k, false);
  assert(n.measured_power() == 10);
  n.reset();
  assert(std::isnan(n.measured_power()));
  // The display-only vector reduction can change summation order, but must
  // use exactly the latest 64 accepted samples after the 256-sample delay.
  std::mt19937 rng(781);
  std::vector<double> powers;
  for (unsigned k = 0; k < 10000; ++k) {
    powers.push_back(std::ldexp(double(1 + rng()%100000), int(rng()%20)-10));
    n.observe(powers.back(),k,false);
    if (k < 319) continue;
    long double sum = 0;
    for (unsigned at = k-319; at <= k-256; ++at) sum += powers[at];
    double expected = double(sum/64);
    assert(std::abs(n.measured_power()-expected) <= expected*1e-14);
  }
}
