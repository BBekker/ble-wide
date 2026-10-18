// SPDX-License-Identifier: MIT
// Paired AWGN experiment. SNR is 100^2/(2*sigma^2) at the 2-Msps input;
// it is not calibrated receiver sensitivity in dBm.
#include "demod-fixture.h"
#include "streaming-demod.h"

int main() {
    for (double snr : {4., 6., 8., 10., 12., 14.}) {
        unsigned baseline = 0, streaming = 0, both = 0;
        for (unsigned trial = 0; trial < 80; trial++) {
            bool adv = !(trial & 1);
            uint32_t aa = adv ? 0x8e89bed6 : 0x12345679;
            int channel = adv ? 37 + trial % 3 : trial % 37;
            unsigned len = adv ? 20 : 40;
            std::vector<uint8_t> pdu = {uint8_t(adv ? 0x40 : 2), uint8_t(len)};
            std::mt19937 rng(30000 + trial);
            for (unsigned k = 0; k < len; k++) pdu.push_back(uint8_t(rng()));
            double cfo = (int(trial % 5) - 2) * 60000.;
            double timing = ((trial * 37) % 101) / 101.;
            double sigma = 100 / std::sqrt(2 * std::pow(10., snr / 10));
            auto iq = waveform(pdu, cfo, timing, sigma, 10000 + trial, false,
                               aa, adv ? 0xaaaaaa : 0x654321, channel);
            Registry a, b;
            if (!adv) { a.remember(aa, {0x654321, 3, 0, true}); b.remember(aa, {0x654321, 3, 0, true}); }
            unsigned got_a = 0, got_b = 0;
            Decoder old(channel, a, [&](const Packet &p) {
                assert(p.aa == aa && p.pdu == pdu); got_a++;
            }, true);
            StreamingDecoder single(channel, b, [&](const Packet &p) {
                assert(p.aa == aa && p.pdu == pdu); got_b++;
            });
            for (auto x : iq) { old.run(x[0], x[1]); single.run(x[0], x[1]); }
            assert(got_a <= 1 && got_b <= 1);
            assert(single.recovery_attempts == 0);
            baseline += got_a; streaming += got_b; both += got_a && got_b;
        }
        if (snr == 14) assert(baseline == 80 && streaming == 80);
        std::cout << "{\"snr_db\":" << snr << ",\"trials\":80,\"adaptive\":"
                  << baseline << ",\"streaming\":" << streaming
                  << ",\"both\":" << both << "}\n";
    }
}
