// SPDX-License-Identifier: MIT
#include "demod-fixture.h"
#include "streaming-demod.h"
int main() {
  const uint32_t aa = 0x506545ef, seed = reverse24(0x7e5f93);
  for (double gap_db : {0., 22., 30.})
    for (double snr_db : {6., 8., 10., 12.}) {
      int alone = 0, after = 0, centrals = 0;
      for (unsigned trial = 0; trial < 100; trial++) {
        auto central = waveform({1, 0}, -7000, .3, 0, 1, false, aa, seed, 16);
        auto peripheral = waveform({5, 0}, -25000, (trial % 10) / 10., 0, 1,
                                   false, aa, seed, 16);
        std::mt19937 rng(900 + trial);
        double sigma = 100 / std::sqrt(2 * std::pow(10., snr_db / 10));
        std::normal_distribution<float> noise(0, float(sigma));
        std::vector<std::array<float, 2>> iq(6000);
        for (auto &x : iq)
          x = {noise(rng), noise(rng)};
        for (size_t k = 4000; k < 4300; k++)
          for (int q = 0; q < 2; q++)
            iq[k + 460][q] += peripheral[k][q];
        for (bool strong : {false, true}) {
          Registry r;
          r.remember(aa, {seed, 3, 0, true});
          bool received = false;
          StreamingDecoder d(16, r, [&](const Packet &p) {
            if (p.pdu == std::vector<uint8_t>{5, 0})
              received = true;
            if (strong && p.pdu == std::vector<uint8_t>{1, 0})
              centrals++;
          });
          for (size_t k = 0; k < iq.size(); k++) {
            float i = iq[k][0], q = iq[k][1];
            if (strong && k >= 4000 && k < 4300) {
              i += central[k][0] * std::pow(10., gap_db / 20);
              q += central[k][1] * std::pow(10., gap_db / 20);
            }
            d.run(i, q);
          }
          if (strong)
            after += received;
          else
            alone += received;
        }
      }
      assert(after >= alone - 3);
      if (gap_db >= 22)
        assert(centrals == 100);
      std::cout << "{\"packet_noise_exclusion\":" << 1
                << ",\"power_difference_db\":" << gap_db
                << ",\"snr_db\":" << snr_db << ",\"alone\":" << alone
                << ",\"after_strong\":" << after << ",\"trials\":100}\n";
    }
}
